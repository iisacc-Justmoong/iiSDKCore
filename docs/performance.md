# Runtime performance contracts

The event loop keeps a dense private index of live reservations. Steady-state
readiness, deadlines and dispatch inspect live slots rather than the configured
maximum capacity. Ordinary task retirement does not rescan real-time utilization;
real-time admission/retirement still recompute the exact reserved sum. The extra
index and per-slot position use two size_t values per capacity slot. Capacity,
backpressure, generational token identity, FIFO within priority, the 4:2:1 weighted
priority wheel and real-time EDF order remain unchanged. User captures still
retire outside loop locks. Stop retains its bounded one-time capacity traversal.

Normal dispatch collects the oldest eligible task per priority in one pass, then
advances the same priority wheel. Callback cancellation and slot reuse cannot
change FIFO order. The runtime regression covers a sparsely populated 65,536-slot
loop, non-tail cancellations, reused slots, mixed priorities and future deadlines.
Existing I/O, host, real-time, cleanup and concurrency tests remain applicable.

UTF-8 validation skips ASCII runs in alignment-safe eight-byte loads. Native byte
order does not affect the high-bit test; non-ASCII scalar validation keeps its
overlong, surrogate, truncation and range rejection. Tests compare all 65,536
possible two-byte inputs with UTF-16 conversion and cover invalid sequences across
word boundaries and embedded NUL. No platform SIMD extension or Qt is required.

Measured results and module-wide source review evidence are recorded under
`SDK/build/performance-20261020/`. These method measurements do not establish
whole-application frame rate or memory consumption.
