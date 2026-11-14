// P1 physical GPU kernel fixture; NOT AttoLang source or actor runtime MIR.
// The CPU oracle lives in atto_gpu_physical_saxpy_probe.cpp.
// No CPU fallback and no implicit host/device transfers.
__kernel void attolang_saxpy_v1(
    const float alpha,
    __global const float* restrict x,
    __global const float* restrict y,
    __global float* restrict out,
    const ulong n
) {
    const ulong i = (ulong)get_global_id(0);
    if (i >= n) return;  // required padded-tail guard for 1003 logical items
    out[i] = alpha * x[i] + y[i];
}
