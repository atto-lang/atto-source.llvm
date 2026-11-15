#include "oreslang/runtime/CudaSystemDriver.hpp"
#include <exception>
#include <iostream>
int main() {
    try {
        // Without an externally established trusted CUDA context, rejection
        // is expected; this does not exercise hardware or guest execution.
        oreslang::cuda_aot::SystemDriver api;
        std::cout << "cuda-driver-context-observed:sm=" << api.current_sm() << '\n';
    } catch (const std::exception&) {
        std::cout << "cuda-driver-not-admitted:NOT_EXECUTED:no-trusted-context\n";
        return 77;
    }
    return 0;
}
