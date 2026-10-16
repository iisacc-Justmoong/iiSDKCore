#pragma once
#include "../Core/Bytes/Bytes.h"
#include "../Core/Result/Result.h"
#include "../Core/Time/Deadline.h"
#include "../IO/Channel.h"
#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace ii::process {
using core::Result;
using Environment = std::map<std::string, std::string, std::less<>>;
using EnvironmentPatch = std::map<std::string, std::optional<std::string>, std::less<>>;
enum class Input { pipe, closed, inherit };
enum class Output { pipe, discard, inherit };
enum class ErrorOutput { pipe, merge_output, discard, inherit };
struct Launch final {
    // argv[0] is the executable. No shell expansion or command-string parser.
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    bool inherit_environment = true;
    EnvironmentPatch environment; // nullopt removes; "" sets an empty value.
    Input input = Input::pipe;
    Output output = Output::pipe;
    ErrorOutput error = ErrorOutput::pipe;
};
struct Capabilities final { bool launch; bool pipes; bool working_directory; bool owned_tree; };
[[nodiscard]] Capabilities capabilities() noexcept;
// Snapshot only: never mutates the host's environment. Coordinate external
// setenv/putenv/environ writers in the host before taking a snapshot/launching.
[[nodiscard]] Result<Environment> current_environment();
enum class ExitReason { exited, signaled, cancelled, timed_out, owner_removed };
struct Exit final {
    ExitReason reason = ExitReason::exited;
    std::uint32_t code = 0; // Windows DWORD is retained without signed narrowing.
    int signal = 0;        // POSIX only; zero for normal exits and Windows.
};
enum class Stream { output, error };
struct CommunicationOptions final {
    std::stop_token stop;
    core::Deadline deadline;
    std::size_t output_limit = 8 * 1024 * 1024;
    std::size_t error_limit = 8 * 1024 * 1024;
    // Runs on the calling thread; bytes are borrowed only during this call.
    // Must return promptly to preserve deadline/cancellation responsiveness.
    std::function<void(Stream, core::ByteView)> receive;
};
struct Communication final {
    Exit exit;
    core::Bytes output, error;
    bool output_truncated = false, error_truncated = false;
    std::size_t input_written = 0;
    bool input_closed_early = false;
};
// Move-only owner. Destruction terminates the owned tree and reaps the direct
// child. Control methods serialize; one caller owns pipe transfers at a time.
// A completed root also retires remaining owned descendants before PID reuse.
class Process final {
public:
    Process() noexcept;
    ~Process();
    Process(Process&&) noexcept;
    Process& operator=(Process&&) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    [[nodiscard]] static Result<Process> start(const Launch& launch);
    [[nodiscard]] std::uint64_t id() const noexcept;
    [[nodiscard]] io::Channel input() const;
    [[nodiscard]] io::Channel output() const;
    [[nodiscard]] io::Channel error() const;
    [[nodiscard]] Result<std::optional<Exit>> poll();
    // A wait timeout leaves the process running; it does not drain its pipes.
    [[nodiscard]] Result<std::optional<Exit>> wait_for(std::chrono::milliseconds timeout);
    [[nodiscard]] Result<Exit> cancel();
    // Pumps all three nonblocking pipes, closes stdin after input, drains both
    // outputs even after capture limits, and reaps. Cancellation/deadline are
    // terminal Exit reasons with partial output retained, not spawn failures.
    [[nodiscard]] Result<Communication> communicate(core::ByteView input = {}, CommunicationOptions options = {});
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ii::process
