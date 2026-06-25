#include "hades/tool/subprocess.h"
#include <csignal>
#include <cstring>
#include <ctime>
#include <poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace hades {

static double mono() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

ProcResult run_subprocess(const std::vector<std::string>& argv,
                          const std::string& stdin_data,
                          double timeout_s,
                          std::size_t mem_limit_mb) {
    // Ignore SIGPIPE so a write to a dead child's stdin doesn't kill the parent.
    signal(SIGPIPE, SIG_IGN);

    // Create three pipes: stdin (ip), stdout (op), stderr (ep).
    // ip[0] = read end (child stdin), ip[1] = write end (parent writes)
    // op[0] = read end (parent reads), op[1] = write end (child stdout)
    // ep[0] = read end (parent reads), ep[1] = write end (child stderr)
    int ip[2], op[2], ep[2];
    if (pipe(ip) != 0 || pipe(op) != 0 || pipe(ep) != 0) {
        return {-1, "", "", false};
    }

    pid_t pid = fork();
    if (pid < 0) {
        // fork failed — close all fds
        close(ip[0]); close(ip[1]);
        close(op[0]); close(op[1]);
        close(ep[0]); close(ep[1]);
        return {-1, "", "", false};
    }

    if (pid == 0) {
        // Child process
        dup2(ip[0], 0);
        dup2(op[1], 1);
        dup2(ep[1], 2);
        // Close all pipe fds; 0/1/2 are now the dup'd ends.
        close(ip[0]); close(ip[1]);
        close(op[0]); close(op[1]);
        close(ep[0]); close(ep[1]);

        if (mem_limit_mb > 0) {
            rlimit rl{mem_limit_mb * 1024 * 1024, mem_limit_mb * 1024 * 1024};
            setrlimit(RLIMIT_AS, &rl);
        }

        std::vector<char*> a;
        a.reserve(argv.size() + 1);
        for (const auto& s : argv) {
            a.push_back(const_cast<char*>(s.c_str()));
        }
        a.push_back(nullptr);

        execvp(a[0], a.data());
        _exit(127);  // exec failed
    }

    // Parent: close child-side pipe ends to avoid blocking.
    close(ip[0]);
    close(op[1]);
    close(ep[1]);

    // Write stdin data to child (retry on EINTR).
    if (!stdin_data.empty()) {
        const char* ptr = stdin_data.data();
        std::size_t remaining = stdin_data.size();
        while (remaining > 0) {
            ssize_t w = write(ip[1], ptr, remaining);
            if (w < 0) {
                if (errno == EINTR) continue;
                break;  // EPIPE or other error — child may have died; stop writing
            }
            ptr += w;
            remaining -= static_cast<std::size_t>(w);
        }
    }
    // Signal EOF to child stdin.
    close(ip[1]);

    // Poll stdout and stderr until both close or timeout.
    ProcResult r{0, "", "", false};
    pollfd fds[2] = {
        {op[0], POLLIN, 0},
        {ep[0], POLLIN, 0}
    };
    double deadline = mono() + timeout_s;
    int open_fds = 2;

    while (open_fds > 0) {
        double remaining_s = deadline - mono();
        if (remaining_s <= 0.0) {
            kill(pid, SIGKILL);
            r.timed_out = true;
            break;
        }
        int ms = static_cast<int>(remaining_s * 1000);
        if (ms <= 0) ms = 1;

        int n = poll(fds, 2, ms);
        if (n < 0) {
            if (errno == EINTR) continue;
            // Unexpected poll error — kill and break.
            kill(pid, SIGKILL);
            r.timed_out = true;
            break;
        }
        if (n == 0) {
            // Timeout expired by poll itself.
            kill(pid, SIGKILL);
            r.timed_out = true;
            break;
        }

        char buf[4096];
        for (int i = 0; i < 2; ++i) {
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                ssize_t k;
                do {
                    k = read(fds[i].fd, buf, sizeof(buf));
                } while (k < 0 && errno == EINTR);

                if (k > 0) {
                    (i == 0 ? r.out : r.err).append(buf, static_cast<std::size_t>(k));
                } else {
                    // EOF or error on this fd.
                    fds[i].fd = -1;
                    fds[i].events = 0;
                    --open_fds;
                }
            }
        }
    }

    // After timeout: drain any remaining output (child might have written before SIGKILL).
    if (r.timed_out) {
        // Set fds back to valid fds that haven't been closed yet.
        char buf[4096];
        // Drain stdout if still open.
        if (fds[0].fd != -1) {
            ssize_t k;
            while ((k = read(fds[0].fd, buf, sizeof(buf))) > 0) {
                r.out.append(buf, static_cast<std::size_t>(k));
            }
        }
        // Drain stderr if still open.
        if (fds[1].fd != -1) {
            ssize_t k;
            while ((k = read(fds[1].fd, buf, sizeof(buf))) > 0) {
                r.err.append(buf, static_cast<std::size_t>(k));
            }
        }
    }

    // Always close parent-side read ends (guard: only if still open).
    close(op[0]);
    close(ep[0]);

    // Always reap the child (no zombie).
    int st = 0;
    waitpid(pid, &st, 0);

    if (!r.timed_out && WIFEXITED(st)) {
        r.code = WEXITSTATUS(st);
    } else if (r.timed_out) {
        r.code = -1;
    } else {
        // Killed by signal.
        r.code = -1;
    }
    return r;
}

}  // namespace hades
