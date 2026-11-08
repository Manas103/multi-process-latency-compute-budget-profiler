#pragma once
// Exported, never-inlined marker functions every pipeline stage calls
// around its own work. The profiler never calls these; it attaches eBPF
// uprobes to them from outside the process (see src/profiler_bpf.c), which
// is the entire "out-of-process" design: a stage's own business logic
// never changes depending on whether a profiler is attached.
//
// extern "C" is load-bearing, not decoration: the profiler resolves these
// symbols by plain name via `nm` (src/profiler.cpp's resolve_offset()),
// and a C++-mangled name (which this header would otherwise produce,
// since pipeline_stage.cpp is compiled as C++) would silently never
// match, which is exactly the bug this project's first end-to-end run
// found (see README "Findings").
extern "C" {
__attribute__((noinline)) void shmwprof_stage_enter(long tick) { __asm__ volatile(""); }
__attribute__((noinline)) void shmwprof_stage_exit(long tick) { __asm__ volatile(""); }
__attribute__((noinline)) void shmwprof_gpu_ns(long elapsed_ns) { __asm__ volatile(""); }
}
