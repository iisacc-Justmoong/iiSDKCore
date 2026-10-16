# File creation outside the foundation

Since Core 0.13, Core neither imports iiFileProvider nor opens/creates regular
files. File generation, atomic saves and post-close details/preview scheduling
belong to [iiFileProvider CoreIO](../../iiFileProvider/CoreIO/README.md).
That optional adapter depends on Core and Native; the lower libraries remain
independently buildable and installable. Contracts, Values and Runtime import no
storage component.

Core retains the Channel byte-stream/runtime contract and the explicit
`IO/NativeFile.h` platform ownership-transfer extension. The adapter opens a
native file through iiFileProvider and transfers it to that extension. A
noexcept callback schedules presentation on successful writable close.
Core tests provision isolated resources through test-only fixtures; production
examples and atomic transaction assertions moved with the storage feature.

The common [ownership contract](../../iiFileProvider/docs/FILE_OWNERSHIP.md)
defines creation/publication policy. The mandatory Core foundation boundary,
installed consumers with storage discovery disabled, adapter integration and
workspace creation audit verify both dependency direction and file ownership.
See [foundation architecture](foundation-architecture.md) for exact migration.
