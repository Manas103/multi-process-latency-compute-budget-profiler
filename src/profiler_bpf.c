// The profiler's own kernel-side code: three uprobes on the marker
// functions every pipeline stage calls (include/markers.h), each emitting
// one ring-buffer event carrying (pid, kind, arg, kernel timestamp). This
// is the entire mechanism that makes latency attribution "out of process":
// nothing here runs in the stage's address space except the breakpoint
// trap itself, and nothing in pipeline_stage.cpp knows this file exists.
#include <linux/bpf.h>
#include <linux/ptrace.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

struct event {
    __u32 pid;
    __u32 kind; // 0 = stage_enter, 1 = stage_exit, 2 = gpu_ns
    __s64 arg;  // tick number (enter/exit) or elapsed ns (gpu)
    __u64 ts_ns;
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 16);
} events SEC(".maps");

static __always_inline void emit(struct pt_regs *ctx, __u32 kind) {
    struct event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e) return;
    e->pid = bpf_get_current_pid_tgid() >> 32;
    e->kind = kind;
    e->arg = (long)PT_REGS_PARM1(ctx);
    e->ts_ns = bpf_ktime_get_ns();
    bpf_ringbuf_submit(e, 0);
}

SEC("uprobe") int on_stage_enter(struct pt_regs *ctx) { emit(ctx, 0); return 0; }
SEC("uprobe") int on_stage_exit(struct pt_regs *ctx) { emit(ctx, 1); return 0; }
SEC("uprobe") int on_gpu_ns(struct pt_regs *ctx) { emit(ctx, 2); return 0; }

char _license[] SEC("license") = "GPL";
