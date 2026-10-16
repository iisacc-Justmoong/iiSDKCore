# Text, bytes and native paths (0.14.0)

UTF-8 text uses std::string/string_view; binary data uses Bytes = vector<byte>
and ByteView = span<const byte>; native paths use filesystem::path. Container
choices remain standard C++23. No iiString/container or locale runtime is added.

`Core/Text/Text.h` adds `to_utf16`, `to_utf8`, `path_from_utf8`, `path_to_utf8`.
Text conversions default to strict rejection and return TextError with the input
offset. UTF-8 offsets/lengths are bytes; UTF-16 offsets/lengths are code units.
Astral characters occupy four UTF-8 bytes and two UTF-16 units. Neither count is
a grapheme index. Code must not reuse a former QString index as a byte position.

Explicit TextPolicy::replace replaces each invalid input byte or isolated UTF-16
code unit with U+FFFD. This deterministic per-unit policy does not promise Qt's
maximal-subpart replacement behavior. Valid text, combining sequences and NUL
are preserved without normalization, case folding or terminator truncation.
Normalization/extended encodings belong to specialized modules. Invalid UTF-8,
overlong encodings, surrogates, truncated sequences and values above U+10FFFF
are covered by `tests/text.cpp`.

Native filesystem paths retain native representation unless the caller requests
a conversion. UTF-8 path creation rejects invalid UTF-8 and embedded NUL; native
path conversion rejects paths that cannot be represented as valid UTF-8. POSIX
arbitrary-byte names can therefore remain filesystem::path values without being
mislabelled as text. Windows uses filesystem's char8_t/native conversion; no ANSI
code-page conversion is used. This performs no filesystem access or file creation.
URL parsing, normalization and percent encoding are separate URI/network work.

QueryPort copies a request but cannot own the data referenced by nested spans or
string_views. Callers transfer owned strings/Bytes or an explicit shared owner
across an asynchronous boundary. iiCSMIDI::decodeAsync accepts owned Bytes and
keeps them until the task capture is retired, including queued cancellation.
Returned document/value views borrow their lvalue owner and cannot be taken from
a temporary. Allocation errors remain standard exceptions.
