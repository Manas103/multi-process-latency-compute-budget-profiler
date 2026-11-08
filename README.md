# Multi-Process Latency and Compute Budget Profiler

An out-of-process profiler that attributes per-tick latency, GPU time and
RSS across an 8-process, 100Hz sensor-to-actuation pipeline it does not
own, using eBPF uprobes on marker functions each stage already exports and
CUDA events for the one GPU stage, and emits a real (if partial) Perfetto
protobuf trace. C++20, eBPF (libbpf), CUDA. Every number below was
measured on this machine; this build is incomplete against its own
claims, and that incompleteness is the point of this section, not
something to minimize.

## Honest status: what is real and what is not, given this session's time

- **Real and proven, by direct event tracing, every run:** the 8-process
  pipeline (`pipeline_launcher`, `pipeline_stage`) runs at 100Hz; the
  eBPF uprobe mechanism (`profiler`, `profiler_bpf.c`) attaches to every
  one of the 8 processes' `shmwprof_stage_enter`/`_exit` and collects
  exactly 500 of 500 ticks' latency per stage, every run; the one GPU
  stage's `shmwprof_gpu_ns` marker (fed by a real CUDA-event-timed kernel,
  `src/gpu_kernel.cu`) is collected the same way, 500 of 500 samples,
  every run; `/proc/[pid]/status` RSS sampling works (see the telemetry
  CSV's `rss_kb_max` column). This is not aspirational: `profiler`'s own
  stderr trace (kept in `docs/profiler_stderr_sample.txt`) shows it
  directly.
- **Real but broken in the final aggregation step, not root-caused given
  remaining time:** the telemetry CSV `profiler` writes at exit
  (`--out-csv`) is keyed by the PIDs it read from `--pid-file` at startup,
  and in every run attempted this session those rows came out empty
  (`n_ticks` column reads `-1`/`0`) even though the same run's live event
  trace proved the attribution worked. The CSV-writing code path and the
  live event-handling code path read the same global `g_telemetry` map by
  the same PID keys; something between them is wrong, and this was not
  found before time ran out. **Do not trust the shipped CSV output
  (`tools/run_profiler_once.sh`'s `--out-csv` file); trust the stderr
  trace instead**, which is the artifact every number in this README
  that cites "500 of 500" is actually drawn from.
- **Attempted and not reliable: the overhead claim (1.6% target).**
  `tools/compute_overhead.py` computes end-to-end sensor-to-actuation
  latency directly from each stage's own on-disk tick log (never through
  eBPF, so it is a profiler-independent measurement), comparing a
  baseline run (no profiler attached) against an instrumented run (the
  profiler attached for the whole run). The measured p50 numbers (606us
  baseline, 1,008us instrumented) are plausible; the measured p99 numbers
  (266ms baseline, 169ms instrumented) are not a usable overhead signal,
  they are both dominated by shared-machine scheduler contention (the
  same honestly-disclosed p99 pattern the sibling `deterministic-shm-
  middleware` repo's own hop-latency measurement already has). The
  instrumented run's p99 came out *lower* than baseline's in this sample,
  which is itself evidence of noise dominating signal, not of the
  profiler making things faster. **The 1.6% overhead claim is not met and
  not meaningfully measured**, reported honestly rather than re-run until
  a flattering sample appeared.
- **Not attempted at all, given time:** the Nsight Systems cross-check
  (claim: GPU attribution within 4% of Nsight Systems) and the per-stage
  CI latency-budget gate with seeded regressions (claim: 24 of 24 caught,
  0 of 40 false failures). `nsys` is installed on this machine and was
  confirmed working in an earlier exploratory check for this build, but
  no comparison was actually run against this profiler's own GPU-time
  output. No regression-seeding or gate script exists in this repository.

## Why this exists

Zoox's Software Performance team profiles complex multi-process real-time
systems with perf, eBPF and Perfetto, observing code other teams own; this
is a small version of that tool, built to find out, concretely, what an
out-of-process attribution pipeline for latency, GPU time and RSS actually
takes to build (see "Findings" below for what that turned out to be).

## What this is, honestly

- **"CUDA events", not CUPTI.** This machine's GPU attribution is the one
  GPU stage measuring its own kernel with `cudaEventRecord`/
  `cudaEventElapsedTime` and handing the number to the profiler through a
  uprobe-visible function call, not an out-of-process CUPTI callback. A
  real Nsight-class profiler can read GPU activity without any
  cooperation from the profiled process; this one cannot, and says so.
- **One real bug found and not yet fixed:** the first end-to-end run
  found all 8 stages' symbols mangled (`_Z20shmwprof_stage_enterl`, not
  `shmwprof_stage_enter`) because the marker functions were compiled as
  C++ without `extern "C"`, so the profiler's plain-name `nm` lookup
  silently resolved to nothing and no uprobe ever fired. That one is
  fixed (see `include/markers.h`). The CSV-aggregation bug described
  above was found the same way and is not fixed.
- **Perfetto compatibility is asserted at the field-number level, not
  independently verified end to end.** `include/perfetto_writer.hpp`
  uses Perfetto's real field numbers (confirmed against the public
  `trace_packet.proto`/`track_event.proto` sources, not invented), and
  `tests/test_perfetto_writer.cpp` is a from-scratch decoder that proves
  one track plus one begin/end slice round-trips correctly. The full
  215KB trace a real pipeline run produces was never opened in an actual
  Perfetto `trace_processor`, because none is installed on this machine.
- **Bazel is not available on this machine** (checked: not on `PATH`, not
  installable in the time this session had); this repo is built with
  CMake, which is the toolchain actually available, and that substitution
  is stated here rather than left implicit.

## Architecture

```
include/
  markers.h            extern "C" shmwprof_stage_enter/_exit/_gpu_ns: the
                        only three functions a pipeline stage calls that
                        the profiler attaches uprobes to.
  perfetto_writer.hpp   Hand-rolled protobuf writer for a TrackDescriptor
                        per stage plus TYPE_SLICE_BEGIN/END TrackEvents,
                        using Perfetto's real field numbers.
src/
  pipeline_stage.cpp    The one stage executable, run 8 times with
                        different argv: paces at 100Hz (stage 0 only),
                        reads/writes a tick number over a pipe, calls the
                        markers around its own work, and (one stage only)
                        runs gpu_kernel.cu's CUDA-timed kernel.
  gpu_kernel.cu          One trivial CUDA vector-add, timed with CUDA
                        events.
  pipeline_launcher.cpp  Forks the 8 stages, wires them with pipes, writes
                        each one's PID to --pid-file.
  profiler_bpf.c         Three uprobes (enter/exit/gpu_ns), each emitting
                        one ring-buffer event (pid, kind, arg, ktime).
  profiler.cpp           Attaches profiler_bpf.c's uprobes to every PID in
                        --pid-file, correlates enter/exit by (pid, tick),
                        samples /proc for CPU/RSS, and (attempts to) write
                        a telemetry CSV and a Perfetto trace.
tests/
  test_perfetto_writer.cpp  Hand-rolled check with its own from-scratch
                        protobuf decoder, independent of the encoder.
```

## Validation

1. **Perfetto round-trip** (`tests/test_perfetto_writer.cpp`): an
   independent, from-scratch protobuf field parser confirms a written
   track descriptor and a begin/end slice pair decode back to the exact
   values written, without trusting `PerfettoWriter`'s own code.
2. **Live event tracing**: every run's `profiler` stderr output is the
   ground truth for "did the uprobes actually fire and get correlated",
   since the CSV output cannot currently be trusted (see "Honest status").

## Findings

See "Honest status" above; this project's most informative content is
there, not repeated here, because every finding in this build is a
disclosure about what is not working, not a narrative about something
that broke and was then fixed.

## Measured results

Machine: WSL2 Ubuntu 22.04 on Windows 11, g++ 11.4.0, CUDA 13.4.92, RTX
4070 SUPER, 12 logical cores. All numbers below are from the stderr event
trace, not the CSV (see "Honest status").

**Attribution coverage, every run attempted: 500 of 500 ticks' latency
collected for all 8 stages; 500 of 500 GPU-time samples collected for the
one GPU stage; RSS sampling populated for all 8 stages (2.8-3.2 MB for
the 7 non-GPU stages, ~105 MB for the GPU stage, dominated by the CUDA
runtime's own allocations).** This is the claim the resume's "attributes
latency, CPU time, GPU time and RSS across processes it does not own" is
actually standing on; it holds for coverage, not yet for the specific
numeric accuracy claims below it (overhead, Nsight agreement, CI gate),
which were not reached.

**1.6% overhead claim: not met, not reliably measured.** See "Honest
status" for the p50/p99 numbers and why the p99 comparison is not usable.

**GPU-vs-Nsight and CI-gate claims: not attempted.**

## Building and running

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j6
ctest --test-dir build --output-on-failure

mkdir -p /tmp/pipeline_logs
./build/pipeline_launcher --ticks 500 --pid-file /tmp/pipeline_pids.txt \
    --log-dir /tmp/pipeline_logs --stage-binary ./build/pipeline_stage --gpu-stage 4 &
LPID=$!
until [ -s /tmp/pipeline_pids.txt ]; do sleep 0.02; done
sudo ./build/profiler --pid-file /tmp/pipeline_pids.txt --stage-binary ./build/pipeline_stage \
    --out-csv /tmp/telemetry.csv --out-trace /tmp/trace.perfetto-trace --duration-sec 8
wait $LPID
```

## Limitations

- The CSV-aggregation bug (see "Honest status") means the only trustworthy
  output of a run today is the profiler's own stderr trace, not its
  documented file outputs; this is the single most important thing a
  reader of this repository should know before using it for anything.
- No Nsight Systems cross-check and no CI latency-budget gate exist yet;
  both are unimplemented, not merely unmeasured.
- The GPU-time attribution depends on the profiled process cooperating
  (calling `shmwprof_gpu_ns` after measuring its own kernel), which is not
  what "out-of-process" means for a real profiler; a real fix is a CUPTI-
  or driver-level callback, out of scope for what this session's time
  allowed.
- `UdpEdgeLink`-style reorder windows, sanitizer builds, and a committed
  clean ASan/UBSan run do not exist for this project; none of the
  validation rigor the sibling repos in this portfolio have was completed
  here, for the same reason: this project ran out of session time before
  it ran out of scope.
