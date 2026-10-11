#pragma once

// Optional, real OpenCL 1.2 device execution path. This header is compiled
// only when OpenCL headers and an ICD loader are installed. GPU selection is
// explicit and fails closed: it NEVER switches to CPU without a caller opt-in.
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

#include "oreslang/runtime/DopKernelIR.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::gpu {

inline void cl_require(cl_int result, const char* operation) {
    if (result != CL_SUCCESS)
        throw std::runtime_error(std::string("OpenCL ") + operation + " failed: "
                + std::to_string(result));
}

struct OpenClDeviceSelector {
    cl_device_type required_type = CL_DEVICE_TYPE_GPU;
    std::string name_contains;
};

struct OpenClResult {
    std::string device_name;
    cl_device_type selected_device_type;
    std::vector<std::uint8_t> output;
};

/**
 * A concrete OpenCL driver adapter for one typed f32 DOP kernel. This uses
 * real clCreateProgram, clBuildProgram, enqueue write, NDRange and readback.
 * Synchronous conformance boundary; a production actor provider must wrap
 * device events asynchronously and hold generation fences until release.
 */
inline OpenClResult execute_opencl_f32(
        const dop::OpenClArtifact& artifact,
        const std::vector<std::uint8_t>& input,
        const OpenClDeviceSelector& selector = {}) {
    if (selector.required_type != CL_DEVICE_TYPE_GPU &&
        selector.required_type != CL_DEVICE_TYPE_CPU &&
        selector.required_type != CL_DEVICE_TYPE_ACCELERATOR)
        throw std::invalid_argument("OpenCL selector must specify GPU, CPU or accelerator");
    if (artifact.required_bytes == 0 || artifact.global_rows == 0 ||
        artifact.global_fields == 0 || artifact.device_source.empty() ||
        input.size() != artifact.required_bytes ||
        artifact.required_bytes > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("invalid GPU source, geometry or byte length");

    cl_uint platforms_count = 0;
    cl_require(clGetPlatformIDs(0, nullptr, &platforms_count), "list platforms");
    if (platforms_count == 0) throw std::runtime_error("no OpenCL platform");
    std::vector<cl_platform_id> platforms(platforms_count);
    cl_require(clGetPlatformIDs(platforms_count, platforms.data(), nullptr), "read platforms");
    cl_device_id selected = nullptr;
    std::string name;
    for (auto platform : platforms) {
        cl_uint count = 0;
        const cl_int enumerated = clGetDeviceIDs(platform, selector.required_type, 0, nullptr, &count);
        if (enumerated == CL_DEVICE_NOT_FOUND) continue;
        cl_require(enumerated, "list selected devices");
        std::vector<cl_device_id> devices(count);
        cl_require(clGetDeviceIDs(platform, selector.required_type, count, devices.data(), nullptr),
                   "read selected devices");
        for (auto device : devices) {
            size_t chars = 0;
            cl_require(clGetDeviceInfo(device, CL_DEVICE_NAME, 0, nullptr, &chars), "device name size");
            std::string device_name(chars, '\0');
            cl_require(clGetDeviceInfo(device, CL_DEVICE_NAME, chars, device_name.data(), nullptr),
                       "device name");
            while (!device_name.empty() && device_name.back() == '\0') device_name.pop_back();
            if (device_name.find(selector.name_contains) != std::string::npos) {
                selected = device;
                name = std::move(device_name);
                break;
            }
        }
        if (selected) break;
    }
    if (!selected)
        throw std::runtime_error("requested OpenCL device type/name unavailable; CPU fallback prohibited");

    cl_ulong max_allocation = 0;
    cl_require(clGetDeviceInfo(selected, CL_DEVICE_MAX_MEM_ALLOC_SIZE,
                               sizeof(max_allocation), &max_allocation, nullptr),
               "device max buffer");
    if (artifact.required_bytes > max_allocation)
        throw std::invalid_argument("OpenCL byte reservation exceeds selected device allocation limit");

    cl_int error = 0;
    struct Resources {
        cl_context context = nullptr;
        cl_command_queue queue = nullptr;
        cl_program program = nullptr;
        cl_kernel kernel = nullptr;
        cl_mem src = nullptr, dst = nullptr;
        ~Resources() {
            if (dst) clReleaseMemObject(dst);
            if (src) clReleaseMemObject(src);
            if (kernel) clReleaseKernel(kernel);
            if (program) clReleaseProgram(program);
            if (queue) clReleaseCommandQueue(queue);
            if (context) clReleaseContext(context);
        }
    } objects;

    objects.context = clCreateContext(nullptr, 1, &selected, nullptr, nullptr, &error);
    cl_require(error, "create context");
    objects.queue = clCreateCommandQueue(objects.context, selected, 0, &error);
    cl_require(error, "create command queue");
    const char* source = artifact.device_source.c_str();
    size_t source_length = artifact.device_source.size();
    objects.program = clCreateProgramWithSource(objects.context, 1, &source, &source_length, &error);
    cl_require(error, "create program");
    error = clBuildProgram(objects.program, 1, &selected, "-cl-std=CL1.2", nullptr, nullptr);
    if (error != CL_SUCCESS) {
        size_t size = 0;
        clGetProgramBuildInfo(objects.program, selected, CL_PROGRAM_BUILD_LOG, 0, nullptr, &size);
        std::string log(std::min<std::size_t>(size, 4096), '\0');
        if (!log.empty())
            clGetProgramBuildInfo(objects.program, selected, CL_PROGRAM_BUILD_LOG,
                                  log.size(), log.data(), nullptr);
        throw std::runtime_error("OpenCL kernel build failed (" + std::to_string(error) + "): " + log);
    }
    objects.kernel = clCreateKernel(objects.program, artifact.kernel_name.c_str(), &error);
    cl_require(error, "create compiled kernel");
    const auto bytes = static_cast<std::size_t>(artifact.required_bytes);
    objects.src = clCreateBuffer(objects.context, CL_MEM_READ_ONLY, bytes, nullptr, &error);
    cl_require(error, "allocate src");
    objects.dst = clCreateBuffer(objects.context, CL_MEM_WRITE_ONLY, bytes, nullptr, &error);
    cl_require(error, "allocate dst");
    cl_require(clEnqueueWriteBuffer(objects.queue, objects.src, CL_TRUE, 0, bytes,
                                    input.data(), 0, nullptr, nullptr), "upload");
    cl_require(clSetKernelArg(objects.kernel, 0, sizeof(objects.src), &objects.src), "bind src");
    cl_require(clSetKernelArg(objects.kernel, 1, sizeof(objects.dst), &objects.dst), "bind dst");
    if (artifact.global_rows > std::numeric_limits<std::size_t>::max() ||
        artifact.global_fields > std::numeric_limits<std::size_t>::max())
        throw std::overflow_error("OpenCL global work dimensions too large");
    const size_t work[2] = {
        static_cast<size_t>(artifact.global_rows),
        static_cast<size_t>(artifact.global_fields)
    };
    cl_require(clEnqueueNDRangeKernel(objects.queue, objects.kernel, 2, nullptr,
                                      work, nullptr, 0, nullptr, nullptr), "dispatch");
    std::vector<std::uint8_t> output(bytes, 0);
    cl_require(clEnqueueReadBuffer(objects.queue, objects.dst, CL_TRUE, 0, bytes,
                                   output.data(), 0, nullptr, nullptr), "download");
    cl_require(clFinish(objects.queue), "quiescent finish");
    return OpenClResult{name, selector.required_type, std::move(output)};
}

} // namespace oreslang::gpu
