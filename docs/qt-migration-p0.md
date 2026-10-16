# Qt migration: concrete first consumer (0.14.0)

The supplied 0.12 assessment is historical. The existing 0.13 foundation first
removed the Core -> FileProvider dependency; this implementation preserves that
direction while completing the assessment's first implementation scope.

| Layer | Implemented contract | Actual consumer |
|---|---|---|
| Core Contracts | Owned bytes/values, strict UTF-8, explicit UTF-16/native paths | iiCSMIDI's byte/path API; independent text consumers |
| Core Runtime | Tracked result/stop/deadline/effects and scoped explicit reply execution | iiCSMIDI::decodeAsync; serial/fixed-thread race tests |
| iiJson | Bounded UTF-8 JSON grammar, exact integer ranges, explicit DTO codecs | MIDI embedded authorship input validation |
| FileProvider Native 0.8 | Exclusive complete-file creation, optimistic update, file removal and presentation | MidiFile create/edit/delete |
| iiCSMIDI 0.3 | Qt-free public types, strict SMF + existing author schemas, optional Async component | Independent base and async package consumers |

iiJson is a codec module above Contracts, not a Core implementation dependency.
Native remains independent of Core and iiJson; its existing authorship serializer
and Unicode normalization remain authoritative for persisted account metadata.
The existing QueryPort behavior is unchanged. No Qt compatibility layer is added
to Core. MIDI document handling is distinct from live MIDI/UMP in the independent
iiDeviceMultimedia module.

Before replacing the old MIDI implementation, an executable linked to the existing
Qt 6.8.3 packages captured an authored SMF, its JSON and a representative
NamespaceJournal-shaped compact JSON with SHA-256. The fixtures are checked into
iiCSMIDI/tests/fixtures. Native MIDI must round-trip the authored SMF and metadata
byte-for-byte, including non-ASCII author data and the original schema version.
Sync's NamespaceJournal serializer/hash code is unchanged. iiJson's writer does
not promise Qt ordering/number/escape identity; future journal migration requires
separate byte/hash fixtures for all deployed schemas and an explicit transition.

The complete first-consumer boundary includes source/API checks, Qt-disabled
builds, native file behavior, existing-format fixtures, bounded/race/lifetime
tests, staged packages, independent installed consumers and binary dependency
inspection. The checks do not establish that all non-GUI SDKs are now Qt-free.
Current verification results and platform limits are in validation.md.

P1's first consumer now uses [common native process execution](process.md),
standard pipes and child-local environment/cwd in iiSDKBuilder 0.1.1. Remaining
consumer-driven work includes sockets, standard app directories and UTC contracts;
network/authentication/session migration; SQL connection/transaction/recovery;
BLE peripheral/advertising; streaming XML; image/document codecs and GUI adapters.
No empty modules or speculative execution-environment hierarchy are introduced.
