# 이벤트 루프 I/O

iiSDKCore 0.5.0의 EventLoop는 작업·타이머와 함께 Channel의 읽기/쓰기 준비 상태를 감시하고 바이트 전송을 실행한다. 채널별 대기 워커나 외부 Qt 이벤트 시스템이 필요하지 않다. 0.5.0의 채널 API에서 장치 프로토콜과 패킷 해석은 소비자가 담당하고, Core는 준비 감지·전송·완료 전달·수명 정리를 담당한다.

## 공개 API

EventLoop::watch(channel, interest, ready, options)는 읽기 또는 쓰기 준비를 한 번 감지하여 IOReady 콜백에 io::Result<void>를 전달한다. options.timeout을 생략하면 준비 상태 또는 취소를 기다린다. 준비 완료가 읽을 데이터의 전체 길이나 이후 전송 성공을 보장하지 않으므로, 콜백에서 실제 전송 결과를 검사한다. 반복 입력은 완료 시 다음 watch/read를 명시적으로 제출한다.

EventLoop::read(channel, Read, completion, priority)와 write(channel, Write, completion, priority)는 준비 상태를 감지하고 해당 바이트 전송을 직접 실행하여 IOResult를 전달한다. IOResult에는 요청 Buffer와 이번 전송의 바이트 수, 읽기의 EOF 상태가 포함된다. Buffer·Read·Write·IOResponse는 Runtime에 의존하지 않는 IO/Requests.h에 정의하며 기존 AsyncIO.h 포함 방식도 유지한다.

```cpp
#include <iiSDKCore/Runtime/EventLoop.h>
using namespace std::chrono_literals;

ii::runtime::EventLoop loop(64);
auto sensor = ii::io::open_serial(device_path).value();
ii::io::Buffer input(256);
auto reading = loop.read(sensor, ii::io::Read{input, 0, 256, 1s},
    [&](ii::io::IOResult result) {
        if (!result) { /* result.error()를 처리한다. */ return; }
        auto bytes = result->buffer.bytes().first(result->transfer.bytes);
        // bytes를 해석하고, 필요한 값만 다른 객체의 QueryPort에 post한다.
    });
// 네이티브 전용 실행 스레드에서는 loop.run(), 호스트 구동에서는 loop.poll().
```

Read/Write.timeout은 이 경로에서는 **접수 시점부터 준비 상태를 기다리는 시간**이다. 0은 probe이며 데이터가 없으면 would_block으로 완료한다. 양수 대기의 만료는 timed_out이다. watch의 생략한 timeout은 무기한, 명시적인 0은 probe이다. std::chrono의 큰 timeout은 시계 최댓값으로 포화시키며 음수는 접수 전에 invalid로 거절한다. 준비 완료를 감지한 뒤 실제 전송에서 would_block/busy를 만났으면 양수 대기 요청은 같은 슬롯·기한 안에서 다시 준비 상태를 기다린다.

길이 0 요청은 준비 대기를 생략하지만 채널의 닫힘과 접근 권한을 검사한다. 잘못된 버퍼 범위·빈 콜백·잘못된 우선순위는 SubmitError::invalid이다. 유효한 요청의 채널 닫힘·권한·장치 오류는 IOResult 또는 IOReady의 오류로 전달한다. 실제 전송은 한 번의 read/write이므로 부분 전송을 허용한다. 전체 프레임·패킷의 전송이 필요하면 bytes를 기준으로 나머지 요청을 제출한다.

## 실행과 예산

watch/read/write는 같은 EventLoop의 고정 작업 슬롯을 사용한다. 일반 작업·타이머와 합쳐 capacity를 초과하면 full이며, 별도의 무제한 장치 큐를 생성하지 않는다. 우선순위의 4:2:1 선택 규칙, callback/time budget, suspend/resume과 stopped 접수 거절을 공유한다. 완료는 해당 루프의 dispatch 스레드에서 호출하며 한 요청은 한 번만 완료한다. 콜백 예외는 슬롯과 캡처를 정리한 뒤 poll/run의 호출자에게 전달한다.

poll은 준비 상태를 0 timeout으로 검사하며 잠드는 커널 대기를 수행하지 않는다. UIKit·Android·GUI·WASM 호스트가 poll을 호출할 때 감지가 이루어진다. next_deadline은 타이머와 I/O의 기한·재시도 시각을 제공하며, 무기한 감시의 향후 장치 이벤트 시각을 예측하지 않는다. run은 네이티브 또는 지원되는 WASM 워커에서 대기를 구동하며 브라우저 메인 스레드 제한을 유지한다.

네이티브 POSIX에서는 모든 대기 채널과 비차단 wake pipe를 같은 poll 집합으로 기다린다. 작업 제출·TaskToken 취소·suspend/resume·stop은 이 파이프를 통해 커널 대기를 깨운다. 채널 핸들은 대기 동안 잠금과 소유 참조로 고정하여 close/FD 재사용과의 경쟁을 막는다. 취소 확인과 busy 재시도를 위해 한 대기 구간은 최대 20ms로 제한하며 OS 스케줄링 지연의 상한을 보장하지 않는다. 실제로 대기 집합을 준비한 횟수는 LoopStats.io_waits로 관찰할 수 있다.

Windows는 기존 COM 큐 상태와 파일 준비 검사에 네이티브 wake event를 결합하며 20ms 구간으로 COM 상태를 재검사한다. IOCP/overlapped COM 구현은 아니다. WASM은 가상 파일 채널을 호스트 또는 워커에서 처리하며, 물리 장치 접근을 모사하지 않는다.

## 장치 경계

open_serial은 기존 raw 8N1 시리얼 설정을 사용한다. USB 시리얼도 OS가 노출한 tty/COM 경로가 있어야 한다. open_device(path, DeviceOptions)는 POSIX의 기존 문자 장치 또는 FIFO를 O_NONBLOCK로 연다. 생성·truncate·프로토콜 설정을 수행하지 않고 일반 파일·디렉터리·블록 장치는 unsupported로 거절한다. 센서·Linux input/evdev·hidraw 등은 드라이버가 노출한 문자 장치의 바이트 경로를 사용할 수 있으며, 장치별 제어 명령이나 보고서 해석은 별도이다. FIFO의 끊어진 출력은 SIGPIPE로 프로세스를 종료하지 않고 EPIPE/closed를 반환하며 호스트의 신호 처리기와 기존 pending 신호를 보존한다.

일반 파일은 항상 준비된 것으로 처리한다. 저장장치 페이지 로드·파일시스템 지연과 O_NONBLOCK를 무시하는 장치 드라이버 호출은 루프를 지연시킬 수 있다. 기존 AsyncIO는 느린 동기 파일/드라이버 작업을 별도 Runtime 워커에 격리하는 경로로 유지한다. 예산은 이미 시작한 OS 호출이나 사용자 콜백을 선점하지 않으며 Windows 시리얼 쓰기는 기존 드라이버 타임아웃 계약을 유지한다.

| 플랫폼 | 이 변경의 채널 경로 |
|---|---|
| macOS/Linux | POSIX 파일·시리얼·문자 장치·FIFO, poll+wake pipe |
| Android | 앱이 접근 가능한 POSIX 파일·tty·문자 장치·FIFO; USB Host 브리지 없음 |
| iOS | 앱이 접근 가능한 POSIX 파일·문자 장치·FIFO; 시리얼 열기와 ExternalAccessory/CoreBluetooth 브리지 없음 |
| Windows | 기존 파일·COM, 상태 재검사+wake event; open_device는 unsupported |
| WASM | 가상 파일; open_serial/open_device는 unsupported |

이 문서의 0.5.0 채널 기능은 열어 둔 채널의 준비 상태를 감지한다. USB/HID 장치 목록 열거·hotplug 서비스·Bluetooth·오디오·MIDI·소켓 연결 또는 장치별 ioctl API를 추가한 것은 아니다. OS 접근 권한과 드라이버 지원은 그대로 적용된다. 0.6.0에 추가한 Hardware·Protocols API는 0.12.0부터 독립 동위 모듈 iiDeviceMultimedia로 이동했다. Core는 그 모듈 없이 일반 채널 I/O를 제공하며, 장치 모듈 0.2 역시 Core 없이 동작한다. 두 모듈의 연결은 앱의 poll() 드라이버가 소유한다. 현재 장치 지원 범위는 [새 모듈 계약](../../iiDeviceMultimedia/docs/hardware.md)을 따른다.

## 제거와 소유권

각 요청은 Channel·Buffer와 콜백 캡처의 소유 참조를 유지한다. 전송 완료 전에는 같은 Buffer를 다른 실행 영역에서 읽거나 수정하지 않는다. 완료를 다른 객체의 큐로 전달하였다면 해당 소비가 끝난 뒤 버퍼를 재사용한다.

대기 요청의 TaskToken::cancel은 완료 전달을 생략하고, 해당 네이티브 대기 구간의 핸들 잠금과 소유 참조까지 해제되는 것을 기다린다. 이후 요청의 Channel·Buffer·콜백을 파괴하고 슬롯을 반환한다. 콜백 함수는 빈 함수와 교환하여 이동 후 상태에 의존하지 않으며 캡처 소멸자의 재진입은 루프 잠금 밖에서 처리한다. 제거 중에는 같은 슬롯을 새 요청에 노출하지 않는다. request_stop과 루프 소멸도 대기 감시를 같은 경로로 정리한다. 이미 실행 중인 전송·완료는 선점하지 않으며 token.pending이 false가 되는 것은 슬롯 정리가 끝난 상태이다.

CancellationToken의 취소는 cancelled 결과를 전달하며, 콜백 없는 TaskToken 취소와 구분한다. 외부 Channel 별칭은 유지한다. 모든 별칭을 닫으려면 Channel::close를 호출한다. 네이티브 대기나 직접 전송이 핸들을 사용하는 순간의 close는 busy일 수 있으므로 등록 토큰을 먼저 취소한 뒤 닫는다. run을 사용하는 Thread는 루프를 소멸시키기 전에 종료·join한다. 이 규칙은 [객체 제거 계약](cleanup.md)을 따른다.

## 검증

tests/loop_io.cpp는 파일·FIFO·PTY의 입력과 출력, typed QueryPort 전달, 준비 완료·EOF·빈 전송·기한·취소·종료·슬롯 재사용과 native wakeup을 검증한다. examples/loop_io.cpp는 실제 파일 입력을 루프에서 읽고 다른 채널로 방출한 뒤 저장된 바이트를 대조한다. 설치 소비자에서도 같은 LoopIO 계약을 실행한다. tests/reuse.cpp는 준비된 Buffer와 루프 슬롯의 반복 재사용을 측정한다.

플랫폼 근거: [poll](https://man7.org/linux/man-pages/man2/poll.2.html), [pipe](https://man7.org/linux/man-pages/man2/pipe.2.html), [Windows ClearCommError](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-clearcommerror), [Windows WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject). 실제 실행·교차 빌드·물리 장치의 증거 범위는 [validation.md](validation.md)에 구분한다.

## 외부 GUI·이벤트 호스트 (0.16)

외부 루프에서 poll만 호출하고 긴 타이머로 잠들면 채널 준비를 통지받을 수 없다. HostLoop의 기본 준비 대기 연결은 이 경계를 해결한다. 하나의 관리형 대기 스레드가 기존 폴러를 사용하고 완료는 소유 호스트의 poll에서 전달한다. 미완료 I/O가 없으면 대기 스레드는 조건 변수에서 잠든다. 대기 중 I/O의 협력적 취소·닫힘 확인에는 기존 최대 20ms 대기 한도가 적용된다. 느린 동기 파일·드라이버 호출은 계속 AsyncIO·Runtime에 격리한다. [호스트 연결 계약](host-loop.md)을 따른다.
