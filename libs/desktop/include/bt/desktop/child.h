// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// The node a drop-in app runs (doc/app.md "The app and its node"): one child process, never a shell; its standard
// input a pipe the app holds (the node stops when it closes: --bt-app-exit-with-stdin), its output appended to a log
// file; on Linux its own process group and PR_SET_PDEATHSIG, on Windows a job object that kills it with the app.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bt::desktop {

class Child {
public:
    Child() = default;
    ~Child();
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    /** Start `program` with `args` (argv[1..]); its output appended to `log_file`. False with `err`. */
    bool start(const std::string& program, const std::vector<std::string>& args, const std::string& log_file, std::string& err);
    /** Whether it still runs; its exit status once it ended. */
    bool running();
    std::optional<int> exit_status() const { return status_; }
    /** Close its standard input (it stops by itself), wait `grace_ms`, then terminate it, then kill it. */
    void stop(int grace_ms);
    /** Wait for it to end, at most `ms`; true when it ended. */
    bool wait(int ms);
    int64_t pid() const { return pid_; }

private:
    int64_t pid_{-1};
    std::optional<int> status_;
#ifdef _WIN32
    void* process_{nullptr};
    void* job_{nullptr};
    void* stdin_write_{nullptr};
#else
    int stdin_write_{-1};
#endif
};

/** The folder of the running program (the node is looked for next to the app). */
std::string program_dir();

} // namespace bt::desktop
