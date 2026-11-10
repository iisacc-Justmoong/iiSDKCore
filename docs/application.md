# Native application ownership and final executable

`iiSDKCore/Runtime/Application.h` provides the application owner for native SDK
programs. Core owns one EventLoop, one shared Runtime worker pool, an owned
TaskScope, a cooperative stop token, arguments, exit status and named native
attachments. It does not include or link iiGUI, LVUI, LVRS, Qt or an OS widget API.

The default application allocates no worker threads or worker task slots.
`Application::post` rejects worker work with `overloaded` until the owner
explicitly sets `ApplicationOptions::workers` or calls `workers().resize_workers()`.
The EventLoop remains available for explicitly submitted base work. Allocate
workers before attaching a host context that cannot wait for worker changes.

`Application::frame_loop()` creates the frame state on first access on the
application owner thread; `has_frame_loop()` reports whether it exists.
It owns the same native frame phases for apps and games,
on the application's EventLoop. `ApplicationOptions::frames` configures fixed-step,
delta clamp, callback capacity and catch-up budget. Registration alone starts no
timer or display polling. A native display host can call `step(timestamp)` once
per display callback; a headless driver can use `poll` or the explicit timer-based
`FrameLoop::run`. A duration timer is not a display synchronization source.
During shutdown Core retires frame callback captures after workers finish and
before attachments deactivate, so they cannot retain a view/backend past detach.

An upper module supplies an `ApplicationDriver` through ordinary C++ callbacks.
Its `run(start)` invokes `start` exactly once when native attachment is safe and
returns after the native host has closed. It must not retain start. Core activates
attachments in declaration order; shutdown retires owned work and deactivates
attempted attachments in reverse order, including partially failed activation.
The first error is preserved while later cleanup continues. A missing driver,
duplicate attachment, duplicate start, wrong owner or a second exec is rejected.

## Native driver completion before loop return

Some operating systems keep their process event loop alive after a native host
has closed. After the host has stopped delivering callbacks, its owner-thread
adapter may call `Application::driver_stopped()` while `run(start)` is still on
the stack. Core stops admitting work, joins owned workers, retires frame callbacks
and deactivates attempted attachments in the same order as ordinary `exec()`
shutdown. The operation is idempotent, preserves the first failure and does not
itself terminate the operating-system process or return from its event loop.

If completion is signaled inside an attachment's activation, Core finishes that
activation, does not activate later attachments, and then retires the attempted
owners. Completion from another thread or before the driver has entered is
rejected. A stopped application cannot be started again. Drivers must arrange
delivery after native callbacks and in-flight uses that require the owners have
retired; this function does not provide a native callback drain.

The owner-thread, reentrant and first-error cases are covered by the application
lifecycle tests. The same cases also passed an AddressSanitizer and
UndefinedBehaviorSanitizer build. The iPhoneOS arm64 library and a consumer of
this API have been compiled and linked; that does not establish device behavior.

## Atomic configuration contract

`Application::configure_atomically` groups driver and attachment registration on
the application owner thread. On success it publishes all registrations. On a
returned error or thrown exception, it removes registrations made within that
call, preserves registrations that preceded it, and returns the original error or
an error containing the exception. Nested transactions and `exec()` during a
transaction are rejected. Destruction of rolled-back captures cannot reenter
configuration. The transaction covers application registration only: it does
not undo caller-owned I/O, worker submissions, or other external side effects.
No attachment is activated during configuration; its phase determines whether it
starts before the driver or when the driver signals readiness.

## Owners before the native driver

`attach_before_driver` registers an owner whose activation runs on the
application thread before the native driver starts. Existing `attach` records
continue to activate only when the driver invokes `start`. Both kinds share a
single name namespace and the same atomic configuration rollback. Activation
order is registration order within each phase; shutdown first retires work and
frames, then deactivates attempted driver-ready records and finally attempted
before-driver records, each in reverse activation order. A failed activation is
also deactivated, later records and the driver are not entered, and the first
error is retained. A missing driver leaves the application ready without activating
either phase. No native window or GUI concept is introduced into Core.

`Application::post` submits to the shared worker pool through its owned TaskScope.
On exit Core requests cooperative stop, stops event admission, cancels pending
owned work, waits for active work and joins workers before attachment deactivation.
Active work must cooperate with stop or finish; no C++ thread is forcibly killed.
`Application::workers()` borrows that same Runtime for existing SDK APIs such as
SerialQueue and Concurrent; it does not construct another pool. Direct submissions
to it remain subject to Core's runtime shutdown/join barrier.
External runtimes and unowned work remain the caller's responsibility. Lifecycle
methods and destruction require the construction thread. `stop_token`, `post`
and the EventLoop's weak Sender provide worker communication. The application
owner must outlive callers; a borrowed Application reference is not a weak handle.

## The final executable

```cmake
find_package(iiSDKCore 0.18 CONFIG REQUIRED COMPONENTS Runtime)
iisdkcore_add_application(MyApp SOURCES application.cpp)
```

Core's installed `ApplicationEntry.cpp` supplies `main`. The application supplies
`ii::runtime::configure_application(Application&)`, returning Result<void> after
configuring a driver and attachments. Core then calls exec, propagates errors
and returns the application exit status. Configuration failure also retires owned
work. Standard native C++ compilation/linking produces the executable; there is
no language VM, interpreter or runtime code compilation.

`iisdkcore_add_application` accepts `GUI`, `SOURCES`, `LIBRARIES` and `RESOURCES`.
It enforces C++23 without compiler extensions and links Core Runtime. GUI targets
use a macOS app bundle or Windows GUI subsystem as appropriate. Resources are
attached to the macOS bundle; other targets receive a resources directory beside
the executable. The argument vector includes the executable name at index zero
when the platform provides it. On Windows the entry reads the native UTF-16 command
line, parses it with `CommandLineToArgvW` and converts every argument strictly to
UTF-8; malformed UTF-16 fails startup. The Windows entry links the Shell32 system
library. On other platforms it preserves the argument bytes supplied to `main`,
including empty arguments, without assuming an encoding. Parsing flags, deciding
which values represent files or URLs, registering file associations and production
distribution/signing are application responsibilities. The Windows entry follows
the documented [native command-line](https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-getcommandlinew)
and [argument parsing](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-commandlinetoargvw)
contracts; this does not establish the behavior of other entry mechanisms.

## SDK GUI programming

iiGUI's `attach_application` installs a native OS driver borrowing the exact Core
EventLoop. `view_attachment` adapts a native C++ object's attach/detach methods to
Core's generic attachment record. LVUI AOT emits an instantiable C++ widget class
with that interface. The same interface accepts an ordinary hand-written C++
widget; Core knows neither widget names nor declarative syntax.

```cpp
#include "MainView.generated.h"
#include <iiGUI/Application.h>

ii::core::Result<void> ii::runtime::configure_application(Application& app) {
    using View = lvui_generated::MainView;
    return app.configure_atomically([](Application& target) -> ii::core::Result<void> {
        auto host = ii::gui::attach_application(target, View::options());
        if (!host) return std::unexpected(host.error());
        return target.attach(ii::gui::view_attachment(
            "main", *host, std::make_shared<View>()));
    });
}
```

The GUI adapter owns platform window behavior and native controls. A closing
window revokes native handles immediately. The generated C++ widget retains its
imported C++ backend until Core finishes owned work and detaches that widget.
LVUI owns its native component behavior and appearance; Core has no dependency
on LVUI, LVRS, Qt or QML.

Tests in `tests/application.cpp` cover ordered lifetime, partial rollback, wrong
thread, atomic registration, reentrancy, single-use/driver contracts and worker retirement. `application_entry.cpp`
is compiled with the actual Core entry source. Core's full dependency and sealed
record checks also include these additions. Interactive GUI/runtime verification
belongs to iiGUI and does not establish Windows/Linux/mobile execution on a Mac.

On 2026-11-04 the macOS arm64 Core build and 69 selected headless tests passed,
including atomic commit, returned-error and exception rollback, pre-existing
registrations, nested execution rejection and capture-destruction reentrancy.
Two existing installation tests and three separate build/policy tests were not
run in that batch; the full build enforced Core's boundary and inheritance
checks, and an isolated staged Core was consumed by LVUI's relocated package
tests. The pre-existing Core install manifest was preserved.

Later on 2026-11-04 the macOS arm64 full build and 15 selected application,
text, foundation, consumer and boundary tests passed. The actual Core entry
preserved an empty argument and Unicode on macOS; a portable UTF-16 test covered
empty values, repeated arguments, surrogate pairs, invalid surrogates and null
inputs. The decoder test also passed with AddressSanitizer and
UndefinedBehaviorSanitizer. The Windows-specific entry branch received a syntax
check against simulated API declarations on macOS; this is not a Windows
compiler, link or execution result. The staged installed entry sources match
the source tree, and LVUI's relocated consumer compiled against that stage.
No Windows or Linux host execution or native file association was tested.

After adding before-driver owners, the Core build and 70 selected headless tests
passed, including nine application tests. The new phase test covers mixed
registration order, activation failure and reverse cleanup, absent drivers,
duplicate names, transaction rollback and wrong-thread registration. The same
two installation tests and three build/policy tests remained excluded; the
existing install manifest was preserved.
