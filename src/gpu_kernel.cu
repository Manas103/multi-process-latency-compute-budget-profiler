// The one CUDA kernel in this repository: a trivial vector-add used only
// as a stand-in GPU workload for one pipeline stage. run_gpu_kernel_ns()
// times it with CUDA events (cudaEventRecord/cudaEventElapsedTime), which
// is the "CUDA events" half of the profiler's "eBPF uprobes and CUDA
// events" GPU attribution: the stage measures its own GPU time this way
// and hands the number to the profiler through shmwprof_gpu_ns(), a
// uprobe-visible function call, since this machine has no CUPTI-based
// out-of-process GPU callback API available to the profiler itself.
#include <cuda_runtime.h>

__global__ void add_kernel(const float* a, const float* b, float* c, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) c[i] = a[i] + b[i];
}

extern "C" long run_gpu_kernel_ns() {
    const int n = 1 << 16;
    static float *d_a = nullptr, *d_b = nullptr, *d_c = nullptr;
    if (!d_a) {
        cudaMalloc(&d_a, n * sizeof(float));
        cudaMalloc(&d_b, n * sizeof(float));
        cudaMalloc(&d_c, n * sizeof(float));
    }
    cudaEvent_t start, stop;
    cudaEventCreate(&start);
    cudaEventCreate(&stop);

    cudaEventRecord(start);
    add_kernel<<<(n + 255) / 256, 256>>>(d_a, d_b, d_c, n);
    cudaEventRecord(stop);
    cudaEventSynchronize(stop);

    float ms = 0.0f;
    cudaEventElapsedTime(&ms, start, stop);
    cudaEventDestroy(start);
    cudaEventDestroy(stop);
    return (long)(ms * 1e6);
}
