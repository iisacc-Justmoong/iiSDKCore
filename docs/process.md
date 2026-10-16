# Native process ownership (0.15.0)

`<iiSDKCore/Process/Process.h>` belongs to the compiled Runtime target. It
provides argument-vector execution, child-local environment/cwd, standard pipes,
exit status and owned-tree cancellation using OS process APIs. It has no Qt,
file-provider, command shell, interpreter or per-process worker dependency.
iiSDKBuilder 0.1.1 is the first consumer. Applications can use the same API for
native helpers; a platform refusing process launch returns `unsupported`.

```cpp
#include <iiSDKCore/Process/Process.h>

using namespace ii::process;
Launch launch;
launch.arguments = {"cmake", "--version"}; // argv[0], then literal arguments
launch.input = Input::closed;
launch.error = ErrorOutput::merge_output;
launch.environment["EXAMPLE_MODE"] = "build";
launch.environment["UNWANTED_VARIABLE"] = std::nullopt;
// launch.working_directory = an_existing_native_path;
auto child = Process::start(launch);
if (!child) { /* spawn/validation failure, with a system_error cause */ }
else {
    CommunicationOptions options;
    options.deadline = ii::core::Deadline::after(std::chrono::seconds(30));
    auto result = child->communicate({}, options);
    // Check result, exit.reason, exit.code/signal and truncation flags.
}
```

## Input and execution environment

Arguments and explicit environment keys/values require valid UTF-8 and reject
embedded NUL. `working_directory` is a native `std::filesystem::path`; empty
inherits, a relative path is relative to the host cwd at launch. The child alone
changes directory. The host must coordinate any external process-global cwd or
environment mutations while snapshotting/launching. Core never calls chdir,
setenv, putenv or SetEnvironmentVariable on the host.

`current_environment()` returns an owned snapshot. POSIX inherits existing
native environment bytes without a Unicode rewrite; explicitly supplied values
are UTF-8. Windows snapshots UTF-16 into UTF-8 and builds a case-insensitively
sorted Unicode environment block, preserving native drive-cwd entries. Windows
overrides/removals match names case-insensitively; POSIX names are case-sensitive.
`inherit_environment=false` begins with an empty environment. Empty string and
unset are distinct. A snapshot contains the host's values; callers decide what
they may expose or log.

Executable lookup uses the child's effective PATH and cwd. POSIX uses
`/usr/bin:/bin` if PATH is absent; an empty PATH component searches child cwd.
Windows searches child cwd followed by its PATH, adding `.exe` for a bare name.
An executable containing a directory is resolved in child cwd. Arguments are
passed literally: shell substitutions, redirection, globbing and `.bat`/script
command parsing are not performed. Windows quoting follows the CRT argv rules;
a child with a custom command-line parser owns that different interpretation.
No ENOEXEC shell fallback is used. Windows retains the original argv[0].

## Ownership, exits and cancellation

`Process` is a move-only RAII owner. The implementation keeps identity until
cleanup: a POSIX child is observed with WNOWAIT, remaining group members are
killed, and only then is the direct child reaped. Windows creates a suspended
child, assigns it to a kill-on-close Job Object before resume, and uses an exact
inherited-handle whitelist. Unrelated inheritable descriptors/handles are not
passed to the child. Closed host stdin/stdout/stderr are supported when the
corresponding child stream is piped or discarded; explicit inheritance of an
invalid host stream fails.

`poll()` and `wait_for()` return an optional stable terminal `Exit`.
`wait_for()` timing out returns no exit and leaves the child running; it does not
drain pipes. `cancel()` kills the owned tree and waits/reaps the direct child;
repeated calls return the original exit. `communicate()` cancellation/deadline
returns partial output and `cancelled`/`timed_out`. Nonzero normal exits and POSIX
signal exits are outcomes, while validation/spawn/wait/pipe failures are
`core::Error`, retaining a `system_error` exception cause for native failures.
Windows exit codes retain all 32 bits. `id()` is diagnostic, not a transferable
ownership capability. An already observed exit wins over later cancellation.

Destruction and move-assignment retire the previous owned tree, reap the direct
child and close all standard-channel aliases. A normal root exit also kills
remaining owned descendants: this is a structured lifetime, not detached launch.
There is no detach or graceful-termination promise. SIGKILL/TerminateJobObject
is deliberate for builder cancellation, and does not roll back external effects.
A POSIX descendant that deliberately leaves the process group is outside this
contract; this is not a security sandbox. Orphan descendant reaping belongs to
the OS; Core reaps its direct child. The host must not use SIGCHLD=SIG_IGN,
SA_NOCLDWAIT, or competing waits to reap Core-owned children. If identity was
lost, Core reports the wait failure and never signals a possibly reused PID.

Control calls serialize internally. Only one `communicate()` can run at a time;
concurrent admission returns `busy`. Object movement/destruction must not race
member calls. Cancel and drain externally submitted Channel waits/transfers
before removing their Process owner; an active Channel operation can return
busy to close. Pipe transfers belong to one consumer: do not drain through
Channel/EventLoop while communicate is using the same endpoint. Callback code
must not reenter communication or destroy the process owner.

## Pipes, bounded capture and runtime composition

Defaults are three pipes. Input can instead be closed (the OS null input) or
inherited; stdout/stderr can be discarded or inherited; stderr can merge into
stdout's actual OS endpoint. An unpiped/merged endpoint returns an empty Channel.
`input()/output()/error()` are owning Channel aliases. Closing one closes all
aliases, including the owner, and explicit pipe closure during communication is
an error. Native handles remain in the implementation or the explicit platform
extension `io::NativePipe::adopt`.

NativePipe takes ownership even on validation/allocation failure, verifies a
pipe/FIFO, and configures nonblocking transfers. It cannot adopt a regular file;
NativeFile retains its regular-file-only contract. A pipe is nonseekable and
nonflushable, with partial transfer, would_block, EOF and closed-peer results.
POSIX writes suppress newly generated SIGPIPE without changing the host's signal
handler or consuming a previously pending signal. Windows uses byte-mode named
pipes with PIPE_NOWAIT. Read readiness includes EOF. Windows writable readiness
is an advisory probe; a following nonblocking write can still return would_block.

`communicate` pumps stdin/stdout/stderr fairly on the calling thread, closes input
at the end of the supplied bytes, and drains outputs through EOF. Binary NUL is
preserved. Each capture defaults to 8 MiB, with explicit truncation flags; limits
of zero disable capture. The optional synchronous sink still receives every
chunk after capture fills. Input_closed_early records a peer closing stdin before
all bytes were sent. A throwing sink or allocation failure retires/reaps the tree
before returning an error with its exception cause. Sink code must return
promptly; Core cannot preempt application callback code. After root cleanup,
external escaped pipe holders have a two-second EOF drain limit, then failure
is reported rather than hanging indefinitely or claiming complete capture.

No thread is created by Process. Run blocking communication on an existing
Runtime worker, compose with OperationOwner/TaskScope, or drive the pipe Channels
through EventLoop::watch/read/write. Do not block a GUI dispatch thread. The
existing read/write buffer ownership and readiness cancellation contracts apply.
A process wait alone can block a full-pipe writer: use communicate or concurrently
pump all enabled endpoints. Process destruction is the terminal resource boundary.

Core creates pipe handles and opens OS null devices; it never creates application
log/document files. Builder's streaming log is owned and presented by
`iiFileProvider::native::OutputFile`. Its sink sets capture limits to zero, so
build output does not accumulate in Core memory.

## Platform contract and evidence

| Target | Compiled implementation |
|---|---|
| macOS 10.15+ | posix_spawn, child cwd file action, CLOEXEC_DEFAULT, process group, waitid/waitpid |
| Linux | POSIX spawn when libc exposes child cwd and closefrom file actions (for example glibc 2.34+); otherwise capabilities are false |
| Windows | CreateProcessW, Unicode environment/cwd, named byte pipes, exact handle whitelist, owned Job Object |
| Android | Same native POSIX path when NDK API level exposes cwd actions and CLOEXEC_DEFAULT (API 34+); lower configured API reports unsupported |
| iOS | Process launch and process pipes report unsupported; no interpreter bridge |
| WASM | Process launch and process pipes report unsupported, including under Node; no host child_process bridge |

Capability is a compiled OS-API contract, not proof that a sandbox/permission
policy permits a particular executable. Cross-compilation is distinct from
execution. Actual local, sanitizer and installed-consumer results are recorded
in [validation](validation.md).

Implementation references: [Microsoft redirected child I/O](https://learn.microsoft.com/en-us/windows/win32/procthread/creating-a-child-process-with-redirected-input-and-output),
[Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects),
[pipe wait modes](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-type-read-and-wait-modes),
and the target SDK's spawn.h/unistd.h/wait.h declarations. The build checks
actual POSIX extension availability instead of substituting a shell runtime.

The communicate input view remains borrowed for the complete call. An
asynchronously submitted caller must capture owning input bytes for that lifetime.
