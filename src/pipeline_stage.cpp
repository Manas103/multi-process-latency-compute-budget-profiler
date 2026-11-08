// The one pipeline-stage executable, run 8 times with different argv to
// form an 8-process sensor-to-actuation chain connected by plain pipes
// (fork+dup2 by pipeline_launcher.cpp). Which stage index gets the GPU
// workload and which is the timing source (stage 0, "sensor") or sink
// ("actuation", the last stage) is decided entirely by argv, never by an
// environment variable the profiler could see and react to: the profiler
// never influences what a stage does.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>

#include "markers.h"

extern "C" long run_gpu_kernel_ns();

namespace {

long now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

void busy_work(long spin_ns) {
    long deadline = now_ns() + spin_ns;
    volatile long x = 0;
    while (now_ns() < deadline) x++;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: pipeline_stage <index> <num_stages> <ticks> <is_gpu_stage 0|1> <log_path>\n");
        return 1;
    }
    int index = atoi(argv[1]);
    int num_stages = atoi(argv[2]);
    long ticks = atol(argv[3]);
    bool is_gpu = atoi(argv[4]) != 0;
    const char* log_path = argv[5];

    usleep(500000); // give an external profiler time to attach uprobes to every stage's PID

    FILE* log = fopen(log_path, "w");
    const bool is_source = index == 0;
    const bool is_sink = index == num_stages - 1;
    const long period_ns = 10'000'000L; // 100Hz
    long t0 = now_ns();

    for (long tick = 0; tick < ticks; ++tick) {
        if (is_source) {
            long target = t0 + tick * period_ns;
            while (now_ns() < target) usleep(200);
        } else {
            long buf;
            if (fread(&buf, sizeof(buf), 1, stdin) != 1) break; // upstream closed
        }

        shmwprof_stage_enter(tick);
        if (is_gpu) {
            long gpu_ns = run_gpu_kernel_ns();
            shmwprof_gpu_ns(gpu_ns);
        } else {
            busy_work(50'000); // 50us of synthetic CPU work
        }
        if (log) fprintf(log, "%ld,%ld\n", tick, now_ns());
        shmwprof_stage_exit(tick);

        if (!is_sink) {
            fwrite(&tick, sizeof(tick), 1, stdout);
            fflush(stdout);
        }
    }
    if (log) fclose(log);
    return 0;
}
