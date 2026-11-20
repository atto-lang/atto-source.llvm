// G3 HARDWARE-REQUIRED diagnostic probe. Not an actor scheduler, provider ABI,
// cancellation implementation, or production AttoLang guest dispatch path.
// This executable cannot report success on a CPU OpenCL device or without GPU.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using I = std::int32_t;
using U = std::uint32_t;
using U64 = std::uint64_t;
using Bits = U64;
using Platform = void*;
using Device = void*;
using Context = void*;
using Queue = void*;
using Program = void*;
using Kernel = void*;
using Buffer = void*;
using Event = void*;
constexpr I OK = 0;
constexpr U64 TYPE_GPU = 1ULL << 2;
constexpr U DEVICE_TYPE = 0x1000;
constexpr U ADDRESS_BITS = 0x100D;
constexpr U MAX_WORKGROUP = 0x1004;
constexpr U MAX_MEM_ALLOC = 0x1010;
constexpr U DEVICE_NAME = 0x102B;
constexpr U DRIVER_VERSION = 0x102D;
constexpr U DEVICE_VENDOR = 0x102C;
constexpr U EVENT_STATUS = 0x11D3;
constexpr Bits MEM_READ_ONLY = 1ULL << 2;
constexpr Bits MEM_WRITE_ONLY = 1ULL << 1;
constexpr U CL_TRUE = 1;

[[noreturn]] void invalid(const std::string& what) {
    throw std::runtime_error("GPU_PHYSICAL_PROBE_FAILED: " + what);
}
void ok(I code, const char* operation) {
    if (code != OK) invalid(std::string(operation) + " status=" + std::to_string(code));
}
class OpenCl {
    void* lib_ = nullptr;
public:
    I (*platforms)(U, Platform*, U*) = nullptr;
    I (*devices)(Platform,U64,U,Device*,U*) = nullptr;
    I (*device_info)(Device,U,std::size_t,void*,std::size_t*) = nullptr;
    Context (*create_context)(const std::intptr_t*,U,const Device*,void(*)(const char*,const void*,std::size_t,void*),void*,I*) = nullptr;
    Queue (*create_queue)(Context,Device,Bits,I*) = nullptr;
    Program (*create_program)(Context,U,const char**,const std::size_t*,I*) = nullptr;
    I (*build_program)(Program,U,const Device*,const char*,void(*)(Program,void*),void*) = nullptr;
    I (*build_info)(Program,Device,U,std::size_t,void*,std::size_t*) = nullptr;
    Kernel (*create_kernel)(Program,const char*,I*) = nullptr;
    Buffer (*create_buffer)(Context,Bits,std::size_t,void*,I*) = nullptr;
    I (*set_arg)(Kernel,U,std::size_t,const void*) = nullptr;
    I (*write_buffer)(Queue,Buffer,U,std::size_t,std::size_t,const void*,U,const Event*,Event*) = nullptr;
    I (*kernel_enqueue)(Queue,Kernel,U,const std::size_t*,const std::size_t*,const std::size_t*,U,const Event*,Event*) = nullptr;
    I (*wait_events)(U,const Event*) = nullptr;
    I (*event_info)(Event,U,std::size_t,void*,std::size_t*) = nullptr;
    I (*read_buffer)(Queue,Buffer,U,std::size_t,std::size_t,void*,U,const Event*,Event*) = nullptr;
    I (*release_buffer)(Buffer) = nullptr;
    I (*release_event)(Event) = nullptr;
    I (*release_kernel)(Kernel) = nullptr;
    I (*release_program)(Program) = nullptr;
    I (*release_queue)(Queue) = nullptr;
    I (*finish_queue)(Queue) = nullptr;
    I (*release_context)(Context) = nullptr;

    OpenCl() {
        lib_ = dlopen("libOpenCL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib_) invalid("OpenCL ICD runtime missing; no CPU fallback");
#define LOAD(member, symbol) member = reinterpret_cast<decltype(member)>(dlsym(lib_, symbol)); if (!member) invalid(std::string("missing OpenCL symbol: ") + symbol)
        LOAD(platforms, "clGetPlatformIDs");
        LOAD(devices, "clGetDeviceIDs");
        LOAD(device_info, "clGetDeviceInfo");
        LOAD(create_context, "clCreateContext");
        LOAD(create_queue, "clCreateCommandQueue");
        LOAD(create_program, "clCreateProgramWithSource");
        LOAD(build_program, "clBuildProgram");
        LOAD(build_info, "clGetProgramBuildInfo");
        LOAD(create_kernel, "clCreateKernel");
        LOAD(create_buffer, "clCreateBuffer");
        LOAD(set_arg, "clSetKernelArg");
        LOAD(write_buffer, "clEnqueueWriteBuffer");
        LOAD(kernel_enqueue, "clEnqueueNDRangeKernel");
        LOAD(wait_events, "clWaitForEvents");
        LOAD(event_info, "clGetEventInfo");
        LOAD(read_buffer, "clEnqueueReadBuffer");
        LOAD(release_buffer, "clReleaseMemObject");
        LOAD(release_event, "clReleaseEvent");
        LOAD(release_kernel, "clReleaseKernel");
        LOAD(release_program, "clReleaseProgram");
        LOAD(release_queue, "clReleaseCommandQueue");
        LOAD(finish_queue, "clFinish");
        LOAD(release_context, "clReleaseContext");
#undef LOAD
    }
    OpenCl(const OpenCl&) = delete;
    OpenCl& operator=(const OpenCl&) = delete;
    ~OpenCl() { if (lib_) dlclose(lib_); }
    std::string text(Device device, U field) const {
        std::size_t size = 0;
        ok(device_info(device,field,0,nullptr,&size),"clGetDeviceInfo(size)");
        if (size == 0 || size > 4096) invalid("unbounded/missing device field");
        std::string result(size,'\0');
        ok(device_info(device,field,size,result.data(),nullptr),"clGetDeviceInfo(data)");
        if (!result.empty() && result.back() == '\0') result.pop_back();
        if (result.empty()) invalid("empty GPU device field");
        return result;
    }
    template<class T> T scalar(Device device, U field) const {
        T v{};
        ok(device_info(device,field,sizeof(v),&v,nullptr),"clGetDeviceInfo(scalar)");
        return v;
    }
};
struct Handles {
    OpenCl& api;
    Context context = nullptr;
    Queue queue = nullptr;
    Program program = nullptr;
    Kernel kernel = nullptr;
    Buffer x = nullptr, y = nullptr, out = nullptr;
    Event fence = nullptr;
    explicit Handles(OpenCl& a) : api(a) {}
    ~Handles() {
        // A returned kernel event witnesses kernel completion; clFinish also
        // fences pre-kernel uploads and post-kernel reads on failure paths.
        if ((fence && api.wait_events(1,&fence) != OK) ||
            (queue && api.finish_queue(queue) != OK)) {
            // Do not deallocate GPU memory while a possibly live kernel can
            // still read or write it; the process must quarantine and exit.
            std::cerr << "GPU_G3_QUARANTINED: device event not quiescent\n";
            return;
        }
        if (fence) (void)api.release_event(fence);
        if (out) (void)api.release_buffer(out);
        if (y) (void)api.release_buffer(y);
        if (x) (void)api.release_buffer(x);
        if (kernel) (void)api.release_kernel(kernel);
        if (program) (void)api.release_program(program);
        if (queue) (void)api.release_queue(queue);
        if (context) (void)api.release_context(context);
    }
};
Device select_physical_gpu(OpenCl& api) {
    U count = 0;
    if (api.platforms(0,nullptr,&count) != OK || count == 0)
        invalid("no OpenCL GPU platform; CPU fallback forbidden");
    if (count > 256) invalid("OpenCL platform count exceeds probe admission quota");
    std::vector<Platform> platforms(count);
    ok(api.platforms(count,platforms.data(),nullptr),"clGetPlatformIDs");
    for (auto platform : platforms) {
        U n = 0;
        if (api.devices(platform, TYPE_GPU, 0, nullptr, &n) != OK || n == 0)
            continue;
        if (n > 256) invalid("GPU device count exceeds probe admission quota");
        std::vector<Device> devices(n);
        ok(api.devices(platform, TYPE_GPU, n, devices.data(),nullptr),"clGetDeviceIDs(GPU)");
        for (Device d : devices) {
            if ((api.scalar<U64>(d,DEVICE_TYPE) & TYPE_GPU) != 0 &&
                api.scalar<U>(d,ADDRESS_BITS) == 64 &&
                api.scalar<std::size_t>(d,MAX_WORKGROUP) >= 256 &&
                api.scalar<U64>(d,MAX_MEM_ALLOC) >= 4096) return d;
        }
    }
    invalid("no 64-bit physical GPU with 256-lane groups; CPU fallback forbidden");
}
void execute(const std::string& source) {
    OpenCl api;
    const Device device=select_physical_gpu(api);
    const auto name=api.text(device,DEVICE_NAME);
    const auto driver=api.text(device,DRIVER_VERSION);
    const auto vendor=api.text(device,DEVICE_VENDOR);
    constexpr std::size_t n=1003;
    constexpr std::size_t local=256;
    constexpr std::size_t global=((n + local - 1)/local)*local;
    constexpr std::size_t bytes=n*sizeof(float);
    const float scale=1.5f;
    std::vector<float> x(n), y(n), out(n,-100.0f);
    for(std::size_t i=0;i<n;++i) {
        x[i]=static_cast<float>(i%43) * 0.125f;
        y[i]=static_cast<float>(i%19) * -0.5f;
    }
    Handles h(api);
    I err=OK;
    h.context=api.create_context(nullptr,1,&device,nullptr,nullptr,&err);
    ok(err,"clCreateContext"); if(!h.context) invalid("null OpenCL context");
    h.queue=api.create_queue(h.context,device,0,&err);
    ok(err,"clCreateCommandQueue"); if(!h.queue) invalid("null command queue");
    const char* text=source.c_str();
    const std::size_t source_bytes=source.size();
    h.program=api.create_program(h.context,1,&text,&source_bytes,&err);
    ok(err,"clCreateProgramWithSource"); if(!h.program) invalid("null device program");
    err=api.build_program(h.program,1,&device,"-cl-std=CL1.2",nullptr,nullptr);
    if(err != OK) {
        std::size_t size=0;
        constexpr U CL_PROGRAM_BUILD_LOG=0x1183;
        (void)api.build_info(h.program,device,CL_PROGRAM_BUILD_LOG,0,nullptr,&size);
        std::string log(std::min<std::size_t>(size,8192),'\0');
        if(!log.empty()) (void)api.build_info(h.program,device,CL_PROGRAM_BUILD_LOG,log.size(),log.data(),nullptr);
        invalid("device kernel build rejected: " + log);
    }
    h.kernel=api.create_kernel(h.program,"attolang_saxpy_v1",&err);
    ok(err,"clCreateKernel"); if(!h.kernel) invalid("null kernel handle");
    h.x=api.create_buffer(h.context,MEM_READ_ONLY,bytes,nullptr,&err);
    ok(err,"clCreateBuffer(x)"); if(!h.x) invalid("null x buffer");
    h.y=api.create_buffer(h.context,MEM_READ_ONLY,bytes,nullptr,&err);
    ok(err,"clCreateBuffer(y)"); if(!h.y) invalid("null y buffer");
    h.out=api.create_buffer(h.context,MEM_WRITE_ONLY,bytes,nullptr,&err);
    ok(err,"clCreateBuffer(out)"); if(!h.out) invalid("null output buffer");
    ok(api.write_buffer(h.queue,h.x,CL_TRUE,0,bytes,x.data(),0,nullptr,nullptr),"clEnqueueWriteBuffer(x)");
    ok(api.write_buffer(h.queue,h.y,CL_TRUE,0,bytes,y.data(),0,nullptr,nullptr),"clEnqueueWriteBuffer(y)");
    U64 count=n;
    ok(api.set_arg(h.kernel,0,sizeof(scale),&scale),"clSetKernelArg(scale)");
    ok(api.set_arg(h.kernel,1,sizeof(h.x),&h.x),"clSetKernelArg(x)");
    ok(api.set_arg(h.kernel,2,sizeof(h.y),&h.y),"clSetKernelArg(y)");
    ok(api.set_arg(h.kernel,3,sizeof(h.out),&h.out),"clSetKernelArg(out)");
    ok(api.set_arg(h.kernel,4,sizeof(count),&count),"clSetKernelArg(n)");
    ok(api.kernel_enqueue(h.queue,h.kernel,1,nullptr,&global,&local,0,nullptr,&h.fence),"clEnqueueNDRangeKernel");
    if(!h.fence) invalid("device submission returned no completion event");
    ok(api.wait_events(1,&h.fence),"clWaitForEvents(kernel)");
    I completion=-1;
    ok(api.event_info(h.fence,EVENT_STATUS,sizeof(completion),&completion,nullptr),"clGetEventInfo");
    if(completion!=0) invalid("kernel event is not COMPLETE");
    ok(api.read_buffer(h.queue,h.out,CL_TRUE,0,bytes,out.data(),0,nullptr,nullptr),"clEnqueueReadBuffer(out)");
    for(std::size_t i=0;i<n;++i) {
        volatile float p=scale*x[i];
        const float expected=p+y[i];
        const float actual=out[i];
        if(!std::isfinite(actual)||std::fabs(expected-actual)>1e-5f+1e-5f*std::fabs(expected))
            invalid("independent f32 numeric oracle mismatch at item "+std::to_string(i));
    }
    std::cout<<"GPU_OPENCL_REPORTED_GPU_SAXPY_PASS device="<<name
             <<" vendor="<<vendor
             <<" driver="<<driver
             <<" logical_items="<<n
             <<" dispatched_lanes="<<global
             <<" oracle_items="<<n
             <<" kernel_event_status=CL_COMPLETE"
             <<" CPU_FALLBACK=false"
             <<" HARDWARE_ATTESTATION=EXTERNAL_REQUIRED\n";
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=2) invalid("usage: atto_gpu_physical_saxpy_probe source.cl");
        std::ifstream source(argv[1],std::ios::binary);
        if(!source) invalid("cannot read device source");
        std::string text((std::istreambuf_iterator<char>(source)),std::istreambuf_iterator<char>());
        if(text.size()>65536||text.empty()) invalid("device artifact size invalid");
        execute(text);
        return 0;
    } catch (const std::exception& e) {
        std::cerr<<e.what()<<"\n";
        return 1;
    }
}
