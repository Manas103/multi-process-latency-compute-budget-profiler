// Forks the 8-process sensor-to-actuation chain, connecting consecutive
// stages with plain anonymous pipes, and writes each stage's PID to
// --pid-file so an external profiler (or nothing, for the uninstrumented
// control run) can attach before the chain finishes. Each stage sleeps
// 500ms before its first tick (see pipeline_stage.cpp), which is the
// window a profiler has to attach uprobes to every PID before real data
// starts flowing.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    int num_stages = 8;
    long ticks = 1000;
    int gpu_stage_index = 4;
    std::string pid_file = "/tmp/pipeline_pids.txt";
    std::string log_dir = "/tmp/pipeline_logs";
    std::string stage_binary = "./pipeline_stage";

    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (k == "--ticks") ticks = atol(next().c_str());
        else if (k == "--pid-file") pid_file = next();
        else if (k == "--log-dir") log_dir = next();
        else if (k == "--stage-binary") stage_binary = next();
        else if (k == "--gpu-stage") gpu_stage_index = atoi(next().c_str());
    }

    std::vector<int> pipe_fds(2 * (num_stages - 1));
    for (int i = 0; i < num_stages - 1; ++i) pipe(&pipe_fds[2 * i]);

    std::vector<pid_t> pids(num_stages);
    for (int i = 0; i < num_stages; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            if (i > 0) dup2(pipe_fds[2 * (i - 1)], STDIN_FILENO);
            if (i < num_stages - 1) dup2(pipe_fds[2 * i + 1], STDOUT_FILENO);
            for (int f : pipe_fds) close(f);

            std::string log_path = log_dir + "/stage" + std::to_string(i) + ".csv";
            std::string is_gpu = (i == gpu_stage_index) ? "1" : "0";
            execl(stage_binary.c_str(), stage_binary.c_str(), std::to_string(i).c_str(),
                  std::to_string(num_stages).c_str(), std::to_string(ticks).c_str(), is_gpu.c_str(),
                  log_path.c_str(), (char*)nullptr);
            perror("execl");
            _exit(127);
        }
        pids[i] = pid;
    }
    for (int f : pipe_fds) close(f);

    FILE* pf = fopen(pid_file.c_str(), "w");
    for (int i = 0; i < num_stages; ++i) fprintf(pf, "%d %d\n", i, pids[i]);
    fclose(pf);

    int worst = 0;
    for (pid_t pid : pids) {
        int status = 0;
        waitpid(pid, &status, 0);
        if (status != 0) worst = status;
    }
    return worst == 0 ? 0 : 1;
}
