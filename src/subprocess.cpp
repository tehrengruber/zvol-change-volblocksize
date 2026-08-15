// C++ has no standard process/pipe facility, so this is POSIX fork/exec.  We keep
// argv-based spawning (no shell) to avoid quoting/injection of arbitrary property
// values and to observe each process's exit code precisely.  O_CLOEXEC pipe ends
// mean the children need no manual fd bookkeeping: they close automatically on
// exec, leaving only what we dup'd onto stdin/stdout.
#define _GNU_SOURCE 1

#include "subprocess.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace sp {

std::string join(const std::vector<std::string>& args) {
    std::string s;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) s += ' ';
        s += args[i];
    }
    return s;
}

[[noreturn]] static void exec_or_die(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);  // exec failed (e.g. command not found)
}

static int exit_code(int status) {
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Fork and exec `args`, redirecting stdin/stdout/stderr to the given fds when
// >= 0.  Those fds should be O_CLOEXEC (e.g. pipe2 ends); dup2 clears CLOEXEC on
// the descriptor it installs, so exactly the redirected streams survive exec.
static pid_t spawn(const std::vector<std::string>& args, int in_fd = -1,
                   int out_fd = -1, int err_fd = -1) {
    pid_t pid = fork();
    if (pid < 0) throw CommandError("fork() failed");
    if (pid == 0) {
        if (in_fd >= 0) dup2(in_fd, STDIN_FILENO);
        if (out_fd >= 0) dup2(out_fd, STDOUT_FILENO);
        if (err_fd >= 0) dup2(err_fd, STDERR_FILENO);
        exec_or_die(args);
    }
    return pid;
}

// Spawn but close `a`/`b` (pipe ends already owned by the caller) if fork fails,
// so a failed spawn doesn't leak descriptors.
static pid_t spawn_or_close(const std::vector<std::string>& args, int in_fd,
                            int out_fd, int err_fd, int a, int b) {
    try {
        return spawn(args, in_fd, out_fd, err_fd);
    } catch (...) {
        if (a >= 0) close(a);
        if (b >= 0) close(b);
        throw;
    }
}

std::string check_output(const std::vector<std::string>& args) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) throw CommandError("pipe2() failed");
    pid_t pid = spawn_or_close(args, -1, fds[1], -1, fds[0], fds[1]);
    close(fds[1]);
    std::string out;
    char buf[65536];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) != 0) {
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fds[0]);
            waitpid(pid, nullptr, 0);
            throw CommandError("read error capturing output of: " + join(args));
        }
        out.append(buf, n);
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (exit_code(status) != 0)
        throw CommandError("command failed: " + join(args));
    return out;
}

int call_status(const std::vector<std::string>& args) {
    pid_t pid = spawn(args);
    int status = 0;
    waitpid(pid, &status, 0);
    return exit_code(status);
}

void check_call(const std::vector<std::string>& args) {
    int rc = call_status(args);
    if (rc != 0)
        throw CommandError("command failed (rc=" + std::to_string(rc) + "): " +
                           join(args));
}

void check_call_input(const std::vector<std::string>& args,
                      const std::string& input) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) throw CommandError("pipe2() failed");
    pid_t pid = spawn_or_close(args, fds[0], -1, -1, fds[0], fds[1]);  // stdin=read end
    close(fds[0]);

    // A child that dies mid-write would SIGPIPE us; ignore it and rely on the
    // child's exit code to report the failure.
    struct sigaction ign {}, old {};
    ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ign, &old);
    const char* p = input.data();
    size_t left = input.size();
    while (left > 0) {
        ssize_t n = write(fds[1], p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;  // child gone; exit code below is the real error
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    close(fds[1]);
    sigaction(SIGPIPE, &old, nullptr);

    int status = 0;
    waitpid(pid, &status, 0);
    if (exit_code(status) != 0)
        throw CommandError("command failed (rc=" + std::to_string(exit_code(status)) +
                           "): " + join(args));
}

int run_quiet(const std::vector<std::string>& args) {
    int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (devnull < 0) throw CommandError("cannot open /dev/null");
    pid_t pid = spawn_or_close(args, -1, devnull, devnull, devnull, -1);
    close(devnull);
    int status = 0;
    waitpid(pid, &status, 0);
    return exit_code(status);
}

void pipeline_for_each_line(
    const std::vector<std::string>& producer,
    const std::vector<std::string>& consumer,
    const std::function<void(const std::string&)>& on_line) {
    int p1[2], p2[2];  // producer->consumer, consumer->parent
    if (pipe2(p1, O_CLOEXEC) != 0) throw CommandError("pipe2() failed");
    if (pipe2(p2, O_CLOEXEC) != 0) {
        close(p1[0]);
        close(p1[1]);
        throw CommandError("pipe2() failed");
    }

    pid_t prod, cons;
    try {
        prod = spawn(producer, -1, p1[1]);
    } catch (...) {
        close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
        throw;
    }
    try {
        cons = spawn(consumer, p1[0], p2[1]);
    } catch (...) {
        close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
        kill(prod, SIGKILL);
        waitpid(prod, nullptr, 0);
        throw;
    }
    close(p1[0]);
    close(p1[1]);
    close(p2[1]);

    FILE* f = fdopen(p2[0], "r");
    if (!f) {
        close(p2[0]);
        kill(prod, SIGKILL);
        kill(cons, SIGKILL);
        waitpid(prod, nullptr, 0);
        waitpid(cons, nullptr, 0);
        throw CommandError("fdopen() failed");
    }

    std::exception_ptr err;
    char* line = nullptr;
    size_t cap = 0;
    ssize_t len;
    try {
        while ((len = getline(&line, &cap, f)) >= 0)
            on_line(std::string(line, static_cast<size_t>(len)));
    } catch (...) {
        err = std::current_exception();
    }
    free(line);
    fclose(f);

    if (err) {  // consumer stopped early: unblock the children before reaping
        kill(prod, SIGKILL);
        kill(cons, SIGKILL);
    }
    int st_prod = 0, st_cons = 0;
    waitpid(prod, &st_prod, 0);
    waitpid(cons, &st_cons, 0);
    if (err) std::rethrow_exception(err);

    // Report both when both fail: a broken producer (zfs send) usually makes the
    // consumer (zstream) exit non-zero too, and the producer is the root cause.
    std::string errs;
    if (exit_code(st_cons) != 0)
        errs = "consumer failed (rc=" + std::to_string(exit_code(st_cons)) +
               "): " + join(consumer);
    // A producer killed by SIGPIPE is fine; only flag a real non-zero exit.
    if (WIFEXITED(st_prod) && WEXITSTATUS(st_prod) != 0)
        errs += (errs.empty() ? "" : "; ") + std::string("producer failed (rc=") +
                std::to_string(WEXITSTATUS(st_prod)) + "): " + join(producer);
    if (!errs.empty()) throw CommandError(errs);
}

bool pread_full(int fd, void* buf, size_t count, uint64_t offset) {
    char* p = static_cast<char*>(buf);
    while (count > 0) {
        ssize_t n = pread(fd, p, count, offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;  // unexpected EOF
        p += n;
        offset += static_cast<uint64_t>(n);
        count -= static_cast<size_t>(n);
    }
    return true;
}

}  // namespace sp
