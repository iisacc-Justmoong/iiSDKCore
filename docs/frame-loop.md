# App and game frame loop (0.11.0)

`FrameLoop` adds ordered frame callbacks and a bounded fixed timestep to native
`EventLoop`. Event-driven apps can keep EventLoop alone. Animation, interactive
tools and games can compose FrameLoop with the same task/query/I/O execution
domain. Independent computation still runs on `Runtime` workers; frame callbacks
run serially on their host thread. No Qt, engine, managed VM or interpreter is
introduced. Core owns timing/ordering; consumers own UI, physics and rendering.

`Application::frame_loop()` provides an application-owned instance for both apps
and games. `TickOptions::interval == 0` delivers once per eligible frame phase;
an interval throttles at frame boundaries and never creates catch-up bursts.
`FrameLoop::close()` rejects registrations, revokes not-yet-started callbacks and
retires captures. It is idempotent and safe from a tick; the executing capture is
released only after its invocation returns. Application teardown closes this loop
before detaching owned views.

A display host drives `step` using native presentation-target timestamps. The
explicit timer-based `run` remains useful for headless simulation, but a 16 ms
timer is neither 60 Hz nor vertical synchronization. Fixed updates remain bounded
and independent of variable display cadence; scaled gameplay can pause while
unscaled UI callbacks continue. Display callback rate, rendered scenes and physical
screen presentations are distinct measurements.

## References

Unity's [Player loop](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/LowLevel.PlayerLoop.html)
and [execution order](https://docs.unity3d.com/Manual/execution-order.html)
describe ordered subsystem updates. Its
[time management](https://docs.unity3d.com/2022.3/Documentation/Manual/TimeFrameManagement.html)
separates variable/fixed updates, scaled/real time and bounded elapsed time.
Unreal's [actor ticking](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-ticking-in-unreal-engine)
describes tick groups, prerequisites and intervals. These inform requirements,
not API or implementation templates. There is no Actor/MonoBehaviour hierarchy,
reflection registry, global player loop or automatic engine initialization.

## Stages and prerequisites

| Stage | Frequency | Consumer responsibility |
| --- | --- | --- |
| `begin` | Once per frame | Apply frame-local state/start measurements |
| `input` | Once per frame | Consume input collected by the host |
| `before_fixed` | Per fixed step | Prepare simulation inputs |
| `fixed_update` | Per fixed step | Perform a constant-duration update |
| `after_fixed` | Per fixed step | Consume its results |
| `update` | Once per frame | App/game variable-time work |
| `late_update` | Once per frame | Follow-up work after updates |
| `prepare_present` | Once per frame | Prepare display data |
| `present` | Once per frame | Call consumer presentation |
| `end` | Once per frame | Finalize the frame |

Fixed stages form a complete triplet per step and can run zero or several times
between input and update. `present` neither submits GPU work nor guarantees pixels
on screen. `add_dependency(dependent, prerequisite)` reuses `DependencyGraph`.
Cycles and prerequisites in later phases are rejected. Fixed-stage ordering
applies per step. Edges order eligible deliveries; they do not demand execution
of disabled, interval-limited or paused ticks. Independent callbacks follow the
graph's deterministic topological order. Express an edge when order matters.
There is no automatic barrier for async jobs; post completed results through
EventLoop/QueryPort before consuming them in a frame.

## Time

`step(timestamp)` accepts monotonic steady-clock timestamps for deterministic
tests/offline consumers. The first frame establishes the clock with zero delta.
Earlier timestamps throw `invalid_argument` without advancing state.
`reset_clock(timestamp)` rebases after host discontinuities, clearing fractional
backlog while preserving elapsed time and counters.

Raw host delta accumulates into `unscaled_elapsed`. Pause/time scale then apply,
and scaled delta is capped by `max_delta` (default 250 ms). The fixed accumulator
runs at most `max_fixed_steps` (default four), discarding excess whole steps and
carrying only the fractional remainder. Each fixed triplet receives
`delta == fixed_step`. `interpolation` is remainder/fixed step, in [0, 1).
`elapsed` is accepted scaled frame time; `fixed_elapsed` advances only by executed
fixed steps. They intentionally diverge under overload. `fixed_tick` counts steps;
`frame` counts attempted frames. In fixed callbacks `unscaled_delta` still means
the host frame's raw delta. Fractional scaling rounds down to integral clock ticks.
Time arithmetic saturates instead of overflowing.

Default `TickClock::scaled` callbacks stop while paused or at zero scale. Fixed
stages require this clock. `TickClock::unscaled` callbacks run with raw real time
during pause, for menus, UI, input and presentation. `set_paused()` and finite
nonnegative `set_time_scale()` apply next frame, including when called from input.
They do not suspend event work, timers, queries or I/O. EventLoop suspension stops
event and frame dispatch; on resume the next delta includes the host gap. Call
`reset_clock()` when that gap must be excluded; simulation clamping still applies.

A tick `interval` is minimum time on its chosen clock (fixed elapsed time for
fixed stages). First eligible delivery is immediate, including zero-delta first
frames. Later deliveries receive accumulated time since last execution; missed
intervals coalesce into one call. Reenabling preserves last-delivery time. Use
interval zero for constant-step integration; positive fixed-tick intervals can
deliver multiple steps' accumulated delta.

Reports/stats expose callbacks/steps, dropped steps, clamped scaled time, failures
and actual frame work. `frame_budget` (default 16 ms) measures overruns without
preempting callbacks or skipping presentation. `PumpBudget` separately bounds event
dispatch in `poll()`. Neither promises hard real-time behavior. Move long blocking
work to Runtime/AsyncIO.

## Frame cadence and timer deadlines

An app and a game use the same `Application::frame_loop()`. Register an
`update` tick with `interval == 0` for one delivery per eligible host frame.
Select `TickClock::unscaled` when UI or input must continue during simulation
pause. The display host supplies timestamps to `step`; do not also start
`FrameLoop::run()` or a second periodic frame timer on that loop.

| Work | Contract | Time source |
| --- | --- | --- |
| UI animation or variable game update | One eligible callback per host frame | Display host timestamps, through `FrameLoop::step` |
| Physics or deterministic simulation step | Zero or several constant-duration updates per host frame | `FrameLoopOptions::fixed_step`, bounded catch-up |
| Debounce, autosave, timeout or periodic service work | Deadline-based dispatch independent of display frames | `EventLoop::at`, `after` or `every` |
| Frame-bound throttled work | At most one callback per host frame, after the minimum interval | `TickOptions::interval` on its selected clock |

A 16 ms timer nominally requests 62.5 callbacks per second, not 60. A timer
set to approximately 16.67 ms still does not synchronize with the display.
When a host changes from 60 to 120 or 144 Hz, interval-zero frame callbacks
follow the supplied frame cadence without changing a timer interval. Their
`delta` reflects the elapsed host time; game motion must use it rather than
assume a fixed amount of motion per frame. A 10 ms fixed simulation step
continues to request 100 simulation steps per elapsed second, subject to
pause, time scale, clamping and the configured catch-up limit.

Positive tick intervals are minimum delays quantized to host frames, not exact
frequency guarantees. A late host frame coalesces missed variable-tick intervals
into one delivery. Fixed-step catch-up has its separate bounded policy. For an
event-driven static app, omit the display frame source until continuous updates
are needed; iiGUI can also avoid GPU submissions when frame callbacks leave the
scene unchanged.

Count display callbacks, scene renders, GPU submissions/presentations and actual
physical scanout separately. A successful `present` phase or a timer count alone
does not establish visible FPS. Scheduling, occlusion, system load and the
renderer can reduce delivery or presentation frequency.

`tests/frame_loop.cpp::host_cadence_and_display_changes` feeds deterministic
60, 120 and 144 Hz timestamps, then switches those cadences on one loop. It checks
one variable update per host frame, an unchanged 100 Hz fixed simulation,
frame-bound throttling without bursts and zero reserved EventLoop timer slots.
This proves the Core scheduling contract; it is not a measurement of a 120/144 Hz
monitor. Native display delivery and unchanged-scene presentation coalescing are
verified separately by iiGUI's frame-host tests.

## Ownership and mutation

FrameLoop is constructed/used on one host thread; registration, controls, stats
and drivers check affinity and throw `logic_error` elsewhere. Other threads post
controls/results through EventLoop or use its thread-safe `request_stop()`.
Slots and the frame schedule are preallocated. Graph edits/arbitrary captures may
allocate; stable warmed-up frames do not. `tests/reuse.cpp` measures ordinary
operator-new calls over 10,000 frames, not whole-application allocation freedom.
Tokens identify graph owner/slot/generation; foreign/stale tokens are rejected.

Each frame snapshots order/options. Additions, dependency/enable changes and clock
controls apply next frame. Removal immediately suppresses deliveries not started;
self removal retires after return. Captures are destroyed before returning slot
capacity. Capture destructors may adjust other registrations but cannot advance
or destroy the loop. Throwing ticks retire before their exception propagates.
Referenced consumer state must outlive its callbacks; remove those registrations
on the host thread before destroying that state.
Remaining fixed backlog is discarded; other registrations survive, without
replaying failed frames or rolling back consumer effects. `end` can be skipped on
failure/suspend/stop and is not an exception finalizer. Stats still finalize.
Stopping EventLoop suppresses subsequent frame callbacks. Destroy FrameLoop after
stopping its driver to release captures. It does not stop the external EventLoop
or remove other owners' work. Self/concurrent destruction during dispatch is invalid.

## Drivers

- Native `frames.run()` reserves one periodic EventLoop slot and uses existing
  waiting/wakeup scheduling without busy spinning. Failed reservation is an error;
  the timer retires on return/exception. Stop via `events.request_stop()`.
- Existing UIKit, Android or desktop GUI frame callbacks call `frames.poll()`
  after collecting input. Event/I/O work executes before the frame. It never
  sleeps or creates another platform main loop.
- Inside EventLoop dispatch, use `frames.step(timestamp)` for frame-only work,
  avoiding recursive event polling.
- Emscripten `BrowserLoop browser(frames)` uses requestAnimationFrame. Exceptions
  are stored for `rethrow_if_failed()` and never cross the browser C boundary.
  All three objects outlive the browser callback. Hidden-tab throttling applies.

The five OS targets use compiled native C++. WASM is a separate WebAssembly target
executed by its host, not native-machine-code proof for those OSes. No common VM
is required for native builds. Compile/link, Node, browser and device execution
are different evidence. `examples/frame_loop.cpp` composes real-time UI, fixed
simulation, interpolation and stop. `tests/frame_loop.cpp` covers these contracts
and runs against both source and installed packages.

## External-host binding (0.16)

HostLoop pins EventLoop dispatch to the external owner and forwards direct Core submissions/cancellation to its native wake gate. For iiGUI, borrow Host::event_loop on the open UI owner, construct FrameLoop there, and drive step from UiExecutor timers or the host frame callback. Never invoke FrameLoop::poll inside existing dispatch or start another run driver. Keep Host alive until FrameLoop is destroyed; remove/destroy frame captures after run returns, not from inside their own tick. Window/GPU presentation remains in iiGUI. See [host-loop.md](host-loop.md).
