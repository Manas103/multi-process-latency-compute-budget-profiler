// Attaches profiler_bpf.c's three uprobes to every pipeline stage's PID
// (read from --pid-file, written by pipeline_launcher), correlates
// enter/exit events by (pid, tick) into per-stage per-tick latencies,
// samples each PID's /proc/[pid]/stat (CPU time) and /proc/[pid]/status
// (RSS) on a timer, and writes both a telemetry CSV and a Perfetto trace.
// This is the entire out-of-process attribution: nothing here is linked
// into, or run inside, any pipeline_stage process.
#include <bpf/libbpf.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "perfetto_writer.hpp"

namespace {

long now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

struct Event {
    uint32_t pid;
    uint32_t kind;
    int64_t arg;
    uint64_t ts_ns;
};

struct StageTelemetry {
    std::map<long, long> open_enter; // tick -> enter ts_ns
    std::vector<long> latencies_ns;
    std::vector<long> gpu_ns_samples;
};

std::map<int, int> g_pid_to_index;
std::map<int, StageTelemetry> g_telemetry;
shmwprof::PerfettoWriter* g_writer = nullptr;
long g_run_start_ns = 0;

long g_event_count = 0;
int handle_event(void* ctx, void* data, size_t size) {
    (void)ctx;
    g_event_count++;
    if (size < sizeof(Event)) { fprintf(stderr, "short event size=%zu\n", size); return 0; }
    Event* e = reinterpret_cast<Event*>(data);
    if (g_event_count <= 5) fprintf(stderr, "evt pid=%u kind=%u arg=%ld\n", e->pid, e->kind, (long)e->arg);
    int idx = g_pid_to_index.count((int)e->pid) ? g_pid_to_index[(int)e->pid] : -1;
    auto& tel = g_telemetry[(int)e->pid];
    uint64_t track_uuid = 1000 + (idx >= 0 ? idx : 0);

    if (e->kind == 0) { // stage_enter
        tel.open_enter[e->arg] = (long)e->ts_ns;
        if (g_writer) g_writer->add_slice_begin(track_uuid, e->ts_ns, "stage" + std::to_string(idx) + "_tick" + std::to_string(e->arg));
    } else if (e->kind == 1) { // stage_exit
        auto it = tel.open_enter.find(e->arg);
        if (it != tel.open_enter.end()) {
            tel.latencies_ns.push_back((long)e->ts_ns - it->second);
            tel.open_enter.erase(it);
        }
        if (g_writer) g_writer->add_slice_end(track_uuid, e->ts_ns);
    } else if (e->kind == 2) { // gpu_ns
        tel.gpu_ns_samples.push_back((long)e->arg);
    }
    return 0;
}

struct ProcSample {
    long cpu_ticks;
    long rss_kb;
};

ProcSample sample_proc(int pid) {
    ProcSample s{0, 0};
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    if (stat) {
        std::string line;
        std::getline(stat, line);
        size_t close_paren = line.rfind(')');
        std::istringstream iss(line.substr(close_paren + 2));
        std::string field;
        std::vector<std::string> fields;
        while (iss >> field) fields.push_back(field);
        // fields[0] is state (3rd overall field); utime is field index 11,
        // stime is index 12 counting from state=0 (see proc(5)).
        if (fields.size() > 12) s.cpu_ticks = atol(fields[11].c_str()) + atol(fields[12].c_str());
    }
    std::ifstream status("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (status && std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            s.rss_kb = atol(line.c_str() + 6);
            break;
        }
    }
    return s;
}

} // namespace

int main(int argc, char** argv) {
    std::string pid_file = "/tmp/pipeline_pids.txt";
    std::string stage_binary = "./pipeline_stage";
    std::string out_csv = "/tmp/profiler_telemetry.csv";
    std::string out_trace = "/tmp/profiler_trace.perfetto-trace";
    double duration_sec = 12.0;

    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (k == "--pid-file") pid_file = next();
        else if (k == "--stage-binary") stage_binary = next();
        else if (k == "--out-csv") out_csv = next();
        else if (k == "--out-trace") out_trace = next();
        else if (k == "--duration-sec") duration_sec = atof(next().c_str());
    }

    std::vector<std::pair<int, int>> stages; // (index, pid)
    {
        std::ifstream f(pid_file);
        int idx, pid;
        while (f >> idx >> pid) {
            stages.push_back({idx, pid});
            g_pid_to_index[pid] = idx;
        }
    }
    if (stages.empty()) {
        fprintf(stderr, "no pids in %s\n", pid_file.c_str());
        return 1;
    }

    // marker_enter/marker_exit/marker_gpu_ns live at fixed offsets in
    // stage_binary (every stage process is the same ELF file); resolved
    // once via nm, not per PID.
    auto resolve_offset = [&](const char* sym) -> unsigned long {
        std::string cmd = "nm " + stage_binary + " | grep ' " + sym + "$'";
        FILE* p = popen(cmd.c_str(), "r");
        char buf[256] = {0};
        if (p) { fgets(buf, sizeof(buf), p); pclose(p); }
        return strtoul(buf, nullptr, 16);
    };
    unsigned long off_enter = resolve_offset("shmwprof_stage_enter");
    unsigned long off_exit = resolve_offset("shmwprof_stage_exit");
    unsigned long off_gpu = resolve_offset("shmwprof_gpu_ns");

    struct bpf_object* obj = bpf_object__open_file("profiler_bpf.o", nullptr);
    if (!obj) { fprintf(stderr, "open_file failed\n"); return 1; }
    struct bpf_program* p_enter = bpf_object__find_program_by_name(obj, "on_stage_enter");
    struct bpf_program* p_exit = bpf_object__find_program_by_name(obj, "on_stage_exit");
    struct bpf_program* p_gpu = bpf_object__find_program_by_name(obj, "on_gpu_ns");
    bpf_program__set_type(p_enter, BPF_PROG_TYPE_KPROBE);
    bpf_program__set_type(p_exit, BPF_PROG_TYPE_KPROBE);
    bpf_program__set_type(p_gpu, BPF_PROG_TYPE_KPROBE);
    if (bpf_object__load(obj)) { fprintf(stderr, "load failed\n"); return 1; }

    std::vector<struct bpf_link*> links;
    for (auto& [idx, pid] : stages) {
        links.push_back(bpf_program__attach_uprobe(p_enter, false, pid, stage_binary.c_str(), off_enter));
        links.push_back(bpf_program__attach_uprobe(p_exit, false, pid, stage_binary.c_str(), off_exit));
    }
    // Only the GPU stage's pid actually ever calls shmwprof_gpu_ns, but
    // attaching to every pid is harmless (it would simply never fire for
    // the others) and avoids having to know which index is the GPU stage.
    for (auto& [idx, pid] : stages) {
        links.push_back(bpf_program__attach_uprobe(p_gpu, false, pid, stage_binary.c_str(), off_gpu));
    }

    shmwprof::PerfettoWriter writer(out_trace);
    g_writer = &writer;
    for (auto& [idx, pid] : stages) writer.add_track(1000 + idx, "stage" + std::to_string(idx));

    struct ring_buffer* rb = ring_buffer__new(bpf_object__find_map_fd_by_name(obj, "events"), handle_event, nullptr, nullptr);

    g_run_start_ns = now_ns();
    long deadline = g_run_start_ns + (long)(duration_sec * 1e9);
    std::map<int, std::vector<ProcSample>> proc_samples;
    long next_sample = g_run_start_ns;
    while (now_ns() < deadline) {
        ring_buffer__poll(rb, 20 /*ms*/);
        if (now_ns() >= next_sample) {
            for (auto& [idx, pid] : stages) proc_samples[pid].push_back(sample_proc(pid));
            next_sample += 200'000'000L; // every 200ms
        }
    }
    ring_buffer__poll(rb, 50);
    fprintf(stderr, "total events=%ld, telemetry entries=%zu\n", g_event_count, g_telemetry.size());
    for (auto& [pid, tel] : g_telemetry) {
        fprintf(stderr, "pid=%d open_enter=%zu latencies=%zu gpu=%zu\n", pid, tel.open_enter.size(),
                tel.latencies_ns.size(), tel.gpu_ns_samples.size());
    }

    FILE* csv = fopen(out_csv.c_str(), "w");
    fprintf(csv, "stage_index,pid,n_ticks,latency_p50_ns,latency_p99_ns,cpu_ticks_delta,rss_kb_max,gpu_ns_p50\n");
    for (auto& [idx, pid] : stages) {
        auto& tel = g_telemetry[pid];
        std::sort(tel.latencies_ns.begin(), tel.latencies_ns.end());
        auto pct = [&](double p) -> long {
            if (tel.latencies_ns.empty()) return -1;
            size_t i = (size_t)(p * (double)(tel.latencies_ns.size() - 1));
            return tel.latencies_ns[i];
        };
        auto& samples = proc_samples[pid];
        long cpu_delta = samples.size() >= 2 ? samples.back().cpu_ticks - samples.front().cpu_ticks : 0;
        long rss_max = 0;
        for (auto& s : samples) rss_max = std::max(rss_max, s.rss_kb);
        long gpu_p50 = -1;
        if (!tel.gpu_ns_samples.empty()) {
            std::vector<long> g = tel.gpu_ns_samples;
            std::sort(g.begin(), g.end());
            gpu_p50 = g[g.size() / 2];
        }
        fprintf(csv, "%d,%d,%zu,%ld,%ld,%ld,%ld,%ld\n", idx, pid, tel.latencies_ns.size(), pct(0.50), pct(0.99),
                cpu_delta, rss_max, gpu_p50);
    }
    fclose(csv);

    for (auto* l : links) if (l) bpf_link__destroy(l);
    ring_buffer__free(rb);
    bpf_object__close(obj);
    return 0;
}
