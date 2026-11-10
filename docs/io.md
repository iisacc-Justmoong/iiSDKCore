# 플랫폼 독립 I/O

## 터치 표본과 제스처

`ii::runtime::touch::TouchRouter`는 표면 좌표·시간·필압·펜 기울기와 식별자를
가진 입력 표본을 순서대로 처리한다. 기기 접근과 화면 획득은 플랫폼 백엔드가
담당하며 Core는 UIKit이나 다른 GUI 모듈에 의존하지 않는다. iPadOS의 일반 앱은
운영체제가 전달한 터치 표본을 사용한다. DriverKit의 외장 장치 지원을 내장
터치스크린에 대한 직접 접근으로 표현하지 않는다.

스크롤 가능한 영역에서 한 손가락이 주로 세로로 움직이면 내용이 손가락과 같은
방향으로 움직이는 `pan`을 방출한다. 가로 움직임은 슬라이더 등의 포인터 입력으로
되돌리며 대기 중 보관한 표본을 순서대로 전달한다. 일반 그리기 영역의 펜 표본은
대기 없이 전달한다. 두 손가락의 중심 이동과 거리 비율은 각각 증분 `pan`과
`pinch`로 방출한다. 제스처가 시작되면 기존 포인터를 취소하고, 한 손가락만 남아도
새 그리기를 시작하지 않는다. 펜이 입력 중일 때 닿은 손가락은 해당 획이 끝날 때까지
무시하며, 펜이 이미 닿은 손가락보다 나중에 시작되면 포인터를 취소하고 펜이
입력을 이어받는다. 이동 임계값은 생성 시 지정할 수 있다. 관성·회전
제스처는 이 계약에 포함하지 않는다.

라우터는 표면별로 소유하며 한 입력 스레드에서 호출한다. 결과는 고정 크기 묶음이고
재진입 시 `revision()`으로 오래된 전달을 중단할 수 있다. 유효하지 않은 표본은
거부하며 상태를 새 제스처로 변경하지 않는다.

## 터치 이동과 관성 계약

`TouchMotion`은 운영체제나 위젯에 의존하지 않는 표본 기반 속도 추정과 프레임 기반
감속을 제공한다. 입력 이동량은 표면의 논리 좌표를 따르며, 최근 표본과 놓는 시각을
이용해 속도를 구한다. 오래 정지한 뒤 놓거나 시각이 역행하는 표본으로는 관성을
시작하지 않는다. 감속은 경과 시간에 따라 적분해 60Hz와 120Hz에서 같은 시간 동안
같은 거리를 이동한다. 새 터치, 취소, 이동 불가능한 축 및 긴 프레임 중단은 해당
움직임을 중지한다. 속도 추정과 감속은 표면의 이벤트 스레드에서 소유하고,
스케줄링과 실제 콘텐츠 경계는 호출자가 맡는다.

기본 정책은 최근 약 120ms의 표본과 100ms의 놓기 유효 시간을 사용한다. 감속률과
최소·최대 속도, 프레임 중단 허용 시간은 `MotionPolicy`로 교체할 수 있다. 다음
예제에서 이벤트 시각과 프레임 시각은 각각 단조 증가하는 초 단위 값이며, 두 시계가
동일한 원점을 가질 필요는 없다.

```cpp
#include <iiSDKCore/Runtime/TouchMotion.h>
using namespace ii::runtime::touch;
PanVelocity samples;
KineticMotion motion;
samples.begin(event_start_seconds);
samples.add({dx, dy}, event_seconds);
if (motion.start(samples.release(release_seconds), frame_seconds)) {
    auto frame = motion.advance(next_frame_seconds);
    // frame.delta를 대상에 적용하고 막힌 축은 motion.block(x, y)로 중단한다.
}
```

`PanVelocity`는 고정 크기 이력과 방향별 속도를 보유한다. `KineticMotion`은
디스플레이 타이머를 만들지 않으며, 입력 취소에는 `cancel()`을 사용한다.

0.13부터 일반 파일 열기·생성은 Core 바깥의 iiFileProvider CoreIO 어댑터가 담당한다.
이 어댑터로 연 쓰기 가능한 Channel은 성공적으로 닫힐 때 iiFileProvider Native 0.7에
최종 파일 경로를 전달한다. 마지막 소유 참조의 정리에도 같은 처리를 적용한다.
Provider는 별도 네이티브 워커에서 상세 정보와 프리뷰를 생성한다. 읽기 전용 채널과
시리얼 채널은 이 알림을 생성하지 않으며, 메타데이터 생성 실패가 파일 닫기 성공을
취소하지 않는다. 임시 파일의 원자적 공개도 어댑터가 소유한다. Core는 공급자 의존성이 없다.
[새 기반 계약과 이전 방법](foundation-architecture.md)을 따른다.
Darwin의 FIFO 쓰기는 디스크립터별 `F_SETNOSIGPIPE`를 적용한다. 프리뷰 생성 등
다른 네이티브 워커가 있는 프로세스에서도 연결 종료를 오류로 반환하며, 호스트의
프로세스 전역 신호 처리기를 바꾸지 않는다. 기존 스레드의 pending 신호는 보존한다.

PTY 동시 접근 테스트는 관찰자의 `size()`와 워커의 `wait()`가 동시에 진입할 수 있음을 반영한다. 워커는 대기 진입 전 busy만 제한 시간 안에서 재시도하며, 실제 대기 중 size/close가 busy를 반환하는지 확인한다. 취소와 join을 마친 뒤 결과를 판정하므로 실패 경로에서도 워커가 테스트 지역 변수보다 오래 살아남지 않는다.

iiSDKCore의 `ii::io`는 OS/드라이버가 공개하는 사용자 공간 I/O를 공통 C++ API로 다룬다. 현재 실제 백엔드는 저장장치의 일반 파일과 raw 8N1 시리얼 바이트 채널이다. 장치 드라이버 설치, 커널 접근 권한, USB/HID/GPIO/I2C/SPI, 네트워크 소켓, GPU·오디오·카메라 API는 이 변경에 포함하지 않는다. 해당 기능을 지원한다고 가정하는 범용 장치 이름이나 가짜 성공 응답을 제공하지 않는다.

Channel은 OS 자원의 RAII 참조이고 상속을 요구하지 않는다. AsyncIO는 Channel과 Runtime의 SerialQueue를 조합하여 쿼리를 처리한다. 의존 방향은 AsyncIO → Channel 및 Runtime이며 Channel은 Runtime이나 제품 객체를 참조하지 않는다. 플랫폼 구현은 Channel.cpp 내부에 있고 public API에는 HANDLE, fd, termios가 노출되지 않는다.

## 기본 사용

```cpp
#include <iiSDKCore/IO/AsyncIO.h>
#include <iiFileProviderCoreIO.h>
using namespace ii::io;

auto opened = iiFileProvider::core::open_file("data.bin", {.access=Access::read});
if (!opened) { /* opened.error(): 범주와 native 오류 */ }
else {
    ii::runtime::Runtime io_workers({.workers=2, .capacity=64});
    AsyncIO file(io_workers, *opened);
    Buffer buffer(4096);
    auto accepted = file.queries().post(Read{buffer},
        [](ii::runtime::QueryReply<IOResult> reply) {
            if (reply && *reply) {
                const auto& result = **reply;
                auto bytes = result.buffer.bytes().first(result.transfer.bytes);
                // bytes에는 이번에 읽은 부분만 있다. eof는 별도 상태이다.
            }
        });
    // 콘솔 예제의 종료 동기화. 실제 호스트는 다른 이벤트를 계속 처리한다.
    auto completed = io_workers.wait_idle();
    auto joined = io_workers.join();
}
```

AsyncIO::queries()는 QueryPort<IOQuery, IOResult>이다. Read, Write, Seek, Flush, Close 요청을 해당 객체의 큐에서 순서대로 실행한다. 다른 AsyncIO 인스턴스는 병렬로 실행할 수 있다. 같은 Channel에 여러 AsyncIO를 만들면 큐 사이의 순서는 보장하지 않으며 겹친 직접 작업은 busy일 수 있다. 동일 채널은 하나의 AsyncIO 실행 영역에서 관리하는 것을 기본으로 한다.

AsyncIO에서 대기하는 I/O는 워커를 차지한다. CPU·프레임 작업과 격리할 필요가 있으면 별도 Runtime 인스턴스를 I/O 워커 집합으로 사용한다. 0.5.0부터는 [EventLoop의 I/O 경로](loop-io.md)가 여러 채널의 준비 상태를 같은 대기 집합에서 감지하고 read/write 완료를 직접 전달한다. AsyncIO는 별도 워커에서 동기 호출을 실행하는 경로로 유지하며 IOCP/io_uring은 사용하지 않는다. open_file/open_serial/open_device와 직접 Channel 호출은 동기 함수이므로 느릴 수 있는 열기 작업도 Runtime 작업 안에서 실행한다.

## 파일과 시리얼

open_file은 Access(read/write/read_write)와 Creation(existing/open_or_create/create_new/truncate)을 받는다. 기본은 기존 파일 읽기이며 생성·초기화에는 쓰기 권한을 명시해야 한다. create_new는 이미 존재하면 실패한다. truncate는 명시적인 내용 초기화이다. POSIX 새 파일 권한은 0600이며 프로세스 umask가 적용된다. 경로는 std::filesystem::path로 받고 빈 경로와 내장 NUL을 거절한다. Windows는 wide 경로로 연다.

일반 파일에 read/write/seek/size/flush를 제공한다. 한 번의 읽기/쓰기는 부분 전송일 수 있다. Transfer.bytes를 확인하여 상위 프로토콜이 후속 작업을 결정한다. 반환된 바이트가 있으면 eof=false이고, 이후 비어 있지 않은 읽기에서 0바이트가 반환될 때 eof=true이다. 길이 0 읽기는 eof가 아니다. EOF는 오류가 아니며 현재 파일 끝 이후 파일이 늘어나면 다시 읽을 수 있다.

flush는 파일의 fsync/FlushFileBuffers를 호출한다. 파일시스템·장치·가상 호스트가 제공하는 범위를 넘어 전원 장애 내구성을 보장하지 않는다. 시리얼에서는 끝나지 않는 출력 drain을 암묵적으로 수행하지 않고 flush/seek/size를 unsupported로 반환한다.

open_serial은 명시한 경로만 열고 raw binary 8N1, flow control 없음으로 설정한다. 기본 115200 baud이며 9600/19200/38400/57600/115200을 지원한다. 그 외 속도는 invalid_argument이다. Windows COM10 이상은 장치 경로 `\\.\COM10`을 사용한다. 열기는 통신 설정을 변경하며 close 시 이전 설정을 복원하지 않는다. 이 API의 플랫폼 지원 여부는 장치 존재나 접근 권한을 보장하지 않는다.

## 준비 대기·취소·오류

Channel::wait(Interest, timeout, CancellationToken)는 읽기/쓰기 준비를 기다린다. timeout=0은 probe이며 준비되지 않으면 would_block, 양수 대기의 만료는 timed_out이다. 음수는 invalid_argument이다. 일반 파일은 즉시 준비된 것으로 처리한다. 준비 완료가 후속 I/O의 전체 길이 성공을 보장하지는 않는다.

Windows의 현재 비동기 미지원 파이프·시리얼 핸들은 이벤트를 제공하지 않으므로 준비되지 않은 양수 대기는 `unsupported`를 반환한다. 호출자가 반복 확인을 명시적으로 선택한 경우에만 `wait_polling(interest, timeout, interval, stop)`을 사용한다. interval은 양수여야 한다. POSIX에서는 같은 요청을 네이티브 준비 이벤트로 처리한다.

`Read`·`Write`와 `IOWatchOptions`의 공개 데이터 구성이 바뀌었으므로 이 버전을 사용하는 C++ 소비자는 Core와 함께 다시 빌드해야 한다. 기존에 컴파일된 바이너리와 구조체 ABI가 같다고 가정하지 않는다.

POSIX 시리얼은 nonblocking read/write와 poll을 사용하며 취소 신호는 대기 전용 파이프로 전달한다. 준비·취소·기한까지 주기적 재검사 없이 기다린다. Windows는 즉시 반환하는 시리얼 읽기 설정과 통신 큐 상태 조회를 사용하고, 쓰기에는 드라이버 타임아웃 50ms를 설정한다. 호출자가 선택한 polling interval은 OS 스케줄링 지연까지 상한으로 보장하지 않는다. Windows 쓰기는 드라이버 동작에 영향을 받으며 완전한 nonblocking 전송 계약이 아니다.

CancellationSource는 공유 취소 상태를 소유하며 token()으로 요청에 전달하고 cancel()로 취소한다. CancellationToken::native_token()은 같은 상태의 C++23 std::stop_token을 제공한다. 토큰 복사·검사는 할당하지 않는다. 기본 토큰은 취소되지 않고, 취소된 상태는 재설정하지 않는다. 새 작업 묶음에는 새 소스를 만든다. WASM에서도 표준 stop_token을 제공하는 도구 체인과 프로젝트의 C++ 라이브러리 설정이 필요하다.

Read/Write.timeout은 **워커에서 시작한 준비 대기 시간**이다. 큐 대기 시간, 이미 시작한 파일 시스템 호출, 실제 Windows 쓰기 시간을 포함한 전체 작업 deadline이 아니다. CancellationToken은 준비 대기와 전송 직전까지 취소를 검사한다. 이미 시작된 OS 전송을 강제로 중단하거나 그 부작용을 되돌리지 않는다. 실행 대기 중 요청은 기존 Runtime::cancel(token)로 취소할 수도 있으며 이 경우 completion은 호출하지 않는다. CancellationToken 취소는 IOResult의 cancelled로 완료된다.

Error는 invalid_argument, not_found, permission_denied, already_exists, closed, unsupported, would_block, timed_out, cancelled, busy, system_error와 원래 std::error_code를 보존한다. OS가 부분 전송 후 오류를 반환할 수 있으므로 오류가 언제나 부작용 없음을 뜻하지는 않는다. QueryReply의 오류는 쿼리 수명·처리기 예외이며 그 안의 IOResult는 I/O 결과이다. 접수 성공도 I/O 완료 성공과 구분한다.

## 소유권과 재사용

Channel 복사는 같은 OS 자원·커서를 공유하는 소유 참조이다. 마지막 참조가 소멸하면 자원을 닫는다. 명시적인 close는 모든 별칭을 닫고 이후 접근을 closed로 만든다. 진행 중인 작업이 있으면 close도 busy를 반환한다. close 실패 시에도 핸들은 재사용하지 않는다. 소멸자는 닫기 실패를 보고할 수 없으므로 오류 확인이 필요하면 close를 명시적으로 호출한다.

Buffer는 한 번 확보한 고정 크기 바이트 배열의 소유 참조이다. 요청과 응답이 같은 저장소를 유지하므로 호출자의 핸들이 사라져도 대기 요청의 버퍼는 유지된다. offset/length는 전송 전에 검사하며 생략한 length는 offset 이후 전체 용량이다. 결과의 bytes만 유효한 이번 전송 데이터이고 나머지 공간은 이전 값일 수 있다. 길이 0 전송은 준비 대기를 생략한다.

버퍼 내용은 자동 잠금하지 않는다. 접수 후 completion 전까지 호출자가 읽거나 수정하지 않으며 같은 저장소를 독립 작업에 중복 제출하지 않는다. 같은 FIFO 큐에서 이어지는 작업은 앞선 작업의 완료 이후 같은 버퍼를 재사용할 수 있다. 완료 후 같은 Buffer로 다음 요청을 제출한다. 다른 객체 큐로 결과를 넘겼으면 그 소비가 끝날 때까지 재사용하지 않는다.

AsyncIO::close와 소멸은 해당 큐의 접수를 닫고 대기 요청·버퍼·completion 캡처를 취소하며, 활성 I/O와 completion 및 슬롯 반환을 기다린 뒤 처리기의 Channel 소유 참조를 해제한다. 외부 Channel/Buffer 별칭이 없다면 마지막 소유 참조와 함께 메모리·OS 핸들을 해제한다. 외부 별칭이 남아 있다면 그 소유자는 계속 사용할 수 있다. AsyncIO 제거가 파일 자체를 삭제하거나 외부 별칭의 채널을 강제로 닫지는 않는다. 모든 별칭을 닫는 명령은 IOQuery의 Close 또는 Channel::close이다. 이미 시작한 OS 전송은 선점하지 않으므로 제거도 해당 전송 종료를 기다린다. 동일 큐의 콜백에서 제거하지 않고 외부 실행 영역에서 close한다. [제거 계약](cleanup.md)을 따른다.

shutdown/drain은 신규 후속 제출을 막을 수 있으므로 비동기 체인이 완료된 뒤 join한다. 개별 작은 동기 read/seek와 재사용 Buffer 경로의 추가 operator new 호출을 tests/reuse.cpp에서 측정한다. std::function 캡처·쿼리 생성·플랫폼 내부 할당까지 무할당이라고 주장하지 않는다.

## 플랫폼 범위

| 플랫폼 | 파일 | 시리얼 |
|---|---|---|
| macOS/Linux | 네이티브 POSIX 일반 파일 | termios/poll, OS 접근 권한 필요 |
| Windows | 네이티브 Win32 파일 | COM 장치·통신 설정 |
| Android | 앱이 접근 가능한 NDK 파일 | 접근 가능한 POSIX tty만 해당. USB Host Java 브리지 없음 |
| iOS | 앱이 접근 가능한 POSIX 파일 | unsupported. ExternalAccessory/CoreBluetooth 브리지 없음 |
| WASM | Emscripten 가상 파일시스템 | unsupported. WebSerial/WebUSB 권한·브리지 없음 |

platform_capabilities()는 컴파일된 백엔드 지원 범위이며 Channel::capabilities()는 해당 채널의 기능이다. WASM은 backend=wasm_virtual을 반환한다. 가상 파일 성공을 물리 저장장치 접근으로 간주하지 않는다. 영속 저장은 별도 호스트 마운트/동기화 구성이 필요하다. 브라우저 pthread 구성은 기존 COOP/COEP 계약을 따른다.

구현 근거: [POSIX termios](https://man7.org/linux/man-pages/man3/termios.3.html), [poll](https://man7.org/linux/man-pages/man2/poll.2.html), [Windows COMMTIMEOUTS](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-commtimeouts). 실제 검증 범위는 [검증 기록](validation.md)에 구분한다.

## Native device families (0.6.0)

Since Core 0.12, `<iiDeviceMultimedia/IO/Hardware.h>` and `<iiDeviceMultimedia/IO/Protocols.h>` provide device discovery, USB/HID, BLE, PCM and MIDI in an independent peer module. Core retains Channel/AsyncIO; neither package depends on the other. Device types/errors use ii::device and the application owns its poll() driver. See [migration](device-extraction.md) and the [device contract](../../iiDeviceMultimedia/docs/hardware.md).

## Process pipes (0.15.0)

NativePipe adopts existing native pipe endpoints into Channel without weakening
NativeFile regular-file checks. Process standard pipes use partial nonblocking
transfers, readiness/EOF, shared aliases and the same EventLoop read/write
contract. See [native process ownership](process.md) for stream modes, native
backends, SIGPIPE handling, bounded pumping and owner/tree cancellation.

## Read-only native mappings (0.17)

IO/MemoryMap.h maps a storage-provided file Channel without creating or reopening it by path. MappedRegion owns the OS view, supports bounded slices and explicit paging hints, and can back immutable resource packs. Closing a Channel does not unmap retained views. The file must remain immutable; OS-backed mapping is unsupported on WASM. See [resources.md](resources.md) for lifetime, paging, error and platform contracts.
