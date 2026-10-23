# Validation records

## 앱·게임 공통 프레임 주기 추가 검증 — 2026-10-22

`Application::frame_loop()`와 기존 FrameLoop 구현을 재사용하여 표시 주기와 시간 기반
타이머의 계약을 구분했다. 공개 API나 런타임 구현은 추가 변경하지 않았으며,
[프레임 문서](frame-loop.md#frame-cadence-and-timer-deadlines)와
`tests/frame_loop.cpp::host_cadence_and_display_changes`를 보강했다.

| 검사 | 결과 | `build/` 아래 증거 |
|---|---|---|
| macOS ARM64 Release, 앱 수명·프레임·시간·작업자·메모리 재사용 | 13/13 통과 | `frame-cadence-question-tests.xml` |
| 재배치한 실제 LVRS 색상 갤러리의 Core 패키지로 빌드한 프레임 소비자 | 1/1 통과 | `frame-cadence-consumer/build/tests.xml` |

소비자는 `LVRS/build/native-color-consumer-final/relocated`의 설치 헤더와 동적 라이브러리를
참조하며 C++23, extensions OFF, `-Wall -Wextra -Wpedantic -Werror`로 컴파일했다.
Qt·상위 파일 공급자 패키지 검색은 비활성화하고 외부 include·라이브러리 환경 변수를
제거한 환경에서 구성·실행했다. 기본 `~/.local` 설치본은 교체하지 않았다.

결정적 타임스탬프로 60·120·144 Hz를 각각 1초 구동하고 같은 루프에서 차례로 전환했다.
각 표시 프레임에 가변 업데이트가 한 번 실행되며, 10 ms 고정 업데이트는 초당 100회로
유지되었다. 전환 검사에서는 초기 기준 프레임을 포함해 325개 프레임·300개 고정 업데이트를
관측했고, 주기 EventLoop 타이머 예약은 0개였다. 양수 interval 콜백도 프레임 경계에서만
실행되며 누락된 간격을 연속 재생하지 않음을 검사했다.

이 결과는 Core 스케줄링 계약의 검증이며 120/144 Hz 모니터 실행이나 물리적 표시 FPS의
측정이 아니다. 실제 macOS 디스플레이 콜백·GPU 표시·변경 없는 장면의 제출 병합은
iiGUI의 별도 frame-host 검사 기록과 구분한다.

## 비동기 계산·QtConcurrent 대응 객체 — 2026-10-22

기존 Runtime·TaskScope 위에 Concurrent, Future, Promise, FutureWatcher,
FutureSynchronizer, TaskBuilder를 추가했다. 결과·진행률과 협력적 제어,
소유 입력의 병렬 map/filter 및 순서가 있는 순차 reduce를 제공한다.
Qt·상위 SDK 의존성이나 작업별 스레드를 추가하지 않는다.
공개 계약과 차이는 [concurrent.md](concurrent.md)에 정의한다.

| 검증 | 최종 결과 | `build/` 아래 증거 |
|---|---|---|
| macOS ARM64 Release/shared 전체 회귀·예제 | 48/48 통과 | `concurrent-full-tests.xml` |
| 새 Future·Concurrent·예제의 ASan + UBSan | 3/3 통과 | `concurrent-validation/asan/build/concurrent-tests.xml` |
| 같은 기능의 TSan | 3/3 통과 | `concurrent-validation/tsan/build/concurrent-tests.xml` |
| 경로를 옮긴 shared 설치 패키지의 독립 소비자 | 2/2 통과 | `concurrent-validation/final-package/consumer/build/installed-tests.xml` |
| Windows x64·Linux ARM64·WASM32 pthread의 테스트·예제 C++23 문법·템플릿 검사 | 9/9 통과 | `concurrent-validation/cross-syntax.json` |
| 공개 헤더 독립 컴파일·상속 정책·Core 의존성 경계 | 통과 | `concurrent-validation/final-build.log`, 전체 CTest 결과 |

두 테스트는 다중 결과의 순서, 빈 입력, move-only 데이터·관찰 캡처, 실제 네 워커의
병렬 실행, 함수·mapper·reducer 예외, 진행률 범위, 큐 포화, 실행기 콜백 폐기,
처리 중 취소, 정지·재개와 소유자 제거에 의한 깨우기를 검사한다. Watcher의
알림 병합·관찰 한도 재사용·포화 후 재시도·콜백 제거 장벽도 포함한다.
동기 편의 API는 워커에서 작업을 제출하기 전에 거절하는지 검사하여 호출자의
참조를 빌린 작업이 오류 반환 뒤에 남지 않도록 한다.

설치 소비자는 테스트 소스를 복사하여 원본 `src/`나 `~/.local` include 없이
재배치한 패키지만 사용한다. `-Wall -Wextra -Wpedantic -Werror`, C++23,
extensions OFF로 빌드하며 Qt·파일 공급자 패키지 검색을 비활성화한다.
네 공개 헤더가 원본과 일치하는지, Mach-O의 rpath와 실제 로더가 검증용 설치
라이브러리를 선택하는지 별도 확인한다. 패키지 증거는
`concurrent-validation/final-package/verification.json`, 전체 집계는
`concurrent-validation/verification.json`이다.

ASan 재검사 중에는 bounded Runtime 테스트의 latch가 직접 제출한 워커 작업보다
먼저 범위를 벗어나는 fixture 오류를 검출했다. latch 제거 전에 명시적으로
`wait_idle()`을 완료하도록 수정했고 assertion이나 sanitizer를 완화하지 않았다.
실패 기록은 `concurrent-validation/asan/build/concurrent-fixture-failure.xml`과
`.log`에 보존한다. 더 엄격한 크로스 컴파일러의 누락 초기화 경고도 공개 스냅샷의
멤버 기본 초기화로 수정했다. 최초 진단은
`concurrent-validation/cross-syntax-initial.json`에 남긴다.

네이티브 실행 증거는 macOS ARM64이다. Windows·Linux·WASM 결과는
`-fsyntax-only`에 의한 공개 템플릿 검사 범위이며 링크·대상 OS·브라우저 실행
증거로 해석하지 않는다. sanitizer의 검사 범위도 위의 새 기능 3개 테스트이다.

## 0.18.0 selective live query streams — 2026-10-16

QueryStream composes typed QueryPort control, bounded per-subscriber output rings,
TaskScope removal and existing executors. Its public C++23 API owns selected
outputs without copying a whole source. No new thread, global broker, Qt or upper
SDK dependency is added. The contract is in [query-stream.md](query-stream.md).

| Validation | Result | Evidence below build/ |
|---|---|---|
| macOS ARM64 Debug/static full regression and examples | 43/43 passed | query-stream/tests-final.xml |
| macOS ARM64 Release/shared full regression | 43/43 passed | query-stream-shared/tests-final.xml |
| ASan + UBSan full Core regression | 43/43 passed | query-stream-asan/tests.xml |
| TSan QueryStream, QueryPort, cleanup and parallel runtime | 4/4 passed | query-stream-tsan/tests-final.xml |
| Final example with FIFO producer lane, Debug/Release/ASan | 1/1 each | respective query-stream*/example-final.xml |
| Copied consumer against relocated shared Runtime/Values package | 19/19 passed | query-stream-package/latest.json identifies verification.json |
| Independent installed Contracts/text and Values consumers | 2/2 and 3/3 passed | same package run |
| Copied installed streaming example and full stream contracts | 2/2 passed | same package run, example/build/tests.xml |
| Six-platform Core -> installed streaming consumer compile/link chain | 6 kits, 30 steps passed | query-stream-matrix/matrix/report.json |
| Native macOS matrix example and stream contracts | 2/2 passed | query-stream-matrix/matrix/kits/macos/Release/app/build/matrix-tests.xml |
| Compiled WASM under Node: contracts, example and payload reentry probe | 3 runs passed | query-stream-matrix/wasm-*.log |

The stream suite exercises per-query filtering and lazy field selection, selected
zero versus omitted fields, revision replacement and a racing old projection,
async QueryPort control ownership, queue overflow/retry, external executor
cancellation, weak executor/producer removal, slot generation isolation, batched
fairness, move-only payloads, multiple producers, parallel and serial receivers,
native HostLoop wake/owner dispatch, self-close/would_block handling, and quota
retention until capture destruction. A separate red/green probe reproduced and
fixed payload move/destructor reentry under a queue lock; only owning pointers
now move under that lock. Runtime workers remain persistent and each
subscription's callbacks remain serialized even with a parallel executor.

All copied consumers compile with exact C++23, extensions off and
-Wall -Wextra -Wpedantic -Werror; compile commands exclude original source and
~/.local include paths. Package and example imports include no Qt/storage/GUI
runtime. Headers are verified against source after installation and the prefix
is renamed before consumer configuration/execution. The global SDK installation
and active product apps are preserved.

The ASan configuration initially made a deprecated macOS POSIX-spawn symbol
probe fail under global -Werror, which disabled the existing process branch and
exposed its unused fallback variable. The final configuration reruns that probe
with only deprecated declarations remaining warnings. Native process capability
and the complete process regression pass; no source fallback or test assertion
was weakened. The initial configure/build logs remain available. System code and
kernel memory are outside sanitizer instrumentation.

Windows x64 PE, Linux ARM64 ELF, Android ARM64 ELF, iOS ARM64 Mach-O and
macOS ARM64 Mach-O compile/link the same application and stream contracts through
iiSDKBuilder and their own target toolchains. The WASM32 image is compiled C++
with the existing pthread contract. Windows/Linux execution remains explicitly
deferred by the user; Android/iOS device execution/signing/deployment and browser
execution are not established by this matrix. Node executes the actual WASM
modules and explicitly passes the new payload-reentry command-line probe.
The example reports query revisions 1 -> 2 and zero unselected label reads.

Payload callbacks, moves/destructors and endpoint copies execute outside the
state lock. Queue storage remains bounded, while each matched event owns an
allocated node; allocation-free or hard real-time operation is not claimed.
The red timeout and green standalone probe remain under query-stream/. Aggregate
current-source/package/platform evidence is build/query-stream-verification.json.

## 0.17.0 native resource foundation — 2026-10-16

Core implements immutable aligned packs and native read-only Channel mappings.
Builder 0.2 owns the host compiler/target selection, and FileProvider owns output
publication. Core still exports only standard/platform dependencies.

| Validation | Result | Evidence below build/ |
|---|---|---|
| Native macOS ARM64 Debug/static regression, examples and architecture | 41/41 passed | resources/core-final-tests.xml |
| Native macOS ARM64 Release/shared regression | 41/41 passed | resources-shared/core-tests.xml |
| ASan + UBSan Core regression | 41/41 passed | resources-asan/core-tests.xml |
| TSan Resources, HostLoop and Operation | 3/3 passed | resources-tsan/core-tests.xml |
| Copied/rebased installed shared Runtime/Values consumer | 18/18 passed | resources-package/latest.json points to the run |
| Independent installed Contracts/text and Values consumers | 2/2 and 3/3 passed | same run, contracts-consumer and values-consumer XML |
| Existing native iiGUI host/GPU regression with Core 0.17 | 19/19 passed | ../iiGUI/build/resources-core/tests.xml |
| Builder host regression including resource compiler/incremental builds | 14/14 passed | ../iiSDKBuilder/build/resources/builder-final-tests.xml |
| Copied application + relocated shared Core and host tool/module | 1/1 passed; runs with source assets removed | ../iiSDKBuilder/build/resources-consumer/latest.json |
| Six-platform Core -> HelloResources compilation/install chain | 5 steps per kit passed | ../iiSDKBuilder/build/resources-matrix/matrix/report.json |
| macOS HelloResources and actual WASM module under Node | Executed successfully | ../iiSDKBuilder/build/resources-matrix/*-execution.log |

The duplicate-payload fixture uses two references to the same 4 MiB content.
Logical bytes are 8,388,608 and encoded bytes are 4,194,496, including 192 bytes
of metadata/alignment. Both views address the same native bytes. A 100,000-lookup
loop reports elapsed time without a hard performance assertion. These are
fixture measurements, not compression, process-RSS, GPU-memory or Qt comparisons.

Malformed input checks include every truncated extent, header changes, correctly
fingerprinted out-of-bounds/reserved/name metadata, invalid encoding, ranges,
budgets, and explicit payload integrity. Lifetime checks cover moved-from views,
channel/pack removal, mapping slices, shared readers and owning Operation results.
Native mapping/paging runs on macOS; compilation on other OSes is separate proof.

One concurrent-build ASan run timed out the Resources entry at 30 seconds without
a sanitizer diagnostic. The same binary passed the isolated entry in 1.27 seconds
and the complete rerun above passed 41/41, without relaxing its timeout or
assertions. The original XML is resources-asan/core-stress-timeout-tests.xml.
System frameworks/kernel paging are outside sanitizer instrumentation.

The Windows x64, Linux ARM64, Android ARM64, iOS ARM64 and WASM32 images compile
the same resource main.cpp and generated C++ data through their target toolchains.
Windows/Linux execution is explicitly deferred by the user. Android/iOS device
execution, signing and deployment are not established. Node's WASM execution is
not browser or physical-device evidence, and WASM reports OS mapping unsupported.
The native CLI remains a desktop build-host tool.

The SDK/Product file-creation audit records 2,509 runtime files, zero violations
and zero unreadable files in resources-file-ownership.json. New generated files
are published through FileProvider. Everything is installed into isolated
build/ prefixes; global SDK installs and running product apps are preserved.
Aggregate current-source/package/platform evidence is
build/resources-verification.json. API, binary format, costs and mapping limits
are documented in [resources.md](resources.md).

## 0.16.0 external-host foundation for iiGUI — 2026-10-16

Core remains below iiGUI. The exported Runtime target links only Contracts,
Threads and, where required, the native dynamic-loader library. No window,
font, graphics, Qt, storage or upper SDK package is imported. All builds use
C++23 with language extensions disabled; independent public-header and mandatory
inheritance/dependency checks run as part of the build.

| Validation | Result | Evidence below build/ |
|---|---|---|
| macOS ARM64 Debug regression, examples and architecture | 40/40 passed | gui-foundation/core-final-tests.xml |
| Core ASan + UBSan, including HostLoop and native pipe fixture | 40/40 passed | gui-foundation-asan/core-asan-final-tests.xml |
| TSan: HostLoop, Operation, Runtime, LoopIO and Cleanup | 5/5 passed; no race report | gui-foundation-tsan/core-tsan-tests.xml |
| Copied installed Runtime/Values consumer after prefix relocation | 17/17 passed | gui-package/latest.json locates its verification report and XML |
| Installed Contracts/text consumer, Threads/GUI discovery disabled | 2/2 passed | same package run, contracts-consumer/tests.xml |
| Installed Values-only consumer, no runtime target | 3/3 passed | same package run, values-consumer/tests.xml |
| Actual macOS iiGUI host, HelloGUI and relocated GUI consumer | 19/19, 1/1 and 1/1 passed | ../iiGUI/build/foundation-verification/macos/ |
| ASan + UBSan across both Core and iiGUI | 19/19 GUI checks passed | ../iiGUI/build/core-foundation-asan/sanitizer-tests.xml |
| Windows x64 and Linux ARM64 Core -> iiGUI -> HelloGUI via Builder | Built, installed and linked; relocated consumers compiled | ../iiGUI/build/foundation-verification/{windows,linux}/ |
| iOS ARM64, Android ARM64/API 34, WASM32 pthread Core | Static-library compilation passed | gui-ios-build.log, gui-android-build.log, gui-wasm-build.log |

The new Core tests verify weak producer expiry, bounded admission, direct token
cancellation notifications, coalesced wakes, owner-thread dispatch, inline/foreign
driver rejection, surviving connections after loop removal, detach barriers,
scoped deadlines, owned completion endpoints, nonblocking host observation and
FrameLoop phase ordering. A real child process acknowledges startup, waits for
release after I/O admission, then writes delayed pipe data. Host delivery must
precede one second against a three-second request timeout; no repeating host
timer drives this test. The startup handshake separates cold loader/sanitizer
startup from readiness latency. An initial pre-handshake timing failure during
concurrent builds led to that fixture correction; the final suites above use
the corrected protocol without relaxing the readiness limit.

iiGUI uses the same connection on AppKit, Win32 and X11; its separate native
tests cover window updates, final Core host counters, tracked completion,
scope cancellation, actual GPU frame preparation and terminal Core stop.
Counts include source/tooling checks and repeated configurations; they are not
counts of distinct UI features. System frameworks and GPU drivers are outside
sanitizer instrumentation.

Windows/Linux execution remains explicitly not_run at the user's request.
iOS, Android and WASM results here establish Core compilation only, not new GUI
backends, physical-device/browser execution or deployment. Emscripten was built
with CMAKE_CXX_SCAN_FOR_MODULES=OFF because that toolchain has no clang-scan-deps;
this project does not use C++ modules. Native GUI/runtime evidence is macOS only.
All packages and copied consumers stay under build/; the global SDK installation
and existing running product applications are preserved. The aggregate reports
are build/gui-integration-verification.json and
../iiGUI/build/core-integration-verification.json. See [host-loop.md](host-loop.md)
for the public API, managed-waiter cost and lifetime contracts.

## 0.14.0 Qt migration P0

Current verification is macOS arm64 native execution plus separately compiled
WASM32 contract/codec and tracked-runtime execution under Node. Every native package was staged under
the owning repository's build/; no global SDK installation or running product
application was replaced. Core/Native remain independent and Qt-disabled.

| Suite | Passed / total |
|---|---:|
| Core Release, including mandatory architecture/boundary checks | 38 / 38 |
| Core ASan + UBSan | 38 / 38 |
| Tracked Operation TSan | 1 / 1 |
| Tracked Operation WASM pthread execution under Node | 1 / 1 |
| Independent installed WASM tracked-runtime consumer | 1 / 1 |
| Core installed Runtime/Values consumer | 15 / 15 |
| Independent Contracts + text, native / WASM | 2 / 2 each |
| iiJson native / ASan+UBSan / WASM | 2 / 2 each |
| iiJson independent installed native / WASM consumer | 1 / 1 each |
| Native storage Release / ASan+UBSan | 3 / 3 each |
| iiCSMIDI source / installed base / installed Async | 4 / 4, 2 / 2, 1 / 1 |
| CoreIO storage adapter / Builder regressions | 2 / 2, 12 / 12 |

Text tests contain 32 checks; tracked work contains 339 checks including 100
completion/cancellation races; JSON contains 97 grammar/range/bounds/DTO checks;
native MIDI contains 75 SMF/storage/legacy-compatibility checks. Suite counts are
CTest entries, not assertions. Memory/thread instrumentation is host evidence.

Qt 6.8.3-authored SMF/metadata fixtures are byte-identical after native round-trip;
the captured compact journal hash is retained. NamespaceJournal itself is not
migrated. Runtime loader evidence confirms installed Native 0.8.0, MIDI 0.3.0 and
(only for Async) Core 0.14.0, with no Qt library loaded. Core's own dylib links only
libc++ and libSystem. The SDK dependency audit covers 34 graph nodes with no cycle
or Core/Native upward edges. SDK/Product file-creation audit reports zero
violations and unreadable files.

The aggregate machine-readable evidence is build/p0-verification.json with each
JUnit path, runtime loader output, file audit and explicit platform boundaries.
Supporting logs use build/p0-* and each consumer's build/. Windows, Linux,
Android/iOS device execution, full WASM Runtime/browser execution and all-SDK Qt
removal are not established by these results. Process/pipes/environment, network,
SQL, XML, image/document and BLE-peripheral work remains outside this P0 scope;
see qt-migration-p0.md. Historical results below retain their original scope.

## 0.13.0 foundation and feature boundary - 2026-10-16

The native macOS arm64 Release build imports no feature SDK. Its actual dylib
loads only libc++ and libSystem; iiFileProvider/Qt/preview frameworks are absent.
The standard-only Contracts component is separate from Values and Runtime.

| Validation | Result | Evidence |
| --- | --- | --- |
| Native Core regression, architecture and headers | 36/36 | build/foundation/foundation-tests.xml |
| AddressSanitizer + UndefinedBehaviorSanitizer | 36/36 | build/foundation-asan/foundation-asan-tests.xml |
| Installed Runtime + Values consumer, Qt/storage discovery disabled | 15/15 | build/foundation/consumer/build/consumer-tests.xml |
| Installed Contracts consumer, Threads/Qt/storage discovery disabled | 1/1 | build/foundation/contracts-consumer/build/contracts-consumer-tests.xml |
| Installed Values-only consumer | 3/3 | build/foundation/values-consumer/build/values-consumer-tests.xml |
| Native-header package consumed by WASM32, Node execution | 1/1 | build/foundation/contracts-wasm/build/contracts-wasm-tests.xml |
| Relocated file creation/atomic/preview assertions and loop example | 2/2 | ../iiFileProvider/build/coreio/coreio-tests.xml |
| Installed CoreIO consumer | 1/1 | ../iiFileProvider/build/coreio/consumer/build/coreio-consumer-tests.xml |
| Native file provider built with Core/Qt disabled | 3/3 | ../iiFileProvider/build/core-independent/core-independent-tests.xml |
| SDKBuilder unit, real Core/app build and relocated package | 12/12 | ../iiSDKBuilder/build/core-foundation/core-foundation-tests.xml |

The independent data/execution test performs 32 checks, including Unicode-scalar
UTF-8, null versus missing, owned nested values, signed/unsigned/real distinctions,
nonfinite/invalid text rejection, and token observation/cancellation without a
runtime link. Compile-time tests reject pointer truthiness, narrowing long-double
construction and borrowed views from temporaries. Native stream ownership adds
10 checks for aliases, exactly-once close notifications, rejected-resource cleanup
and allocation-failure cleanup. The allocation-failure probe compiles Channel.cpp
into the executable so sanitizer DSO allocation interposition cannot bypass the
injected failure; the installed consumer also exercises the shared public API.

Storage assertions moved with the storage implementation rather than being
replaced by fake saves inside Core. Core's I/O tests provision isolated descriptors
through test-only fixtures. The optional adapter retains exclusive creation,
truncation, atomic cancel/commit, typed serialized-record restoration, last-owner
presentation and the EventLoop copy example. It performs 29 integration checks.

build/foundation-upgrade.log confirms removal of three legacy Core headers while
preserving an unrelated consumer-owned file. build/foundation-sdk-dependencies.log
checks 33 declared SDK/component nodes, including the adapter above Core and
Native, with no cycle. build/foundation-file-ownership.json records 2,472 runtime
sources, zero creation bypasses and zero unreadable sources in SDK/Product.
The mandatory foundation guard also checks an explicit dependency allowlist in
all runtime/value/contract target manifests, including tests-disabled builds.

TDD failures are retained in build/core-foundation-tdd-red.log (the former upward
file-provider include) and build/foundation-task-token-tdd-red.log (header-only
token methods formerly required a runtime link). Configure/build/test logs use
the build/foundation-* names. Generated files and staged packages stay in build/.

This is native Core and adapter/package proof on macOS plus a Contracts-only
WASM32 execution check. It is not a new full WASM runtime/browser validation,
Windows/Linux/mobile build or device proof, nor a claim that the public APIs or
binaries of all Qt-based SDKs and applications have migrated. Existing app/game
runtime semantics remain covered by the native regression suites. The component,
source migration and responsibility contract is in foundation-architecture.md.


검증일: 2026-09-25, Asia/Seoul. 아래 결과는 이 변경을 작업한 macOS ARM64 환경의 실제 실행 기록이다. CI 구성 추가와 CI 실행 성공은 별개이며, 원격 CI는 아직 실행하지 않았다.

## 결과

| 대상 | 확인 결과 | 제한 |
|---|---|---|
| macOS ARM64 / Release / 공유 라이브러리 | 멤버 트리 포함 CTest 7/7, 라이브 120개 생산·소비 성공 | GUI/윈도우 호스트는 범위 밖 |
| macOS / ASan + UBSan / Debug 정적 라이브러리 | 경고를 오류로 처리한 빌드 및 CTest 7/7 성공 | 테스트가 실행한 경로의 검사 |
| macOS / TSan / Debug 정적 라이브러리 | CTest 6/6 성공, 경합 보고 없음 | 모든 가능한 실행 순서의 증명은 아님 |
| macOS 설치 소비자 | stage 설치본의 find_package, 공개 헤더, 링크, 동적 로딩, 실행 1/1 성공 | 전역 ~/.local 설치본은 교체하지 않음 |
|iOS ARM64 장치 SDK| 정적 라이브러리 교차 빌드 성공 | 물리 iPhone 및 서명된 앱 실행은 미검증 |
| iOS 27.0 ARM64 시뮬레이터 | 런타임·쿼리·멤버 트리 계약, 재사용, 라이브·객체 통신 예제 실행 성공 | UIKit 앱 생명주기 통합 검증과 구분 |
| Android NDK 30 / ARM64 / API 26 | 공유 라이브러리, 계약·재사용·소비자 테스트, 라이브 실행 파일 링크 성공 | 연결된 장치 없음, Android 실행 미검증 |
|WASM / Emscripten / 노드| 공통·쿼리·멤버 트리·소비자·객체 통신 CTest 5/5 성공 | 스레드 테스트는 비활성화 |
| WASM / 실제 Codex 브라우저 | 라이브 120개 생산·소비 완료, BrowserLoop 계약 성공 | 백그라운드 실행과 browser pthread는 미검증 |
| Windows / Linux | 동일한 표준 C++ 코어와 CMake 경로, 네이티브 CI 구성 추가 | 이번 로컬 환경에서 빌드·실행하지 않음 |

`otool -L build/libiiSDKCore.0.2.0.dylib`의 외부 의존성은 libc++와 libSystem이다. Qt·JVM·.NET 의존성이 없다. Android 결과물은 AArch64 ELF 공유 객체, 브라우저 결과물은 WebAssembly 바이너리임을 `file`로 확인했다.

## 실제 출력

macOS와 iOS 시뮬레이터의 재사용 테스트:

```text
reuse: 10000 iterations, operator new calls=0
```

macOS, iOS 시뮬레이터, 실제 브라우저의 라이브 예제:

```text
live runtime complete: produced=120 consumed=120 capacity=32 high_watermark=2
```

브라우저 계약 페이지 `iiSDKCoreBrowserTests.html`:

```text
browser contracts passed
```

## 검증한 의미론

- 큐 포화 거절, 같은 우선순위 FIFO, 빈 콜백/0 주기 거절, 1만 회 슬롯 재사용.
- 다른 루프 및 이전 세대 토큰 거절, 예약·실행 중 취소, 캡처 파괴 시 큐 재진입.
- 미래 타이머 조기 실행 방지, 최대 콜백 수/0 시간 예산, 주기 지연 건너뛰기.
- background 기아 방지, 재귀/동시 pump 거절, 예외 전파 후 복구.
- 4개 생산자 스레드의 400개 작업, 중단/재개, idle 종료, active 취소.
- 스트림 FIFO, 용량 제한, 최신 데이터 유지 정책, 종료 후 drain.
- 4개 생산자·2개 소비자가 용량 8의 스트림에서 400개 값을 각각 정확히 한 번 소비.
- 브라우저 중복 시작, 콜백 내 정지/재시작, 드라이버 파괴 시 프레임 취소, 예외 저장·조회.
- 객체 쿼리의 동기/큐 전달, 값/void/move-only 응답, 요청 값 보존, 수신 포트 만료, 처리 예외 보존, 취소, 큐 포화/종료, completion 예외, 실행 스레드 일치.

쿼리 모델 추가 후 macOS와 iOS 시뮬레이터에서 `object query complete: level=42`를 확인했다. Android ARM64에서도 쿼리 계약 및 객체 통신 예제의 교차 컴파일·링크를 완료했다. 설치 소비자는 설치된 Query 헤더를 포함하고 값 응답을 검사한다.

### 필요한 값만 선택하는 쿼리 계약 보강

객체 전체를 전달하지 않는 기본 계약을 문서에 명시하고 예제를 `ReadDevice{.level = true}`로 변경했다. 추가 검증은 macOS Release 6/6, ASan·UBSan 6/6, WASM/Node 4/4를 통과했다. 이번 보강에서는 iOS·Android·TSan을 다시 실행하지 않았으며 위 해당 플랫폼 기록은 이전 쿼리 모델 단계의 결과이다.

새 계약 테스트는 빈 선택이 전체 객체로 확대되지 않음, 선택한 0 값과 미선택 필드의 구분, 미선택 필드의 읽기·복사 생략, 큐에 저장된 선택 조건의 보존, 복사 불가능한 객체의 명시적 참조 선택, 부분 갱신에서 생략과 0/false의 구분을 검사한다.

```text
object query complete: level=42 name_selected=0 object_selected=0
```

### public/private 멤버 트리 구성

`examples/objects/Device.h`에서 public `audio.output`과 private `internal_.identity`, `audio.output.state_.history`를 실제 인스턴스로 구성했다. 각 계층의 값·일반 메서드·쿼리 포트 접근과 private 경로 차단을 `tests/member_tree.cpp`에서 컴파일 시 검사한다. 실행 시에는 상태 분리, 중첩 선택/응답, 큐 전달, 실패 시 상태 보존, 부모 소멸 후 자식 포트 만료를 검사한다.

이 단계는 macOS Release 7/7, ASan·UBSan 7/7, WASM/Node 5/5를 통과했다. Android ARM64 테스트·예제 빌드도 성공했다. iOS 시뮬레이터에서는 아래 두 실행 결과를 확인했다. Windows/Linux 원격 CI와 Android 장치 실행은 여전히 미검증이며 TSan 기록은 이전 쿼리 모델 단계의 결과이다.

```text
member tree contracts passed on iOS
object query complete: level=42 name_selected=0 object_selected=0 path=audio.output.level
```

## Queryable/designable 동적 인스턴스 속성

`ObjectProperties`를 Core의 공개 헤더·라이브러리·설치 패키지에 추가했다. `tests/properties.cpp`를 먼저 추가해 헤더 미구현으로 빌드 실패함을 확인한 후 구현했다. 생성 시 정의, 실행 중 값·타입·메서드·하위 트리의 추가/교체/삭제, 인스턴스 분리, public/private·queryable/designable/writable 정책, private/잠긴 자손의 상위 경로를 통한 우회 차단을 검사한다.

검증 실패 시 기존 값/revision 유지, 미선택 값의 복사 생략, 메타데이터와 값 조회 분리, 삭제 후 스냅샷 수명, 명시적 전체 객체 참조, mutable 메서드 상태 유지, 메서드 자기 삭제, 검증기 재진입 거절, 커밋 후 알림 조회, 알림 예외의 편집 결과 분리, 큐 전달 시점의 스키마 검증, 객체 소멸 후 수신자 만료까지 실행 검사했다.

| 검증 환경 | 이 변경의 결과 |
|---|---|
| macOS ARM64 Release 공유 라이브러리 | CTest 9/9 통과 |
| macOS ASan·UBSan, 경고를 오류로 처리 | CTest 9/9 통과 |
|WASM / Emscripten / 노드| CTest 7/7 통과 |
| iOS ARM64 시뮬레이터 | 전체 타깃 빌드, 동적 속성 계약 테스트와 예제 실행 성공 |
| Android ARM64 API 26 | 전체 타깃 교차 컴파일·링크 성공; 장치 실행 미검증 |
| 별도 설치 소비자 | build/stage 설치 후 find_package, 새 공개 헤더·심볼 링크 및 CTest 1/1 통과 |
| Windows / Linux | 기존 CI 구성이 새 테스트를 포함한다. 이 변경의 원격 빌드·실행 미검증 |

macOS와 iOS 시뮬레이터의 예제 실행 결과:

```text
dynamic instance complete: gain=0.75 scaled=1.5 other_instance_unchanged=1
```

iOS 시뮬레이터의 계약 테스트는 `dynamic property contracts passed on iOS`를 출력하고 종료 코드 0을 반환했다. 검사 후 시뮬레이터를 종료했다. macOS 공유 라이브러리의 동적 의존성도 다시 확인했으며 libc++와 libSystem만 있고 Qt는 없다. 이번 동적 속성 변경에 대해 TSan, iOS 실기기, 브라우저 UI 실행을 새로 수행하지 않았다. 이전 단계의 해당 기록과 구분한다.

## 플랫폼 독립 Thread 추가 검증

Thread 계약 테스트를 먼저 추가하고 공개 헤더 부재로 빌드가 실패함을 확인한 뒤 구현했다. idle 상태, 실제 별도 스레드 실행, join의 결과 가시성, 실행 재사용, 중복 시작 거절, 중단 요청과 대기 깨우기, 예외 보존, 자기 join 거절, RAII 종료, idle/suspended EventLoop 중단, 루프 예외 전달, 다른 join과 동시 중단·상태 조회를 검사한다. 기존 다중 생산자·소비자 및 쿼리 스레드 테스트도 Thread를 사용한다.

| 환경 | 이 변경의 검증 결과 |
|---|---|
|macOS ARM64 릴리스| CTest 11/11 통과 |
| macOS ASan·UBSan 및 경고 오류 처리 | CTest 11/11 통과 |
| macOS ThreadSanitizer | CTest 11/11 통과 |
| WASM 단일 스레드 / Node | CTest 9/9 통과; Thread unsupported와 인라인 대체 실행 없음 확인 |
|WASM pthread / Node| CTest 9/9 통과; 실제 워커에서 스레드·다중 생산자/소비자·쿼리 실행 |
| 실제 브라우저 / WASM pthread | 스레드 계약 전체 실행과 exit=0, 메인 스레드 차단 거절 및 BrowserLoop 계약 통과 |
| iOS ARM64 시뮬레이터 | Thread·런타임·Query 테스트와 ThreadExample 실행 성공 |
| Android ARM64 API 26 | 전체 타깃 컴파일·링크 성공; 장치 실행 미검증 |
| 별도 설치 소비자 | build/stage의 새 Thread 헤더·심볼로 스레드 실행·join 및 CTest 1/1 통과 |
| Windows / Linux | 기존 CI가 새 테스트를 포함하며 이번 변경의 원격 실행은 미검증 |

브라우저에서 tests/serve_wasm.py의 COOP/COEP 헤더를 사용하는 페이지를 열어 실제 표시 결과를 확인했다.

```text
crossOriginIsolated=true
thread contracts passed on WASM
exit=0

browser host blocking correctly rejected
browser contracts passed
```

iOS 시뮬레이터 실행 결과:

```text
thread contracts passed on iOS
runtime contracts passed on iOS
query contracts passed on iOS
managed thread complete: answer=42 background=1
```

브라우저 테스트 탭·로컬 서버·시뮬레이터는 검사 후 정리했다. macOS 라이브러리의 동적 의존성은 libc++/libSystem이며 Qt가 없다. GitHub Actions의 WASM job도 pthread OFF/ON으로 분리했다. Node 성공과 실제 브라우저 성공을 별도로 기록하며, iOS 시뮬레이터 결과를 실기기 검증으로 간주하지 않는다.

## 멀티스레드·병렬·비동기 기본 런타임 검증

Runtime/SerialQueue 계약 테스트를 먼저 추가하고 공개 헤더 부재로 빌드 실패를 확인한 뒤 구현했다. 기본 워커가 2개 이상인지, 독립 작업 및 서로 다른 객체 큐가 실제로 겹쳐 실행되는지, 같은 객체 큐는 우선순위가 달라도 FIFO와 최대 동시 실행 1을 유지하는지 검사한다. 4개 생산자의 동시 제출, 바인딩된 쿼리·동적 속성의 외부 동기 접근 거절, 완료 처리기의 실행 영역도 검사한다.

고정 용량 포화, 실행자·슬롯 세대별 취소 토큰, 객체 큐의 선두·중간 취소, 대기 작업 일괄 취소, 수신자 만료, 예외 이후 워커 유지, 자기 풀 대기 거절, RAII drain/join 및 Runtime 소멸 후 큐 제출 거절을 실행 검사했다. 기존 테스트도 함께 수행했다.

| 환경 | 이 변경의 검증 결과 |
|---|---|
|macOS ARM64 릴리스| CTest 13/13 통과 |
| macOS ASan·UBSan 및 경고 오류 처리 | CTest 13/13 통과 |
| macOS ThreadSanitizer | CTest 13/13 통과 |
|WASM pthread / Node| CTest 11/11 통과; 기존 캐시 옵션을 제거하고 새 기본값 ON도 확인 |
| WASM 명시적 단일 스레드 / Node | CTest 11/11 통과; 런타임 생성 거절과 인라인 대체 실행 없음 확인 |
| 실제 브라우저 / WASM pthread | 병렬 런타임 계약 전체와 exit=0, 메인 스레드 차단 거절 및 프레임 루프 계약 통과 |
| iOS ARM64 시뮬레이터 | 전체 타깃 빌드, 병렬 계약·메모리 재사용 테스트와 비동기 쿼리 예제 실행 성공 |
| Android ARM64 API 26 | 전체 타깃 컴파일·링크 성공; 장치 실행 미검증 |
| 별도 설치 소비자 | build/stage의 Runtime/SerialQueue 및 바인딩된 쿼리로 비동기 실행, CTest 1/1 통과 |
| Windows / Linux | 기존 CI가 새 테스트를 포함하며 이 변경의 원격 실행은 미검증 |

macOS와 iOS 시뮬레이터에서 확인한 출력:

```text
parallel runtime complete: workers=4 result=42 asynchronous=1
reuse: 10000 iterations, operator new calls=0
parallel reuse: 10000 iterations, operator new calls=0
```

재사용 측정은 준비된 큐에 작은 작업을 제출하고 완료하는 경로에 한정한다. 임의 payload·콜백·동적 스키마와 OS 할당 전체가 무할당이라는 의미는 아니다. iOS 계약 테스트는 `parallel runtime contracts passed on iOS`와 종료 코드 0을 반환했다.

실제 브라우저 표시 결과:

```text
crossOriginIsolated=true
parallel runtime contracts passed on WASM
exit=0

browser host blocking correctly rejected
browser contracts passed
```

macOS 공유 라이브러리 의존성은 libc++/libSystem이며 Qt가 없다. iOS 시뮬레이터, 브라우저 테스트 탭과 로컬 서버는 검증 후 정리했다. 네이티브 실기기와 원격 CI 결과를 시뮬레이터·교차 컴파일 결과로 대신하지 않는다.

## 동적 타입·명시적 캐스팅 검증

tests/value.cpp와 CMake 타깃을 먼저 추가하고 Value.h 부재로 빌드 실패를 확인한 뒤 구현했다. 정확한 타입과 사용자 DTO, 동적 재대입, const 참조 조회, 빈 값/null 구분, 정수 부호·범위, float→정수 경계, 64비트 정수 정밀도, 부동소수점 축소와 언더플로, 문자열 전체 파싱·형식 오류·왕복을 검사한다.

속성의 fixed/dynamic 정책, 명시적 쓰기 변환, 응답만 변환하는 선택 조회, 실패 시 값·revision 보존, 검증기와 private 경로, 미선택 값의 변환 생략 및 오류 경로도 검사했다. 객체 큐에서 100개의 동적 타입 변경과 DynamicValue 입력·응답 쿼리를 실행한다. 마지막 동적 응답 콜백은 const 참조 형태로 검증했다.

| 환경 | 결과 |
|---|---|
|macOS ARM64 릴리스| 전체 CTest 14/14 통과; 마지막 쿼리 콜백 수정 후 Value 테스트 재통과 |
| macOS ASan·UBSan 및 경고 오류 처리 | 전체 14/14 통과; 마지막 Value 테스트 재통과 |
| macOS ThreadSanitizer | 전체 14/14 통과; 마지막 Value 테스트 재통과 |
|WASM pthread / Node| 전체 12/12 통과; 마지막 동적 응답 쿼리 테스트 재통과 |
| WASM 명시적 단일 스레드 / Node | 전체 12/12 통과 |
| 실제 브라우저 / WASM pthread | value-browser.html의 동적 값·캐스팅·비동기 쿼리 계약 통과, exit=0 |
| iOS ARM64 시뮬레이터 | 전체 타깃 빌드, 동적 값·캐스팅 및 속성 계약 실행 성공 |
| Android ARM64 API 26 | 전체 타깃 컴파일·링크 성공; 장치 실행 미검증 |
| 별도 설치 소비자 | 설치된 Value.h와 cast_value 심볼로 문자열→정수 변환 실행, CTest 1/1 통과 |
| Windows / Linux | CI에 새 타깃이 포함되며 이번 변경의 원격 실행은 미검증 |

브라우저 실제 표시 결과:

```text
crossOriginIsolated=true
dynamic value and casting contracts passed
exit=0
```

iOS 시뮬레이터는 `dynamic value and casting contracts passed`와 `dynamic property contracts passed on iOS`를 출력하고 종료 코드 0을 반환했다. iOS 실행은 마지막 동적 쿼리 콜백을 const 참조로 변경하기 전이며, 캐스팅·속성 구현은 동일하다.

교차 검증에서 부동소수점 charconv의 WASM 지원 부재 및 iOS 배포 대상 가용성 제한을 발견하여 classic locale을 사용하는 스트림 변환으로 교체했다. WASM long double 문자열 출력에서 double 정밀도로 축소되는 결과도 확인했으므로 이 변환 쌍은 전 플랫폼에서 명시적으로 unsupported이다. long double 저장과 숫자 사이 변환은 지원한다. 일부 Emscripten expected<any> 값 전달의 컴파일 제약과 참조 콜백 사용법은 docs/dynamic-value.md에 기록했다.

검증용 서버·탭·시뮬레이터를 정리했다. 공유 라이브러리는 libc++/libSystem에만 의존하며 Qt 의존성은 없다. 기존 런타임 메모리 재사용 회귀 테스트도 통과했다.

## 네이티브 I/O 추상화 검증

tests/io.cpp와 CMake 타깃을 먼저 추가하고 IO/AsyncIO.h 부재로 실패함을 확인한 뒤 구현했다. 파일 생성·배타 생성, 바이너리 왕복, 부분 읽기, EOF, 빈 전송, 접근 모드, 잘못된 옵션의 비파괴 처리, seek/size/flush, 공유 채널 닫기와 닫힌 자원 재접근을 검사한다. 테스트 파일은 네이티브에서 build 아래에 생성·정리한다.

비동기 쿼리는 별도 워커 실행, FIFO seek/write/flush, 실제 저장된 바이트, 취소, 버퍼 범위 검사, 임시 Buffer의 요청 수명 유지, close의 별칭 반영을 검사한다. macOS에서는 테스트가 소유한 PTY master/slave를 생성하여 raw 시리얼 양방향 전송, 준비 대기, would_block/timeout, 실행 중 취소와 busy/close 보호를 실행 검증했다. 물리 USB/시리얼 장치를 검사한 것은 아니다.

| 환경 | 이 변경의 검증 결과 |
|---|---|
|macOS ARM64 릴리스| 전체 CTest 15/15 통과; 최종 수정 후 IO/Consumer/Reuse 3/3 재통과 |
| macOS ASan·UBSan 및 경고 오류 처리 | 전체 15/15 통과; 최종 관련 3/3 재통과 |
| macOS ThreadSanitizer | 전체 15/15 통과; 최종 관련 3/3 재통과 |
|WASM pthread / Node| 전체 13/13 통과; 최종 IO/Consumer 2/2 재통과 |
| WASM 명시적 단일 스레드 / Node | 전체 13/13 통과 |
| 실제 브라우저 / WASM pthread | 가상 파일 I/O와 비동기 쿼리 계약 통과, exit=0 |
| iOS ARM64 시뮬레이터 | 전체 타깃 빌드 성공. 실행은 외장 볼륨 open의 호스트 TCC 권한 판정 대기로 중단; I/O 실행 성공 미검증 |
| Android ARM64 API 26 | 전체 타깃 컴파일·링크 성공; 장치 실행 미검증 |
| 별도 설치 소비자 | 설치된 IO/Channel.h와 라이브러리로 실제 파일 읽기, CTest 1/1 통과 |
| Windows / Linux | 네이티브 백엔드와 CI 타깃을 구성했으며 이번 변경의 원격 빌드·실행은 미검증 |

macOS 직접 실행 결과:

```text
native serial PTY roundtrip passed
I/O contracts passed on macOS
I/O buffer reuse: 10000 iterations, operator new calls=0
```

재사용 측정은 한 번 연 파일과 확보한 Buffer로 작은 read/seek를 반복하는 경로이다. 임의 비동기 콜백·사용자 데이터·OS 내부 할당까지 무할당이라는 의미는 아니다.

브라우저 실제 표시 결과:

```text
crossOriginIsolated=true
I/O contracts passed on WASM
exit=0
```

iOS 시뮬레이터 대기 원인은 build/ios-simulator/build/io-sample.txt에서 main → run → open_file → open → __open 호출 스택으로 확인했다. 같은 PID에 대한 호스트 tccd AUTHREQ_ATTRIBUTION 로그에서 SimulatorTrampoline 책임 프로세스와 I/O 테스트 프로세스의 접근 요청을 확인했다. 권한 대기 중인 테스트 프로세스를 종료하고 시뮬레이터를 정리했으며, 뒤따르는 iOS 재사용 테스트는 실행하지 않았다.

WASM 도구 체인에서 std::stop_token 지원 부재를 확인하여, 모든 플랫폼에 같은 최소 atomic 기반 CancellationToken/Source 계약을 적용했다. 브라우저 탭과 검증 서버도 정리했다. macOS 공유 라이브러리의 의존성은 libc++/libSystem이며 Qt는 없다.

## 회귀 검사 경로

- `build/Testing/Temporary/LastTest.log`: 네이티브 Release.
- `build/sanitize/build/Testing/Temporary/LastTest.log`: ASan + UBSan.
- `build/thread-sanitize/build/Testing/Temporary/LastTest.log`: TSan.
- `build/wasm/build/Testing/Temporary/LastTest.log`: 노드/WASM.
- `build/wasm-threads/build/Testing/Temporary/LastTest.log`: 노드/WASM pthread.
- `build/installed-consumer/build/Testing/Temporary/LastTest.log`: 설치 소비자.
- `build/ios-simulator/build/*.app/`: 시뮬레이터에서 실행한 바이너리.
- `build/android-arm64/build/`: Android 교차 빌드 결과물.

생성물과 로그는 git에 포함하지 않는다. `.github/workflows/runtime.yml`은 macOS/Windows/Linux 공유·정적 빌드와 소비자 실행, iOS 라이브러리, Android 4개 ABI 교차 빌드, WASM 공통 테스트를 재현한다. 원격 CI 성공이나 모든 Linux 배포판/CPU 지원을 미리 선언하지 않는다.

ASan/UBSan 재현:

```sh
cmake -S . -B build/sanitize/build -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build/sanitize/build --parallel 4
ctest --test-dir build/sanitize/build --output-on-failure
```

TSan은 별도 `build/thread-sanitize/build`에서 `-fsanitize=thread`로 빌드한다. iOS simulator는 `CMAKE_SYSTEM_NAME=iOS`, `CMAKE_OSX_SYSROOT=iphonesimulator`, `CMAKE_OSX_ARCHITECTURES=arm64`로 빌드하고, 부팅된 시뮬레이터에 `xcrun simctl spawn <device-id> <executable-path>`로 각 테스트를 실행한다.

## 빈 폴더 기반 객체 구현 검증

`cmake/Foundations.cmake`에 등록한 공개 헤더 18개를 각기 독립 번역 단위로 컴파일했다. `src/` 내 `.DS_Store`를 제외하고 실제 파일이 없는 하위 디렉터리는 0개이며, 18개 공개 헤더 모두 `tests/foundations.cpp`의 소비 대상이다. 기반 객체별 책임과 제한은 [foundations.md](foundations.md)에 정의한다.

| 검증 구성 | 확인 결과 |
|---|---|
|macOS 릴리스| 전체 17/17 통과 |
| macOS ASan + UBSan, 경고를 오류로 처리 | 전체 17/17 통과, 최종 PTY 테스트 수정 후 I/O 재통과 |
|macOS TSan| 전체 17/17 통과, I/O 테스트 20회 연속 재통과 |
|WASM pthread / Node| 전체 15/15 통과 |
| WASM 단일 스레드 / Node | 전체 15/15 통과. 병렬 경로는 지원 거절/생략을 검증하며 병렬 실행 증거가 아님 |
| 실제 브라우저 / WASM pthread | `foundations-browser.html`에서 기반 객체 계약 통과, `exit=0` |
| Android ARM64 API 26 | 라이브러리·공개 헤더·테스트·예제 컴파일 및 링크 성공 |
| iOS ARM64 시뮬레이터 | 라이브러리·공개 헤더·테스트·예제 컴파일 및 링크 성공 |
| 별도 설치 소비자 | 기존 소비자 및 18개 기반 객체 계약 2/2 통과 |

이번 기반 객체 변경에서 Android/iOS 장치 실행, Windows/Linux 원격 빌드·실행, 실제 SDL 호스트 및 Vulkan 장치 연결은 검증하지 않았다. 위 모바일 결과는 교차 빌드 결과이다. 물리 시리얼 장치 대신 macOS PTY로 네이티브 시리얼 바이트 경로를 검사한다.

브라우저에서 실제 관찰한 출력:

```text
crossOriginIsolated=true
foundation contracts passed on WASM
exit=0
```

네이티브 기반 객체 조합 예제의 직접 실행 출력:

```text
foundation pipeline complete: sensor.reading=42
```

경계 조건으로 Runtime 취소/종료 시 TaskGroup 작업 수 정리, 수락 거절, 작업 예외 전달, 그래프 순환/타 소유자/오래된 세대 차단, Scene 실행 영역과 실패한 부모 변경의 원자성, 함수 교체·자기 삭제, 호스트 초기화 실패, 타임라인 완료 역행·초과·재진입을 검사했다. GPU 완료는 주입 카운터 계약만 검사했으며 드라이버 실행으로 간주하지 않는다.

TSan 첫 실행에서 기존 PTY 테스트가 실패했다. `size()` 관찰자가 잠금을 잡은 순간 워커 `wait()`가 busy로 종료되어, 대기 진입을 확인하지 못하는 테스트 경쟁 조건이었다. 워커의 진입 busy를 재시도하고 취소·join 이후 판정하도록 수정했다. 이후 전체 TSan 통과 및 I/O 20회 연속 통과를 확인했으며, 제품 I/O의 잠금·거절 계약은 바꾸지 않았다.

설치는 `build/stage`에 수행했다. `~/.local` 설치본은 이번 변경에서 갱신하지 않았다. 공유 라이브러리의 의존성은 libc++와 libSystem이며 Qt/SDL/Vulkan 의존성은 없다. 브라우저 검증 탭과 로컬 검증 서버는 종료했다.

## 0.3.0 객체 제거·자원 반환 검증

검증일: 2026-10-06, Asia/Seoul. 공개 헤더와 빌드 패키지 버전은 0.3.0이다. 제거 계약은 [cleanup.md](cleanup.md)에 정의하며, TaskScope와 Cleanup 테스트를 먼저 추가하여 헤더 부재의 빌드 실패를 확인한 뒤 구현하였다.

대기 쿼리를 가진 포트 제거 후 처리기·입력·completion의 마지막 소유 참조 소멸, pending=0, 이전 토큰 무효화와 새 객체의 전체 capacity 재사용을 확인한다. 이 과정을 capacity=1로 1,000회 반복한다. 실행 중 처리기와 completion의 각각 다른 대기 지점에서 close가 반환하지 않는지, 동기 run과 동시 접수·close, 실패한 접수와 cleanup 예외의 캡처 해제도 검사한다.

Scene의 하위 트리 제거·속성 소멸·자체 제거 거절·이전 ID 차단·용량 반환 순서, TaskGroup·AsyncIO와 SerialQueue의 대기 작업 정리, 다른 큐의 작업 보존을 확인한다. 캡처 소멸자를 정지시킨 동안 기존 작업 슬롯이 계속 점유되어 full이고, 소멸 완료 후 다음 작업이 성공하는지 검사한다. EventLoop 중단의 타이머 해제, Thread 중단 정리 중 재진입, 취소 캡처의 Runtime 자기 대기 거절, 실행자 소멸 후 토큰 접근도 검증한다.

새 Scene 경계 테스트가 메서드의 재진입 허용 구간에서 자기 노드 제거를 차단하지 못하는 경로를 발견하였다. 속성 편집 잠금과 별도로 전체 호출 수명을 추적하여 busy로 거절하고 계층을 보존하도록 수정하였다. 기존 수신자 만료 테스트는 제거 후 지연 completion을 실행하던 0.2.0 계약에서, 대기 콜백·자원·할당량을 즉시 취소하는 0.3.0 계약으로 갱신하였다.

| 최종 변경본 검증 구성 | 결과 |
|---|---|
| macOS ARM64 Release, 공유 라이브러리 | 전체 CTest 18/18 통과 |
| macOS ASan + UBSan, 정적 라이브러리, 경고 오류 처리 | 전체 CTest 18/18 통과 |
| macOS ThreadSanitizer, 정적 라이브러리, 경고 오류 처리 | 전체 CTest 18/18 통과; Cleanup 20회 연속 통과 |
| WASM pthread / Node | 전체 CTest 16/16 통과; 병렬 제거 경로 실행 |
| WASM 명시적 단일 스레드 / Node | 전체 CTest 16/16 통과; 스레드 의존 제거 검사는 생략하며 병렬 실행 증거가 아님 |
| build/stage 설치본의 별도 소비자 | 0.3.0 패키지로 빌드, CTest 3/3 통과; Cleanup 포함 |
| 문서의 WorkerObject 조합 예제 | 설치 헤더·라이브러리로 경고 오류 빌드 및 실행 성공; close 후 제출 거절·pending=0 확인 |
| iOS ARM64 시뮬레이터 대상 | 라이브러리·Cleanup 테스트 실행 파일·공개 헤더 컴파일 및 링크 성공; 실행 미검증 |
| Android ARM64 API 26 대상 | 라이브러리·Cleanup 테스트 실행 파일·공개 헤더 컴파일 및 링크 성공; 실행 미검증 |

기반 객체 공개 헤더 18개에 TaskScope.h를 추가한 총 19개 헤더를 독립 번역 단위로 컴파일하였다. 기존 EventLoop/Stream/Runtime/I/O의 준비된 저장소 1만 회 재사용 검사도 통과하였다. TaskScope의 작업 추적과 사용자 payload 자체는 동적 할당을 허용하므로 전체 쿼리 무할당 보장은 아니다.

설치는 Workspace의 `build/stage`에 수행하고, 소비자의 iiSDKCore_DIR이 그 설치 패키지를 가리키는지 확인하였다. `~/.local` 설치본은 이 작업에서 갱신하지 않았다. 공유 라이브러리의 동적 의존성은 libc++와 libSystem이며 Qt 의존성이 없다. 제거 완료는 등록 작업·소유 자원·예약 할당량의 정리를 뜻하며, 프로세스 RSS 즉시 감소·해제 바이트의 보안 덮어쓰기·외부 공유 자원 또는 영속 파일 삭제를 뜻하지 않는다.

이 변경의 실제 브라우저, iOS/Android 실행, 물리 모바일 장치 및 Windows/Linux 실행은 검증하지 않았다. 이전 절의 브라우저·모바일 결과를 0.3.0 변경본의 실행 증거로 사용하지 않는다.

## 0.4.0 다중 상속 금지 검증

검증일: 2026-10-06, Asia/Seoul. 패키지 버전은 0.4.0이며 정책은 [inheritance.md](inheritance.md)에 정의한다. 정책 테스트를 먼저 추가하여 검사기 부재로 실패하는 상태를 확인하였다. 빌드 검사를 연결한 뒤에는 final이 없는 공개 정의 50개로 빌드가 거절되는 상태를 확인하고 해당 정의를 봉인하였다.

공개 소스 헤더 30개의 완전한 class/struct 정의 82개가 모두 final이다. 공개 헤더 안에 있는 내부 보조 정의도 포함한다. 기존 final 정의를 보존하고 20개 헤더의 정의 50개에 final을 추가하였다. 설치 헤더 30개가 대응하는 소스 헤더와 바이트 단위로 동일함을 확인하였다. 기존 지정 초기화·값 전달·객체 제거 계약은 유지한다.

필수 iiSDKCoreArchitectureCheck는 라이브러리·테스트·예제·독립 공개 헤더 검사 타깃보다 먼저 실행한다. 네이티브 구성의 번역 단위 50개, WASM 구성의 번역 단위 51개를 각각 해당 컴파일러로 전처리하여 검사하였다. 원본 검사와 전처리 결과 검사는 모두 두 개 이상의 직접 기반 클래스와 직접 기반 팩을 거절하며, 공개 헤더의 final 누락은 매크로 확장 후에도 거절한다.

tests/architecture_policy.py의 회귀 검사 11개는 단일 기반 템플릿/decltype의 쉼표와 다중 기반을 구별하며, 접근 지정·가상 기반·기반 팩·매크로·주석·문자열·속성·전방 선언·익명 공개 구조체를 검사한다. 실제 C++ 컴파일러로 RuntimeOptions, PropertyDefinition, io::Read의 상속 거절과 같은 타입의 조합 성공도 확인한다.

BUILD_TESTING=OFF인 별도 CMake 검증 프로젝트를 먼저 정상 빌드한 뒤, private 타깃 정의와 GNU 확장 모드에 따라 매크로가 다중 기반을 만들도록 변경하였다. 필수 검사가 빌드를 거절하고 기존 객체 파일의 수정 시각을 유지하는지 확인하여 컴파일 전 차단을 검증하였다. 이후 조합으로 수정한 소스가 기존 캐시를 무효화하고 빌드되는지, 매크로가 만든 공개 타입에서 final을 생략하면 빌드가 거절되는지도 확인하였다. WASM에서는 이 중첩 네이티브 CMake 검증 하나를 생략하며 나머지 검사 10개를 실행한다.

| 변경본 검증 구성 | 결과 |
|---|---|
| macOS ARM64 Release, 공유 라이브러리 | 전체 CTest 19/19 통과 |
| macOS ASan + UBSan, 정적 라이브러리, 경고 오류 처리 | 전체 CTest 19/19 통과 |
| macOS ThreadSanitizer, 정적 라이브러리, 경고 오류 처리 | 전체 CTest 19/19 통과 |
| WASM pthread / Node | 전체 CTest 17/17 통과 |
| WASM 명시적 단일 스레드 / Node | 전체 CTest 17/17 통과; 스레드 의존 검사는 생략 |
| build/stage 설치본의 별도 소비자 | 0.4.0 패키지로 빌드, CTest 3/3 통과 |
| build/stage 설치 헤더의 상속·조합 검사 | SDK 타입의 단일·다중 상속은 final 진단과 함께 컴파일 실패, 조합 및 RuntimeOptions의 aggregate 특성 검사는 컴파일 성공 |
| iOS ARM64 시뮬레이터 대상 | 라이브러리·Cleanup 테스트 실행 파일·독립 공개 헤더 컴파일 및 링크 성공; 실행 미검증 |
| Android ARM64 API 26 대상 | 라이브러리·Cleanup 테스트 실행 파일·독립 공개 헤더 컴파일 및 링크 성공; 실행 미검증 |

Python 3.9 이상은 소스 빌드 호스트의 검사 도구이며 런타임 및 설치 소비자의 의존성이 아니다. 검사 성공 결과는 build/architecture에 저장하고 검사기·구성·타깃 옵션·입력 변경에 따라 다시 검사한다. WASM 단일 스레드 구성에서 캐시를 제거한 검사로 번역 단위 51개를 모두 전처리하고, 변경 없는 반복 빌드에서는 전처리 0개로 같은 필수 검사가 통과하는지 확인하였다. 설치 헤더 비교와 설치 컴파일 확인 결과는 build/architecture/installed-probe.json에 기록하였다. 빌드 생성물은 git에 포함하지 않는다.

이전 버전의 SDK 값 구조체를 상속하던 소비자는 멤버 조합으로 변경하고 헤더·라이브러리를 함께 갱신하여 다시 빌드해야 한다. SDK를 링크하는 외부 프로젝트의 임의 타입 전체를 검사하는 정책은 아니며, SDK가 정의한 공개 타입의 상속 차단은 설치 헤더만으로 강제된다. 표준 라이브러리 타입 별칭은 해당 표준 타입의 상속 규칙을 유지한다.

설치는 Workspace의 build/stage에 수행하였고 ~/.local 설치본은 갱신하지 않았다. 이번 변경의 실제 브라우저, iOS/Android 실행 및 Windows/Linux 빌드·실행은 검증하지 않았다. 모바일 교차 빌드와 Node 실행을 실제 장치·브라우저 실행 증거로 사용하지 않는다.

## 이벤트 루프 I/O 0.5.0 검증 — 2026-10-06

EventLoop에 watch/read/write를 추가하여 일반 작업·타이머와 같은 슬롯에서 채널 준비 감지와 바이트 전송을 처리한다. POSIX run은 다중 채널과 wake pipe를 함께 기다리고, poll은 0 timeout 준비 검사로 호스트 구동 계약을 유지한다. Read/Write·Buffer 등의 기존 공개 정의는 하위 IO/Requests.h로 이동하여 EventLoop와 AsyncIO 사이의 순환 의존을 피하였다. 느린 동기 호출의 워커 격리는 AsyncIO로 유지한다. 공개 계약은 [loop-io.md](loop-io.md)에 정의한다.

TDD의 최초 구성 후 컴파일은 EventLoop::watch/read/write와 open_device가 존재하지 않아 실패하였다. 구현 후 tests/loop_io.cpp에서 다음 경로를 실행하였다.

- 실제 파일의 이진 입력·출력, 부분 읽기·EOF·빈 전송, 닫힌 채널, 범위 오류, 음수 및 최솟값 timeout, 빈 콜백의 접수 거절.
- 입력 완료에서 typed QueryPort로 값을 전달하고 같은 루프에서 응답을 처리하는 경로.
- 한 번의 준비 완료, suspend/resume, 4:2:1 우선순위, 작업과 I/O의 공유 capacity, 외부·재사용 토큰 거절, 완료 예외의 슬롯 정리.
- FIFO의 무입력 기한·probe·협력 취소, 데이터 입력, 출력 포화와 would_block, 타이머가 출력을 재개시키는 경로.
- 끊어진 FIFO의 EPIPE 반환, 프로세스 SIGPIPE 종료 방지, 호스트의 기존 pending SIGPIPE와 스레드 신호 마스크 보존.
- 두 PTY의 입력 → 완료 콜백 → 다른 채널 출력, 독립 채널 기한과 CPU 작업·타이머의 진행, 출력 피어의 실제 바이트 대조.
- Thread가 네이티브 I/O 대기에 진입한 뒤 post로 깨우기, 감시 취소 후 즉시 Channel::close 성공, stop/join 및 pending 0 확인.
- 취소 캡처의 소멸자가 루프에 다시 작업을 제출하는 경로. 네이티브 대기 참조와 핸들 잠금, Channel·Buffer·콜백을 정리한 뒤 슬롯을 반환하며, std::function은 빈 함수와 교환하여 이동 후 상태에 의존하지 않는다.

examples/loop_io.cpp는 파일의 5바이트를 EventLoop에서 읽고 다른 파일 채널로 출력한 뒤 다시 열어 저장된 바이트를 대조하였다. 출력은 `loop I/O complete: input=5 output=5`이다. 준비한 Buffer와 슬롯을 재사용하는 tests/reuse.cpp의 새 I/O 경로는 10,000회 반복에서 `event loop I/O reuse: 10000 iterations, operator new calls=0`을 기록하였다. 이는 해당 재사용 경로의 측정이며 임의 사용자 캡처·버퍼 생성·장치 열기에 대한 무할당 보장은 아니다.

| 최종 변경본 검증 구성 | 결과 |
|---|---|
| macOS ARM64 Release, 공유 라이브러리 | 전체 CTest 21/21 통과 |
| macOS ASan + UBSan, 정적 라이브러리, -Wall -Wextra -Wpedantic -Werror | 전체 CTest 21/21 통과 |
| macOS ThreadSanitizer, 정적 라이브러리, -Wall -Wextra -Wpedantic -Werror | 전체 CTest 21/21 통과, 새 LoopIO 테스트 10회 반복 통과 |
| WASM pthread / Node | 전체 CTest 19/19 통과 |
| WASM 명시적 단일 스레드 / Node | 전체 CTest 19/19 통과; 스레드 의존 경로 생략 |
| build/stage 설치본의 별도 소비자 | 0.5.0 패키지로 빌드·동적 링크, CTest 4/4 통과; 설치본으로 LoopIO 계약 실행 |
| 설치 헤더·상속 정책 | 공개 헤더 31개가 소스와 일치, 공개 타입 84개 모두 final, 내부 Poller/Transfer 헤더는 설치 제외 |
| iOS ARM64 시뮬레이터 대상 | 라이브러리·LoopIO 테스트·LoopIO 예제·독립 헤더 20개 컴파일 및 링크 성공; 이번 변경본 실행 미검증 |
| Android ARM64 API 26 대상 | 라이브러리·LoopIO 테스트·LoopIO 예제·독립 헤더 20개 컴파일 및 링크 성공; 실행 미검증 |

필수 상속 검사는 네이티브·모바일 번역 단위 53개, WASM 번역 단위 54개에서 통과하였다. 설치 헤더 비교 결과는 build/architecture/loop-io-installed-probe.json에 기록하였다. otool로 확인한 0.5.0 공유 라이브러리의 외부 의존성은 libc++와 libSystem이며 Qt 의존성이 없다. 설치는 Workspace의 build/stage에 수행하였고 ~/.local 설치본은 갱신하지 않았다. 이전 제거·상속 계약의 회귀 검사도 위 전체 CTest에 포함된다.

실행 증거는 macOS의 파일·FIFO·가상 시리얼 PTY와 Node의 WASM 가상 파일이다. 물리 USB 시리얼·센서·HID·GPIO 및 실제 브라우저·iOS/Android 실행, Windows/Linux 빌드·실행은 이번 변경에서 검증하지 않았다. Windows 코드는 기존 COM 상태 재검사와 wake event 경로이며 IOCP/overlapped 구현은 아니다. 장치 열거·hotplug·USB Host/IOKit·Bluetooth·오디오·MIDI·소켓 연결·장치별 ioctl API는 추가하지 않았다. 일반 파일과 O_NONBLOCK를 무시하는 드라이버의 실제 전송은 루프를 지연시킬 수 있다. 예산은 이미 실행 중인 OS 호출·콜백을 선점하지 않으며, run을 사용하는 Thread는 루프 소멸 전에 stop/join해야 한다.

## 네이티브 하드웨어 I/O 0.6.0 검증 — 2026-10-06

`IO/Hardware.h`의 장치 열거·hotplug·제어·media I/O와 `IO/Protocols.h`의 표준 프로토콜 해석을 추가하였다. 공개 타입은 final이고 native implementation은 플랫폼별 translation unit으로 분리되어 있다. Qt 및 상위 iiSDK 모듈을 참조하지 않는다. 상세 지원 범위와 호스트 조건은 [hardware.md](hardware.md)에 기록하였다.

| 검증 대상 | 실제 결과 | 증거 범위 |
|---|---|---|
| macOS ARM64 Release 공유 라이브러리 | 전체 CTest 24/24 통과 | native hardware, protocol, queue/cleanup, architecture 및 기존 런타임 회귀 |
| macOS Debug ASan + UBSan 정적 라이브러리 | 전체 CTest 24/24 통과 | C++와 Objective-C++를 모두 계측하고 경고를 오류로 처리 |
| macOS Debug ThreadSanitizer 정적 라이브러리 | 전체 CTest 24/24 통과, 경합 보고 없음 | C++와 Objective-C++ 모두 계측; 실행한 스케줄의 검사 |
| WASM / Emscripten / Node | 전체 CTest 22/22 통과 | portable parser/state와 unsupported hardware 계약; 브라우저 장치 접근 증거는 아님 |
| Linux ARM64 / Debian 13 sysroot | 공유 라이브러리·테스트·예제·공개 헤더 전체 교차 컴파일 및 링크 성공 | libudev, D-Bus, ALSA UMP/PCM 구현 컴파일; Linux 실행 및 실물 장치 미검증 |
| iOS ARM64 device SDK | 0.6.0 정적 라이브러리 교차 빌드 성공 | BLE·AudioQueue·CoreMIDI 소스 컴파일; 물리 iPhone 실행 미검증 |
| iOS ARM64 simulator SDK | 테스트·예제·공개 헤더 전체 교차 빌드 성공 | 이번 단계 시뮬레이터 실행은 미검증 |
| Android NDK 30 ARM64 API 26 | 공유 라이브러리·테스트·예제·공개 헤더 전체 교차 빌드 성공 | Hardware native device backend는 미구현; parser/state 및 unsupported 계약의 컴파일 |
| Windows x64 / LLVM-MinGW 20260922 / Windows SDK 10.0.28000.2705 | DLL·테스트·예제·공개 헤더 전체 교차 컴파일 및 링크 성공 | WinUSB·HID·BLE·WASAPI·WinRT MIDI 1.0 구현; Windows 실행 미검증 |
| build/stage 설치 소비자 | find_package 0.6.0, 빌드·링크·동적 로딩 및 CTest 6/6 통과 | 설치 헤더/라이브러리로 protocol/hardware OS 경로 재검증 |

Native CTest는 `build/Testing/Temporary/LastTest.log`, sanitizer는 `build/sanitize/build/Testing/Temporary/LastTest.log` 및 `build/thread-sanitize/build/Testing/Temporary/LastTest.log`, WASM은 `build/wasm/build/Testing/Temporary/LastTest.log`에 기록되어 있다. 두 sanitizer 설정에는 `CMAKE_CXX_FLAGS`와 `CMAKE_OBJCXX_FLAGS` 모두 해당 `-fsanitize=... -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror`를 적용하였다.

macOS hardware 실행은 USB·HID·오디오·MIDI의 네이티브 장치 열거, AudioQueue 기본 출력 열기·무음 PCM 큐 수락·닫기, CoreMIDI 가상 destination 생성·OS 목록 노출·MIDI 2.0 UMP 송신·EventLoop 수신·추가/제거 알림을 검증하였다. endpoint 용량 1에서 8회 생성/제거, 포화 거절, 제거 후 같은 용량 재사용, stale identity 거절을 실행하였다. 마지막 MIDI hub를 완전히 닫아 native notification Thread를 종료·join한 뒤 새 hub로 notification pump와 MIDI 입출력을 재생성하는 검사도 통과하였다. 무음 큐 수락은 실제 스피커의 음향 출력 측정과 구분한다.

초기 headless CoreMIDI 검사에서 제거 알림이 누락되는 문제를 재현하였다. process notification transport가 활성 CFRunLoop를 필요로 하므로 SDK 소유 Thread에서 첫 초기화와 run loop를 관리하도록 수정하였다. native block 복사와 수신 thread의 초기 capture 접근 사이 ThreadSanitizer 경합 보고도 재현하고, 등록 및 초기 capture 읽기에 명시적 publication 동기화를 추가한 후 전체 검사에서 보고가 사라졌다. 테스트 시간 제한이나 검사를 약화하여 통과시키지 않았다.

프로토콜 검사는 USB setup/descriptor 길이와 endian, HID short item/global stack/usage/array/packed signed field/report ID와 실제 비트 범위, BLE UUID/AD/Heart Rate, PCM 유한값·클리핑, UMP 길이와 MIDI 1.0/2.0 note, running status/realtime/SysEx 제한과 status interruption을 포함한다. byte-only MIDI 전송은 MIDI 1.0 channel/system UMP group 0의 양방향 변환을 검사하며, 잘못된 data bit, nonzero group 및 MIDI 2.0 정밀도 손실을 거절한다. state 검사는 합산 payload 상한, FIFO, overflow/drop, 제거 중 active delivery 대기, late callback 폐기, quota 반환과 audio SPSC ring의 동시 순서를 검사한다.

`build/stage`에 설치한 공개 헤더 33개는 원본 SHA-256과 전부 일치하였다. `otool -L`로 0.6.0 설치 dylib의 Apple system frameworks, libc++, libSystem, CoreFoundation 및 Objective-C runtime 의존성을 확인하였다. Qt 의존성이 없다. 이 버전은 네이티브 하드웨어 API를 사용하므로 이전 버전의 표준 라이브러리만 사용하는 의존성 목록을 그대로 적용하지 않는다.

외부 USB/HID의 실물 입력·control/feature write, 실물 unplug/replug, BLE radio discovery/GATT 왕복, 마이크 capture, 물리 MIDI 장치, Linux/Windows 실행 및 원격 CI 실행은 이번 환경에서 미검증이다. BLE 범위는 LE central/GATT이고 Classic RFCOMM은 구현하지 않았다. Android와 WASM의 device-family host bridge는 미구현이며, iOS의 generic USB host/raw HID는 public OS API 지원 범위 밖이다. 임의 제조사의 vendor protocol 의미, USB bulk/isochronous stream, MIDI-CI 및 MIDI scheduling은 포함하지 않았다.

Windows stock SDK에는 `Windows.Devices.Midi2` 프로젝션이 포함되어 있지 않음을 확인하였다. 이 환경의 `midi=true, midi2=false`는 배포된 WinRT MIDI 1.0 API를 의미한다. MIDI 2.0 optional branch의 시그니처는 Microsoft 최신 공식 IDL과 대조하였으나 해당 프로젝션으로 컴파일하거나 Windows MIDI Service에서 실행하지 않았다. 따라서 Windows MIDI 2.0 build/runtime 성공으로 보고하지 않는다. Windows media host는 MTA를 초기화하고 모든 media reference를 닫을 때까지 유지해야 한다.

Windows `HardwareWindows.cpp`는 위 SDK 및 LLVM-MinGW의 C++23 `-Wall -Wextra -Wpedantic -Werror -fsyntax-only` 검사도 통과하였다. `build/windows-probe/hardware-syntax-final.log`는 진단 없이 비어 있고 명령 종료 코드는 0이었다. 설치 버전 메타데이터는 0.6.0이며 설치 소비자는 `build/installed-consumer/build/Testing/Temporary/LastTest.log`에 6개 성공 기록을 남겼다. 전체 검사에는 Objective-C++ 소스 및 macro expansion으로 숨긴 다중 상속을 거절하는 회귀 검사가 포함되어 있다.

완전 종료 후 새 허브의 MIDI 입력만 확인하던 검사를 topology 추가/제거까지 확대한 결과, CoreMIDI가 첫 process notification context를 유지하여 새 notification pump에는 추가 알림이 도착하지 않는 조건을 재현하였다. watched MIDI 종류의 native enumeration을 100 ms마다 대조하고 notification이 오면 더 일찍 대조하도록 수정하였다. 마지막 허브의 thread/client를 계속 남겨두지 않아도 재생성 후 추가·입력·제거가 전달되는 native 회귀 검사가 통과하였다. 이 주기는 cooperative host pump와 native 호출 시간의 영향을 받는다.

`OBJC_DEBUG_MISSING_POOLS=YES`로 GUI 루프가 없는 C++ 호스트를 실행하여 Foundation 임시 객체의 autorelease pool 누락 진단을 재현하였다. Apple native 명령·수집·파괴 진입점과 SDK 소유 MIDI worker에 pool을 구성한 뒤 같은 native 실행은 종료 코드 0이고 `MISSING POOLS` 진단이 0건이었다. 이 진단을 Apple hardware CTest 및 설치 소비자의 실패 조건으로 고정하였다. 이는 실행 경로의 autorelease pool 검증이며 모든 OS 내부 allocation의 누수 부재를 증명하는 것은 아니다.

CoreMIDI UniqueID의 실제 SInt32 계약에 맞추어 열거 결과를 부호 있는 숫자로 표시하고 음수 식별자도 받아들이도록 수정하였다. 기존 코드의 unsigned 표시도 같은 32-bit 식별자의 별칭으로 받아들인다. private 식별자 parser는 INT32_MIN/INT32_MAX, high-bit unsigned 별칭, 범위 초과, 잘못된 prefix, 빈 ID 및 trailing data를 회귀 검사한다. 이는 ID 해석 검증이며 음수 UID를 가진 외부 MIDI 기기의 실행 증거와 구분한다.

## 시간 기반 시분할·라이브 스트리밍 0.7.0 검증 — 2026-10-06

Core/Time/Time.h에 단조 시간축의 포화 연산·주기 위치·TimeSlice를 추가하였다. Deadline에서 Runtime 의존을 제거하고 EventLoop가 이 하위 시간 모듈을 사용하도록 정리하였다. EventLoop::realtime은 주기·마감·실행 예산·quantum을 예약하고 준비된 작업을 EDF 순서로 선택한다. LiveStream은 제한된 타임스탬프 저장소와 같은 시간 예약 경로를 조합한다. 상세 계약은 [realtime.md](realtime.md)에 기록하였다.

새 테스트를 먼저 CMake에 연결하였으며, Time.h/LiveStream.h가 없는 최초 빌드가 필수 architecture 전처리 단계에서 실패함을 확인하였다. 구현 후 기존 런타임·I/O·하드웨어·제거·상속 검사와 함께 다음 구성을 실행하였다.

| 최종 변경본 구성 | 결과 | 증거 범위 |
|---|---|---|
| macOS ARM64 Release 공유 라이브러리 | 전체 CTest 26/26 통과 | 시간 예약·타임스탬프 전달 및 기존 계약 회귀 |
| macOS ASan + UBSan | 전체 CTest 26/26 통과 | C++/Objective-C++ 계측, 경고 오류 처리 |
| macOS ThreadSanitizer | 전체 CTest 26/26 통과, 경합 보고 없음 | 활성 전달과 제거를 다른 Thread에서 실행 |
| WASM 명시적 단일 스레드 / Node | 전체 CTest 24/24 통과 | Time·EDF·타임스탬프 계약; 병렬 제거·blocking run 검사는 생략 |
| WASM 기본 pthread / Node | 전체 CTest 24/24 통과 | 워커에서 native-driver 의미론 및 활성 전달/제거 검사 실행; 브라우저 시간 보장 증거는 아님 |
| build/stage 설치 소비자 | find_package 0.7, 빌드·동적 링크 및 CTest 7/7 통과 | 설치된 Time/LiveStream 및 realtime API 재검증 |
| Windows x64·Linux ARM64·Android ARM64 | 라이브러리·테스트·예제·공개 헤더 전체 교차 컴파일 및 링크 성공 | 새 시간/스트리밍 구현의 컴파일; 해당 OS 실행 미검증 |
| iOS ARM64 기기 및 시뮬레이터 대상 | 기기 정적 라이브러리, 시뮬레이터 테스트·예제·공개 헤더 교차 빌드 성공 | 이번 변경본의 시뮬레이터/물리 기기 실행 미검증 |

tests/realtime.cpp는 signed epoch 양끝의 시간 덧셈·차이와 period 위치, EDF 순서, 동일 마감 continuation 순환, runtime/period 과예약 거절, invalid quantum, 영구 종료·예외·취소의 할당량 반환을 검사한다. 24시간 늦은 주기는 현재 열려 있는 주기 한 번만 실행하고 과거 주기를 callback burst로 실행하지 않는지 확인하였다. 캡처 소멸자가 새 예약을 제출하는 검사에서는 기존 예약량이 소멸 중에도 보존되고, 소멸 후에 재사용되는지 확인하였다.

LiveStream 검사는 지연 한도 초과 프레임 폐기, 미래 프레임 보존, timestamp 역행 거절, 포화 시 오래된 프레임 폐기, 닫힌 생산자 거절을 포함한다. 활성 handler를 latch로 유지한 상태에서 다른 Thread가 close를 시작하고, 생산 차단 이후에도 활성 전달이 끝나기 전에는 close가 반환하지 않는지 확인하였다. 종료 후 handler·버퍼 payload의 공유 소유권이 소멸하고 EventLoop pending이 0인지 검사하였다. own-handler close는 busy를 반환한다.

native run 검사는 quantum 100 us, runtime 2 ms, period/deadline 5 ms인 작업에서 첫 callback을 3 ms 지연시켜 budget exhaustion과 slice overrun을 관측한 뒤 다음 실행에서 종료하였다. 이는 협력적 실행의 과부하 관측과 시간 구동 경로 검사이며, 강제 선점이나 OS 지연 상한의 증명이 아니다.

examples/realtime.cpp는 재생 시각이 다른 96-sample PCM 블록 8개를 순서대로 전달하고 총 768 samples, energy=48을 대조하였다. 최종 macOS 실행 기록은 slices=20, deadline_misses=0이었다. 이는 PCM 데이터 처리 예제이며 실제 마이크·스피커 왕복 지연을 측정한 것이 아니다. 기본 양자·호스트 실행 상태에 따라 slice 횟수와 마감 통계는 달라질 수 있다.

tests/reuse.cpp의 준비된 시간 예약·trivial payload 스트림 경로를 10,000회 재사용한 결과 operator new calls=0이었다. 전체 Core·임의 payload·사용자 capture·OS I/O의 무할당 또는 lock-free 보장은 아니다. 오래된 프레임 폐기 루프에도 slice 종료를 검사하여 큰 stale backlog를 한 callback에서 무제한 처리하지 않는다.

설치 헤더 35개 모두 원본 SHA-256과 일치하고 설치 0.7.0 dylib가 최종 빌드와 동일한지 확인하였다. 기록은 build/architecture/realtime-installed-probe.json이다. 설치는 Workspace의 build/stage이며 ~/.local은 갱신하지 않았다. 필수 architecture 검사는 native/cross 구성 번역 단위 66개, WASM 67개에서 통과하였다. 실제 브라우저 timing, Linux/Windows/모바일 실행, 하드 리얼타임 보장, 장시간 부하·실제 미디어 I/O 왕복은 미검증이다.

WASM pthread 최초 검사에서는 새 Realtime 테스트 타깃이 기존 worker-program 목록에 없어서 호스트 스레드의 Thread::start가 would_block으로 거절되는 문제를 재현하였다. 새 타깃에 기존 PROXY_TO_PTHREAD·worker pool·host blocking 금지 설정을 연결한 뒤 전체 24개 검사가 통과하였다. 검사를 생략하거나 호스트 스레드 보호를 해제하지 않았다. 두 WASM 로그는 build/wasm/build 및 build/wasm-threads/build 아래 Testing/Temporary/LastTest.log에 각각 저장되어 있다.


## 0.8.0 modern core families — 2026-10-06

This pass implements and validates the seven-family common runtime subset in [modern-core.md](modern-core.md): animation, parallel transforms, direct standard container/PMR use, native/static plugins, immutable shared snapshots, atomic file replacement and query-composed event filters. Package version is 0.8.0. Earlier 0.6/0.7 tables remain historical evidence.

| Configuration | Result | Scope |
| --- | --- | --- |
| macOS Release, shared runtime | 29/29 CTests | Full runtime regression set, two native plugin modules, explicit destroy-before-unload marker |
| macOS ASan + UBSan, static runtime | 29/29 CTests | C++ and Objective-C++ instrumented; -Wall -Wextra -Wpedantic -Werror |
| macOS ThreadSanitizer, static runtime | 29/29 CTests | Concurrent edit conflicts, active frame removal, native/static plugin removal and existing hardware callbacks |
| WASM single thread, Node 24 | 27/27 CTests | Portable values, static plugins, virtual file transactions; worker-dependent contracts explicitly skipped |
| WASM pthread, Node 24 | 27/27 CTests | Worker execution, parallel cancellation/futures and active removal; values-only target remains independent |
| Native installed package | 10/10 CTests | find_package 0.8, installed headers/library, modern contracts and static plugin consumption |
| Values-only native consumer | 1/1 CTest | Thread/PkgConfig discovery disabled; no runtime target imported, executable loads only libc++/libSystem |
| Values-only Linux-package consumer on macOS | 1/1 CTest | Linux hardware dependencies omitted; native code compiled only from portable installed value headers |
| Values-only native-package consumer in WASM | 1/1 CTest | 64-bit native package to 32-bit WASM; independent export and Emscripten exception flags, no runtime ABI import |
| Windows x64 / Linux ARM64 / Android ARM64 | Core, examples, tests, native plugin fixtures and public header checks cross-built | No target execution |
| iOS ARM64 / iOS simulator ARM64 | Device static core; simulator core/tests/examples/header checks cross-built | Static plugin registration, no target execution |

The architecture gate audits 81 translation units on native/Windows/Linux/Android, 79 on iOS simulator, and 19 on the device configuration with testing disabled. Source and preprocessed record checks remain mandatory; unchanged preprocessing results are cached. MODULE plugin fixtures are included in the gate. All 42 installed public headers match source SHA-256; the staged 0.8.0 dylib matches the built dylib. Probe details are in build/architecture/modern-installed-probe.json. Staging is under build/stage; ~/.local was not modified by this pass.

The tests cover strictly ordered keyframes, easing, sequential/overlapping tracks, pause/seek/play and callback failures; reentrant control rejection; active frame removal and weak capture retirement; ordered parallel outputs, empty/invalid batches, mapper exceptions, queued cancellation and partial admission failure; retained immutable snapshots and stale concurrent commits; event acceptance filters in a SerialQueue; commit/abort file visibility and temporary cleanup; static/native plugin ABI rejection, bounded outputs, active callback removal and actual module unload order. Standard PMR storage is reused 1,000 times after container destruction and rejects over-budget allocation without a heap fallback.

TDD began with missing public headers rejected by the architecture preprocessor. An initial native regression exposed an unresolved future after partial parallel admission while an unrelated worker was blocked; failure now cancels previously queued chunks and settles every ticket. A values-only WASM throwing-edit probe exposed missing exception support when the runtime was omitted; compile/link flags now belong to the Values target itself. Cross-package probes then exposed native pointer-width rejection and attempted import of a shared runtime target on WASM; Values now has a separate export and value-only version path, while full-runtime bitness rejection is retained and negatively checked.

The comparative Qt references define requirements, not benchmark results. This pass supplies no paired Qt benchmark, production soak test, GUI framework, filesystem watcher, process/IPC supervisor, URL/settings/resource framework, codec/media networking stack, hard preemptive real-time guarantee or new physical-device proof. Full Qt replacement and shipped-product parity remain unverified.

## 0.9.0 data type serialization — 2026-10-06

This pass adds the standard-only typed CBOR profile, bounded complete-message parsing, explicit application codecs and an instance-owned DynamicValue codec registry documented in [serialization.md](serialization.md). Version is 0.9.0. The wire engine, typed codecs and runtime adapter have separate headers; the first two remain available through the independent Values export.

| Configuration | Result | Scope |
| --- | --- | --- |
| macOS Release, shared runtime | 32/32 CTests | Full regression suite, static/dynamic serialization and asynchronous query example |
| macOS ASan + UBSan, static runtime | 32/32 CTests | C++/Objective-C++ instrumentation; warning-as-error build |
| macOS ThreadSanitizer, static runtime | 32/32 CTests | Concurrent dynamic encode/decode on four persistent Runtime workers |
| WASM single thread, Node 24 | 30/30 CTests | Golden bytes, typed/dynamic roundtrips, virtual-file persistence; worker-only checks conditional on support |
| WASM pthread, Node 24 | 30/30 CTests | Same format suite plus concurrent registry use and asynchronous serialization query |
| Native installed package | 12/12 CTests | Installed 0.9 headers/runtime and both serialization suites |
| Values-only native consumer | 2/2 CTests | Full typed serialization suite; runtime/Threads/PkgConfig imports forbidden |
| Values-only Linux-package consumer on macOS | 2/2 CTests | Linux hardware dependencies omitted, portable installed serialization consumed on another OS |
| Values-only native-package consumer in WASM | 2/2 CTests | Native 64-bit package to WASM32, pure serialization without runtime ABI import |
| Windows x64 / Linux ARM64 / Android ARM64 | Core, examples, tests and public header checks cross-built | No execution on target systems |
| iOS simulator ARM64 | Core, examples, tests and public header checks cross-built | No simulator execution |
| iOS device ARM64 | Static core plus separate Values-only installed consumer cross-built | Typed serialization consumer app binaries compiled/linked; no device install or execution |

The typed suite compares exact RFC/profile byte fixtures, every one of the 65,536 binary16 bit patterns, 2,000 seeded random integer and binary64 roundtrips, 2,000 malformed random inputs, and every truncated prefix of an application record. It verifies unsigned/signed boundaries, float precision loss, signed zero/NaN normalization, embedded NUL and UTF-8 validity, sorted standard maps, normalization of unsorted input, rejected duplicate/unsorted custom map output, nested optionals, tuple/variant/fixed-array schemas and byte/blob/container/item/depth/storage limits. These are deterministic regression probes, not an exhaustive security fuzz campaign or throughput benchmark.

Dynamic tests preserve exact registered C++ types, distinguish empty any from explicit null, reject unsupported borrowed/long-double values, unknown IDs, duplicate ID/type registration, registry capacity overflow and version mismatch. A concrete StoredSample codec migrates version-zero payloads to version one. Throwing codecs preserve causes and ignored decoder errors remain failed. Owned field counters return to zero after a successful scope, a partially constructed tuple fails, and a subsequent successful decode completes. AtomicFile -> Channel -> DynamicSerializer verifies persisted data restoration. Test files remain inside their build working directory.

Initial TDD rejected the absent public header in the mandatory architecture preprocessor. A real WASM build then exposed an older libc++ expected<any> converting-constructor constraint cycle and missing jthread. The adapter now constructs its successful expected value in place, and concurrent tests use existing Runtime workers. Both WASM configurations subsequently pass their full suites; the initial failure is retained in build/wasm-threads/build/serialization-first-failure.log.

The architecture gate audits 87 translation units on native/Windows/Linux/Android, 85 on iOS simulator, and 19 on the device core configuration with testing disabled. All 45 staged public headers match source SHA-256, and the staged 0.9.0 dylib matches the native build. The independent native serializer executable loads only libc++ and libSystem. Proof and log paths are recorded in build/architecture/serialization-installed-probe.json. Staging is inside Workspace at build/stage and build/linux-arm64/stage; ~/.local is unchanged.

The implementation is a defined CBOR profile, not every CBOR tag/simple value, JSON/XML serializer, automatic object reflection, encrypted transport or resource compiler. Requested-storage limits exclude allocator metadata and unaccounted custom-code allocations; no exact RSS cap or allocation-free real-time guarantee is claimed. Codec external effects and thread safety remain explicit application responsibilities. Physical target execution, actual browser execution, third-party CBOR implementation interoperability and production endurance remain unverified by this pass.

## 0.10.0 implicit sharing — 2026-10-06

This pass extends SharedValue with automatic copy-on-write modification and adds its optional typed serialization bridge, as documented in [implicit-sharing.md](implicit-sharing.md). Ordinary copies and assignments retain storage without copying T; short serialized `modify` calls detach shared or previously published blocks. Private, unpublished storage is reused for a noexcept editor. Existing optimistic `update` transactions keep their conflict and rollback contracts.

| Configuration | Result | Scope |
| --- | --- | --- |
| macOS Release, shared runtime | 34/34 CTests | Complete regression set, value sharing and runtime integration |
| macOS ASan + UBSan, static runtime | 34/34 CTests | C++/Objective-C++ instrumentation and warning-as-error build |
| macOS ThreadSanitizer, static runtime | 34/34 CTests | Same-instance writes, weak snapshot upgrades during modification and stale transaction conflicts; no race report |
| WASM single thread, Node 24 | 32/32 CTests | Portable sharing, dynamic values and serialization; worker-only checks conditional on support |
| WASM pthread, Node 24 | 32/32 CTests | Worker query payloads, concurrent modification and weak observer checks |
| Native installed package | 14/14 CTests | Installed 0.10 headers/runtime and both new sharing suites |
| Values-only native consumer | 3/3 CTests | Sharing and serialization without runtime, Threads or PkgConfig imports |
| Values-only Linux-package consumer on macOS | 3/3 CTests | Portable sharing from Linux's staged headers without hardware dependency discovery |
| Values-only native-package consumer in WASM | 3/3 CTests | Native package headers compiled/executed in WASM32; no runtime ABI import |
| Windows x64 / Linux ARM64 / Android ARM64 | Core, tests, examples and public header checks cross-built | New sharing suites compiled/linked; no target execution |
| iOS simulator ARM64 | Core, tests, examples and public header checks cross-built | No simulator execution |
| iOS device ARM64 | Static core and three Values-only consumer apps cross-built | New sharing app compiled/linked; no install or device execution |

The pure value suite records zero T copies for normal copy/assignment, exactly one copy for the first shared modification, and zero copies across 1,000 private noexcept modifications. It checks immutable retained snapshots, expired weak aliases, exception rollback, recursive access rejection, last-strong-owner destruction, logical serialization bytes and requested payload storage limits. A 64 KiB typed query payload records one T copy at the receiver's modification; request/response transport copies retain sharing. These copy counts are specific regression evidence, not Qt comparison benchmarks or a general zero-allocation claim.

The weak observer regression leaves only a weak Snapshot alias outside the value, pauses a worker midway through modification, and upgrades the alias on another thread. The observer must read the old immutable value throughout the commit. The publication marker remains sticky on an exposed block, and an atomic read-modify-write observes publication before allowing private reuse. Strong reference counts alone do not authorize mutation. Four persistent workers also complete 2,000 same-instance increments with revision 2,000; an older optimistic transaction returns busy after an intervening modify.

DynamicValue copies retain the SharedValue payload and separate receiver writes. A registered dynamic codec restores the exact shared C++ type. Typed serialization persists only T; decoded storage has revision zero and independently resumes copy-on-write behavior. Owned payload counters return to zero after values, query captures and snapshots retire. Weak owners can keep standard control metadata alive without retaining T. Complete rejected candidates and replaced blocks are destroyed outside the owning instance lock.

Initial TDD rejected the absent optional serialization bridge header in the mandatory architecture preprocessor. The implemented value suite then passed before runtime integration and the full matrix. The architecture gate passes 90 translation units on native/Windows/Linux/Android, 89 on WASM, 88 on the iOS simulator and 19 on the device configuration with testing disabled. Source and preprocessed public-record checks remain mandatory; cached unchanged preprocessing is retained.

All 46 staged public headers match source SHA-256, and the staged 0.10.0 dylib matches the native build. The Values-only sharing executable loads only libc++ and libSystem. Logs, file hashes and scope are recorded in build/architecture/implicit-sharing-installed-probe.json. Installation remains inside Workspace at build/stage and build/linux-arm64/stage; ~/.local was not modified. Consumers must rebuild because the template's internal storage layout changes in 0.10.0.

Raw standard containers inside DynamicValue retain their standard copying behavior; applications select implicit sharing by storing SharedValue<T>. T must own mutable contents or supply appropriate copy semantics, and callback borrows must not escape. No lock-free, hard real-time, arbitrary deep-clone, paired Qt performance, physical-target execution, actual browser execution or production endurance claim follows from this matrix.

## 0.11.0 app/game frame loop — 2026-10-14

The new [FrameLoop contract](frame-loop.md) is implemented as native C++23
composition over EventLoop and the existing DependencyGraph. Unity/Unreal official
documentation informed scheduling requirements without introducing an engine or
VM dependency. The versioned package includes the new public header, native
symbols, a shared app/game example and an installed-package contract test.

| Configuration | Result | Evidence boundary |
| --- | --- | --- |
| macOS arm64 Release/shared | 36/36 CTests | Complete suite, example, public headers and mandatory architecture gate |
| macOS Debug/static, ASan + UBSan | 7/7 focused CTests | FrameLoop, example, EventLoop, realtime, I/O, cleanup and allocation reuse; warnings are errors |
| Relocated installed package | 15/15 CTests | Independent find_package(0.11), headers, exported symbols and shared-library loading |
| Emscripten pthreads, Node | 1/1 FrameLoop CTest | Deterministic scheduling/time/lifetime and worker-affinity contracts |
| Actual in-app browser | Browser contracts and app/game example passed | requestAnimationFrame, UI during pause, resumed simulation, failure containment and stop |
| Windows x64 | New tests and example compiled/linked | PE32+; no Windows execution |
| Linux arm64 | New tests and example compiled/linked | ELF; no Linux execution |
| Android arm64 | New tests and example compiled/linked | NDK ELF; no device execution |
| iOS arm64 | Core static library built | FrameLoop object is Mach-O, platform IOS, minimum OS 15.0, SDK 27.0; no device execution |

The deterministic suite checks all ten phases and repeated fixed triplets,
fixed/real clocks, interpolation, clamping and bounded backlog discard, pause and
time scale, coalesced intervals, enablement snapshots, cyclic/backward dependencies,
stale/foreign identities, capacity, callback mutation, self removal, capture
retirement, exceptions, recursion/affinity and native driver cleanup. A rejected
timer reservation or invalid pump budget preserves other owners' event work.
Initial TDD failed on the absent FrameLoop header in the mandatory architecture
preprocessor before implementation.

The allocation suite executes 10,000 stable warmed-up frames with zero additional
ordinary operator-new calls. This excludes schema changes, arbitrary captures and
OS allocations. A native Mach-O example also completes under `env -i PATH=`.
The browser's observed outputs are `browser contracts passed` and
`app/game frame loop complete: frames=8 UI=8 fixed=23`; frame cadence is owned by
the browser and the fixed-step count is an observation, not a required constant.
The test server supplied COOP/COEP isolation and was stopped after verification.

All 47 installed public headers match source SHA-256 and the installed 0.11.0
dylib matches the native build. The package was installed and then moved to
`build/frame-package/relocated` before configuring its independent consumer in
`build/frame-package/consumer/build`. The mandatory architecture gate audits 94
translation units on native/Windows/Linux/Android, 93 on WASM and 20 in the
tests-disabled iOS device configuration. No policy bypass was added.

Commands and diagnostics are supported by these machine-readable reports:

- `build/frame-loop-verification.json`: source/artifact hashes, package checks,
  test counts, browser observations and platform execution boundaries.
- `build/frame-loop-native-tests.xml`: complete native CTest output.
- `build/sanitize/build/frame-loop-sanitizer-tests.xml`: sanitizer output.
- `build/frame-package/consumer/build/frame-loop-installed-tests.xml`: relocated consumer output.
- `build/wasm-threads/build/frame-loop-wasm-tests.xml`: Node contract output.

Runtime execution on Windows/Linux/Android/iOS, mobile device installation,
OS-specific window/input lifecycle integration and real rendering/physics consumers
are unverified. WASM is a separate WebAssembly host execution target. The five OS
targets compile native machine code; no common VM is required. Installation for
this verification is inside Workspace; the global `~/.local` installation was not
updated. Frame budgets remain cooperative and do not promise hard real-time work.

## 0.12.0 device/media extraction — 2026-10-15

USB/HID, BLE central/GATT, float32 PCM and MIDI UMP source, protocol helpers,
native bridges, tests and inventory example now belong to iiDeviceMultimedia
0.1.0. Core retains generic runtime, FrameLoop, typed queries, threads,
serialization, Channel and AsyncIO. The device library depends on Core 0.12+;
Core has no upward link, forwarding device header or native media dependency.
Migration and install-upgrade behavior are documented in
[device-extraction.md](device-extraction.md).

| Configuration | Result | Evidence boundary |
| --- | --- | --- |
| macOS arm64 Release/shared Core | 34/34 CTests | Complete retained suite, examples, mandatory policy and new device-boundary guard |
| macOS ASan + UBSan/static Core | 12/12 focused CTests | FrameLoop/example, Runtime, Query, Thread, shared runtime, realtime, generic I/O, cleanup, reuse and boundary |
| Relocated Core-only package consumer | 13/13 CTests | Core 0.12 installed over an actual 0.11 prefix; no device/PkgConfig imports |
| Values-only installed consumer | 3/3 CTests | No runtime, Threads or device imports |
| New device module with source Core | 4/4 native CTests and 4/4 sanitizer CTests | Existing functionality moves without algorithm or assertion changes |
| New device module with installed Core | 4/4 native CTests | No Core source/tools needed; public dependency interface works |
| Relocated composed package consumer | 3/3 CTests | Device API/protocols, native hardware contracts and retained FrameLoop coexist |
| Windows x64 / Linux arm64 / Android arm64 | Core/device libraries, tests, public headers and inventory example cross-built | Native PE/ELF output; no target execution |
| iOS arm64 | Core/device static libraries cross-built | Native Apple bridge object, IOS 15.0 minimum, SDK 27.0; no device execution |
| WASM32 pthreads | 3/3 device/state/protocol CTests in Node | Explicit unsupported hardware backend; no new browser hardware bridge |

Three device CTests moved out of Core and the new boundary guard was added, so
the prior 36-test Core set becomes 34. The standalone mandatory architecture gate
passes 85 translation units. The source ownership test first rejected the
pre-extraction device files; it now checks source ownership, generated headers,
compiled units, actual target link requirements and installed package imports.

The extraction comparison verifies 14 moved code/test/example files after only
include-path and OS client/queue/test label changes. Eight existing FrameLoop and
browser source/test/example files preserve their previously recorded hashes;
the other 61 retained tracked Core source files match their original Git bytes.
Existing uncommitted work and original extraction inputs are preserved under
`build/device-extraction-baseline/`.

All 45 installed Core headers match the generated public headers and the new
0.12 dylib matches the native build. Its macOS dynamic dependencies are only
libc++/libSystem; Hardware, hardware_capabilities and protocol symbols occur only
in the device library. The Core 0.11 upgrade removes only its two obsolete device
headers while preserving unrelated prefix data and the old versioned dylib.
DESTDIR and a prefix with spaces preserve an adjacent generic header and leave
the unstaged prefix untouched. Core 0.11 is explicitly rejected by the new device
package to prevent duplicate old/new hardware implementations.

Proof is recorded in `build/device-extraction-verification.json`, native results
in `build/device-extraction-core-tests.xml`, sanitizer results in
`build/sanitize/build/device-extraction-sanitizer-tests.xml`, and package results
under `build/device-package/`. The device module's
[validation](../../iiDeviceMultimedia/docs/validation.md) records its source,
platform, native hardware and sanitizer boundaries, including an initial
architecture timeout followed by an unchanged successful rerun. Installation
remains inside Workspace; the global `~/.local` SDK was not modified. Physical
USB/HID/BLE/audio hardware operations and other-OS runtime execution remain
unverified by this extraction pass.

## Peer iiDeviceMultimedia 0.2.0 — 2026-10-15

The subsequent peer-module refactor removes the dependency in the other direction
as well: iiDeviceMultimedia 0.2.0 no longer includes, imports, configures, links or
calls Core. Both packages run independently and the application explicitly
composes `ii::device::Hardware::poll()` with Core's existing EventLoop/FrameLoop.
Device errors/codecs belong to the distinct `ii::device` namespace. No shared SDK
base, Core adapter, optional package edge or replacement Core runtime is added.

Core remains 0.12.0. All 65 Core source implementation/header files and its build
and package definitions match the preceding extraction snapshot. The full native
suite passes 34/34 again. Its separate installed consumer passes 13/13 with device
package lookup disabled, preserving generic I/O, scheduling, frames, values and
cleanup. Existing FrameLoop work is unchanged.

The standalone device package passes 6/6 native CTests, 6/6 ASan/UBSan CTests,
6/6 ThreadSanitizer CTests, 3/3 relocated installed-consumer CTests with Core lookup
disabled, and 6/6 WASM/Node CTests. A separate application consumer passes once in
each package-import order and compiles both header orders. It drives device poll
from a Core timer, routes a device protocol packet through Core Channel file I/O,
receives one OS-routed CoreMIDI UMP note, retires the timer/captures and continues
Core frames after device shutdown. Both recorded runs report
`frames=7 polls=5 native_notes=1`.

Binary inspection confirms no cross-library dependency in either direction.
The standalone device executable imports no Core, while the application
composition explicitly imports both. SDK installation discovery reports no edge
between the projects. Device Windows/Linux/Android/iOS native artifacts and WASM
are independently cross-built without a Core target; other-OS and physical-device
execution limits remain explicit in the device validation.

Current migration and shutdown requirements are in
[device-extraction.md](device-extraction.md) and the device
[independence contract](../../iiDeviceMultimedia/docs/independence.md). In
particular, the application retires its external polling driver before closing
or destroying the hub. Foreign active-poll close stops admission and returns busy
for owner retry, without a Core worker-thread dependency.

New proof is `build/device-independence-verification.json`; Core native output is
`build/device-independence-core-tests.xml` and installed output is
`build/device-package/core-consumer/build/device-independence-installed-core-tests.xml`.
The device [validation](../../iiDeviceMultimedia/docs/validation.md) records its
current tests and binary/namespace evidence. Earlier 0.1 extraction results above
retain their historical dependency boundary. Verification installs stay inside
Workspace and the global ~/.local SDK is unchanged.

## Native process P1 — 0.15.0 (2026-10-16)

This completes the process/pipes/child-environment slice of P1 with an actual
consumer, iiSDKBuilder 0.1.1. C++23 source, public headers, installed packages,
source and preprocessed architecture checks, dependency direction and file
ownership are covered. Core imports no Qt or upper SDK, including FileProvider.

| Verification | Result |
|---|---|
| macOS arm64 Release Core suite, `build/foundation` | 39/39 CTest entries |
| Installed Core consumer with Qt/provider discovery disabled | 16/16; includes 104 native process checks |
| Independent installed Contracts consumer, Threads/feature discovery disabled | 2/2 |
| Relocated installed process consumer, `build/p1-relocated/consumer/build` | 1/1; 104 process checks |
| AddressSanitizer + UndefinedBehaviorSanitizer, Process/NativeFile/Operation/LoopIO | 4/4; no findings |
| ThreadSanitizer Process | 1/1; 104 process checks, no findings |
| WASM source and installed process consumers under Node | 1/1 each; four assertions of explicit unsupported behavior |
| iiSDKBuilder Release suite | 12/12, including native Core → Addon → app build and relocated package/CLI consumer |
| SDK dependency direction | 34 declarations, acyclic; Core and Native remain independent |
| Workspace file creation audit | no violations or unreadable sources |

The process checks exercise literal/empty/Unicode argv, Unicode child variables,
unset versus empty values, inherited/clean environments, parent cwd/environment
preservation, absolute and relative child PATH/cwd, independent/merged/discarded
streams, closed input and closed host stdio, binary duplex beyond pipe capacity,
bounded capture plus full streaming, early stdin close/SIGPIPE, stable normal and
signal exits, wait-without-cancel, deadline/stop/explicit tree cancellation,
remaining descendants on normal root exit, direct-child reaping, owner removal,
move replacement, throwing sinks, concurrent communication/cancel, invalid
inputs, unrelated handle isolation, EventLoop pipe reads, repeated resource reuse
and failed-spawn cleanup. A real Darwin probe reproduced EPERM for a group whose
only member was a zombie; Core verifies dead membership before treating that
case as complete, with leader identity pinned until reaping.

| Target artifacts and installed consumer | Evidence in this change | Execution limit |
|---|---|---|
| macOS arm64 | Mach-O library and consumer, full native execution | tested on this host |
| Windows x86_64 | LLVM-MinGW 23, PE DLL and consumer EXE cross-build/link | no Windows execution |
| Linux aarch64 | glibc 2.41 sysroot, ELF library and consumer cross-build/link; cwd/closefrom backend enabled | no Linux execution |
| Android arm64 API 34 | ELF library and consumer cross-build/link; native spawn/cwd backend enabled | no device execution or app-policy proof |
| Android arm64 API 26 | ELF library and consumer cross-build/link; subprocess capability disabled at configure | no device execution |
| iOS arm64 | static library and Mach-O app consumer cross-build/link; subprocess unsupported | no device execution |
| WASM | binary module plus installed consumer; unsupported subprocess/environment tested under Node | no browser or host-child-process execution |

Direct Mach-O dependency inspection shows Core linking libc++ and libSystem.
Runtime loader traces for the installed Core consumer and Builder CLI contain no
Qt libraries; the CLI reports 0.1.1 from its CMake project version. Source hashes,
artifact formats, test XML, dependency/audit output and loader evidence are
collected in `build/p1-verification.json` and the adjacent p1 logs. All installs
are isolated prefixes under build/, including package relocation; this check
does not update the user's global SDK installation or running applications.

Reproduce the native path (Core repository cwd):

```sh
cmake -S . -B build/foundation -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build/foundation -j 4
ctest --test-dir build/foundation --output-on-failure
cmake --install build/foundation --prefix "$PWD/build/foundation/install"
cmake -S tests/process-consumer -B build/process-consumer/build \
  -DiiSDKCore_DIR="$PWD/build/foundation/install/lib/cmake/iiSDKCore"
cmake --build build/process-consumer/build -j 4
ctest --test-dir build/process-consumer/build --output-on-failure
```

These results concern the native process slice. Remaining P1 feature migrations
continue to follow the concrete-consumer boundary described in process.md and
qt-migration-p0.md; they are not inferred from process-test success.
