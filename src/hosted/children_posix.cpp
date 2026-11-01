// Child processes on POSIX hosts (include/stx_auxlib/children.hpp), over
// posix_spawn. posix_spawn reports a failed exec to the parent on macOS and
// on glibc 2.24 and later, so start can report a missing program. Children
// start with default signal handling and an empty signal mask, whatever
// the parent ignores or blocks (dispositions set to "ignore" otherwise
// survive exec).

#include "stx_auxlib/children.hpp"

#include "children_common.hpp"

#include "stx_auxlib/clocks.hpp"
#include "stx_auxlib/threads.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace stx_auxlib {

struct Child::State final {
    pid_t pid{-1};
    // The read end of the captured output pipe, or -1.
    int output{-1};
    bool waited{false};
};

namespace {

CapabilityStatus status_from_errno(const int error) noexcept {
    switch (error) {
    case ENOENT:
    case ENOTDIR:
        return CapabilityStatus::not_found;
    case EACCES:
    case EPERM:
    case ENOEXEC:
        return CapabilityStatus::permission_denied;
    case ENOMEM:
    case EAGAIN:
    case EMFILE:
    case ENFILE:
        return CapabilityStatus::resources_exhausted;
    default:
        break;
    }
    return CapabilityStatus::io_error;
}

// Children destroyed before being waited for, reaped without blocking by
// later starts and waits. Never destroyed, so it outlives static
// destructors that destroy Child objects.
struct Abandoned final {
    Mutex lock{};
    std::vector<pid_t> children{};
};

Abandoned& abandoned() noexcept {
    static Abandoned* const state = new Abandoned();
    return *state;
}

void reap_abandoned() noexcept {
    Abandoned& state = abandoned();
    const ScopedLock lock(state.lock);
    std::size_t kept = 0;
    for (std::size_t index = 0; index < state.children.size(); ++index) {
        const pid_t child = state.children[index];
        int status = 0;
        const pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result == 0) {
            state.children[kept] = child;
            ++kept;
        }
    }
    state.children.resize(kept);
}

// Owns a posix_spawn_file_actions_t and a posix_spawnattr_t for one start.
struct SpawnSetup final {
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    bool actions_ready{false};
    bool attributes_ready{false};

    SpawnSetup() noexcept = default;
    SpawnSetup(const SpawnSetup&) = delete;
    SpawnSetup& operator=(const SpawnSetup&) = delete;
    ~SpawnSetup() {
        if (actions_ready) (void)posix_spawn_file_actions_destroy(&actions);
        if (attributes_ready) (void)posix_spawnattr_destroy(&attributes);
    }
};

// Owns a descriptor; -1 holds none.
struct Descriptor final {
    int value{-1};
    Descriptor() noexcept = default;
    explicit Descriptor(const int descriptor) noexcept : value(descriptor) {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() {
        if (value >= 0) (void)::close(value);
    }
    int release() noexcept { return std::exchange(value, -1); }
};

// Signals whose disposition the child resets to the default.
constexpr std::array<int, 7> reset_signals = {SIGPIPE, SIGINT, SIGTERM, SIGHUP,
                                              SIGQUIT, SIGTSTP, SIGWINCH};

int prepare(SpawnSetup& setup, const ChildRequest& request, const int output_writer) noexcept {
    int error = posix_spawn_file_actions_init(&setup.actions);
    if (error != 0) return error;
    setup.actions_ready = true;
    error = posix_spawnattr_init(&setup.attributes);
    if (error != 0) return error;
    setup.attributes_ready = true;

    error = posix_spawn_file_actions_addopen(&setup.actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (error == 0 && request.streams == ChildStreams::discard) {
        error = posix_spawn_file_actions_addopen(&setup.actions, STDOUT_FILENO, "/dev/null",
                                                 O_WRONLY, 0);
        if (error == 0)
            error = posix_spawn_file_actions_adddup2(&setup.actions, STDOUT_FILENO, STDERR_FILENO);
    }
    if (error == 0 && request.streams == ChildStreams::capture) {
        error = posix_spawn_file_actions_adddup2(&setup.actions, output_writer, STDOUT_FILENO);
        if (error == 0)
            error = posix_spawn_file_actions_adddup2(&setup.actions, output_writer, STDERR_FILENO);
    }
    if (error != 0) return error;

    sigset_t defaults{};
    (void)sigemptyset(&defaults);
    for (const int signal_number : reset_signals) (void)sigaddset(&defaults, signal_number);
    sigset_t empty{};
    (void)sigemptyset(&empty);
    error = posix_spawnattr_setsigdefault(&setup.attributes, &defaults);
    if (error == 0) error = posix_spawnattr_setsigmask(&setup.attributes, &empty);
    if (error != 0) return error;
    short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
#if defined(POSIX_SPAWN_SETSID)
    if (request.detached) flags = static_cast<short>(flags | POSIX_SPAWN_SETSID);
#else
    // Without POSIX_SPAWN_SETSID, a new process group still keeps terminal
    // signals from reaching a detached child.
    if (request.detached) flags = static_cast<short>(flags | POSIX_SPAWN_SETPGROUP);
#endif
    error = posix_spawnattr_setflags(&setup.attributes, flags);
    return error;
}

} // namespace

Child::Child() noexcept = default;

Child::Child(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

Child::Child(Child&& other) noexcept : state_(std::move(other.state_)) {}

// A child not yet waited for is reaped now if it has ended, and otherwise
// handed to the abandoned list.
Child::~Child() {
    if (!state_) return;
    State& state = *state_;
    if (state.output >= 0) (void)::close(state.output);
    if (state.waited) return;
    int status = 0;
    if (::waitpid(state.pid, &status, WNOHANG) != 0) return;
    Abandoned& list = abandoned();
    const ScopedLock lock(list.lock);
    list.children.push_back(state.pid);
}

Child& Child::operator=(Child&& other) noexcept {
    if (this == &other) return *this;
    Child released(std::move(*this));
    state_ = std::move(other.state_);
    return *this;
}

CapabilityValue<Child> Child::start(const ChildRequest& request) {
    if (!children_detail::valid_request(request))
        return CapabilityValue<Child>::failure(CapabilityStatus::invalid_request);
    reap_abandoned();

    Descriptor reader{};
    Descriptor writer{};
    if (request.streams == ChildStreams::capture) {
        int ends[2] = {-1, -1};
        if (::pipe(ends) != 0) return CapabilityValue<Child>::failure(status_from_errno(errno));
        reader.value = ends[0];
        writer.value = ends[1];
        // The parent's ends must not leak into this or any other child; the
        // child receives the writer through dup2, which clears the flag.
        (void)::fcntl(reader.value, F_SETFD, FD_CLOEXEC);
        (void)::fcntl(writer.value, F_SETFD, FD_CLOEXEC);
    }

    SpawnSetup setup{};
    const int prepared = prepare(setup, request, writer.value);
    if (prepared != 0) return CapabilityValue<Child>::failure(status_from_errno(prepared));

    std::string program = request.program;
    std::vector<std::string> arguments = request.arguments;
    std::vector<char*> argv{};
    argv.reserve(arguments.size() + 2);
    argv.push_back(program.data());
    for (std::string& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);

    pid_t pid = -1;
    const bool has_slash = program.find('/') != std::string::npos;
    const int error = has_slash ? posix_spawn(&pid, program.c_str(), &setup.actions,
                                              &setup.attributes, argv.data(), environ)
                                : posix_spawnp(&pid, program.c_str(), &setup.actions,
                                               &setup.attributes, argv.data(), environ);
    if (error != 0) return CapabilityValue<Child>::failure(status_from_errno(error));

    std::unique_ptr<State> state(new (std::nothrow) State());
    if (!state) {
        // Nothing can own the child; end and reap it rather than leak it.
        (void)::kill(pid, SIGKILL);
        int status = 0;
        (void)::waitpid(pid, &status, 0);
        return CapabilityValue<Child>::failure(CapabilityStatus::resources_exhausted);
    }
    (*state).pid = pid;
    (*state).output = reader.release();
    return CapabilityValue<Child>::success(Child(std::move(state)));
}

CapabilityValue<ChildEnd> Child::wait(const MonotonicClock::duration timeout) noexcept {
    if (!state_ || (*state_).waited) return CapabilityValue<ChildEnd>::failure(CapabilityStatus::invalid_state);
    State& state = *state_;
    reap_abandoned();
    const MonotonicClock::time_point deadline = children_detail::deadline_after(timeout);
    MonotonicClock::duration pause = std::chrono::microseconds(200);
    for (;;) {
        int status = 0;
        const pid_t result = ::waitpid(state.pid, &status, WNOHANG);
        if (result < 0) {
            if (errno == EINTR) continue;
            return CapabilityValue<ChildEnd>::failure(status_from_errno(errno));
        }
        if (result == state.pid) {
            state.waited = true;
            if (WIFSIGNALED(status))
                return CapabilityValue<ChildEnd>::success(
                    ChildEnd::signalled(static_cast<std::uint32_t>(WTERMSIG(status))));
            return CapabilityValue<ChildEnd>::success(
                ChildEnd::exited(static_cast<std::uint32_t>(WEXITSTATUS(status))));
        }
        const MonotonicClock::time_point now = MonotonicClock::now();
        if (now >= deadline) return CapabilityValue<ChildEnd>::failure(CapabilityStatus::timed_out);
        const MonotonicClock::duration left = deadline - now;
        sleep_for(left < pause ? left : pause);
        if (pause < std::chrono::milliseconds(10)) pause *= 2;
    }
}

CapabilityStatus Child::kill() noexcept {
    if (!state_ || (*state_).waited) return CapabilityStatus::invalid_state;
    if (::kill((*state_).pid, SIGKILL) != 0) return status_from_errno(errno);
    return CapabilityStatus::ok;
}

CapabilityStatus Child::read_output(const std::size_t limit, std::string& output) {
    if (!state_ || (*state_).output < 0) return CapabilityStatus::invalid_state;
    State& state = *state_;
    std::array<char, 4096> chunk{};
    std::size_t received = 0;
    bool over_limit = false;
    for (;;) {
        const ssize_t count = ::read(state.output, chunk.data(), chunk.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            return status_from_errno(errno);
        }
        if (count == 0) break;
        // `received` counts only kept bytes, so it never exceeds `limit`.
        const std::size_t bytes = static_cast<std::size_t>(count);
        if (!over_limit && bytes <= limit - received) {
            output.append(chunk.data(), bytes);
            received += bytes;
        } else {
            over_limit = true;
        }
    }
    (void)::close(state.output);
    state.output = -1;
    if (over_limit) return CapabilityStatus::too_large;
    return CapabilityStatus::ok;
}

} // namespace stx_auxlib
