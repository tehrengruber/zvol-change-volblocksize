// Minimal process helpers: run commands and stream a `producer | consumer`
// pipeline line by line without ever buffering the whole output.
#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace sp {

struct CommandError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string join(const std::vector<std::string>& args);

// Run a command and return its stdout; throws CommandError on non-zero exit.
std::string check_output(const std::vector<std::string>& args);

// Run a command inheriting this process's stdout/stderr; throw on non-zero exit.
void check_call(const std::vector<std::string>& args);

// Run a command inheriting stdio and return its exit code (no throw).
int call_status(const std::vector<std::string>& args);

// Run a command with stdout/stderr sent to /dev/null; return its exit code.
int run_quiet(const std::vector<std::string>& args);

// Spawn `producer | consumer` and invoke `on_line` for every line the consumer
// writes to stdout.  Neither program's output is fully materialised.  Throws if
// either child exits non-zero (reporting both when both fail; a producer killed
// by SIGPIPE is tolerated); if `on_line` throws, both children are killed and the
// exception is re-raised.
void pipeline_for_each_line(
    const std::vector<std::string>& producer,
    const std::vector<std::string>& consumer,
    const std::function<void(const std::string&)>& on_line);

// pread exactly `count` bytes at `offset` (looping over short reads, retrying
// EINTR).  Returns false on any read error or EOF before `count` bytes.
bool pread_full(int fd, void* buf, size_t count, uint64_t offset);

}  // namespace sp
