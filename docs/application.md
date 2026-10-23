# Native application ownership and final executable

`iiSDKCore/Runtime/Application.h` provides the application owner for native SDK
programs. Core owns one EventLoop, one shared Runtime worker pool, an owned
TaskScope, a cooperative stop token, arguments, exit status and named native
attachments. It does not include or link iiGUI, LVUI, LVRS, Qt or an OS widget API.

`Application::frame_loop()` owns the same native frame phases for apps and games,
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
the executable. CRT arguments are preserved as strings; target-specific argument
encoding and a production distribution/signing policy remain application concerns.

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
    auto host = ii::gui::attach_application(app, View::options());
    if (!host) return std::unexpected(host.error());
    return app.attach(ii::gui::view_attachment(
        "main", *host, std::make_shared<View>()));
}
```

The GUI adapter owns platform window behavior and native controls. A closing
window revokes native handles immediately. The generated C++ widget retains its
imported C++ backend until Core finishes owned work and detaches that widget.
LVRS supplies appearance data/fonts, without adding a Qt/QML runtime dependency.

Tests in `tests/application.cpp` cover ordered lifetime, partial rollback, wrong
thread, single-use/driver contracts and worker retirement. `application_entry.cpp`
is compiled with the actual Core entry source. Core's full dependency and sealed
record checks also include these additions. Interactive GUI/runtime verification
belongs to iiGUI and does not establish Windows/Linux/mobile execution on a Mac.
