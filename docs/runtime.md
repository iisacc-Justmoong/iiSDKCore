# 런타임 설계와 계약

## 책임과 의존성

애플리케이션의 기본 실행 모델은 [Runtime의 멀티스레드·병렬·비동기 처리](parallel-runtime.md)이다. 객체별 SerialQueue에 연결한 쿼리를 기본으로 사용한다. 이 문서의 EventLoop는 단일 실행 영역의 예약·프레임·호스트 이벤트 의미론을 정의하며, 전체 애플리케이션을 하나의 스레드로 제한하지 않는다.
이번 요구사항에 따라 Core에 최소 실행·중단·재개·종료 의미론을 포함한다. `AGENT.md`의 application lifecycle 범위는 “명확한 필요성이 확인되기 전까지” 제외하는 원칙이므로, 제품 공통 실행 상태만 여기서 제공한다. UIKit/AppKit/Win32/Android의 전체 앱 생명주기를 재구현하지 않는다.

의존 방향은 제품/상위 SDK → EventLoop·Stream → Core/Time·C++ 표준 라이브러리 및 OS이다. [Thread](thread.md)는 작업 스레드 수명을 소유하고 선택적으로 EventLoop를 구동하며, EventLoop는 Thread를 참조하지 않는다. Core/Time은 Runtime을 참조하지 않는다. Emscripten에서는 BrowserLoop와 스레드 호스트 검사가 브라우저 C ABI를 사용한다. Core는 상위 SDK와 Qt를 참조하지 않으며, 하드웨어 계층은 공식 네이티브 장치·미디어 API를 사용한다.

객체 간 통신은 [QueryPort](query.md)로 표현한다. QueryPort의 비동기 전달이 EventLoop의 실행 단위로 내려가며, EventLoop가 수신 객체나 쿼리 타입에 의존하지 않는다.

생성 이후에도 변경할 멤버 스키마는 [ObjectProperties](object-properties.md)를 인스턴스로 소유한다. 이 계층이 PropertyQuery를 처리하며, QueryPort와 EventLoop는 동적 속성 구현에 의존하지 않는다. 동적 설계·스냅샷의 할당 비용은 아래 고정 큐/버퍼 재사용 보장과 구분한다.

공개 클래스는 `final` 인스턴스이다. 가상 함수, 사용자 상속 계층, 전역 앱 싱글턴은 없다. 객체를 직접 소유하고 참조로 연결한다. 객체는 복사하거나 이동하지 않아 참조의 주소를 안정적으로 유지한다.

## 실행·수명·동시성

- 제출, 취소, 상태 변경, 통계 조회는 다중 스레드에서 호출할 수 있다.
- `poll` 또는 `run`은 한 번에 하나만 실행할 수 있다. 재귀 호출과 동시 호출은 `std::logic_error`이다. 다른 스레드에서 순차 호출은 허용하므로 GUI 스레드 affinity는 호스트가 유지한다.
- 콜백은 실행자를 호출한 스레드에서 직렬 실행된다. 내부 큐 잠금 없이 호출하므로 새 작업 제출이나 타이머 취소가 가능하다.
- 객체 파괴 전에 생산자와 실행 스레드를 종료/join하고 브라우저 드라이버를 정지해야 한다. 콜백 실행 중 자기 루프/드라이버를 파괴하지 않으며, 객체 소멸 중 그 객체로 재진입하지 않는다.
- 참조 캡처는 실행·취소 및 콜백 해제까지 살아 있어야 한다. 콜백은 복사 가능한 `std::function<void()>`이다. 소유권이 필요한 payload는 값으로 캡처한다.
- 큐의 콜백 캡처를 폐기할 때 잠금을 잡지 않는다. `std::function` 이동 후 원본이 비었다고 가정하지 않으며, 빈 함수와 `swap`해 슬롯에 캡처가 잔류하지 않도록 한다.

## 예약, 공정성, 예산

0.7.0의 [시간 기반 런타임](realtime.md)은 주기·상대 마감·주기별 실행량·quantum으로 시간 작업을 예약한다. 준비된 예약은 절대 마감 순서로 실행하고, 동일 마감에서는 continuation을 순환시킨다. 아래의 기존 우선순위 정책은 실행 가능한 시간 예약이 없을 때 적용한다. TimeSlice의 종료 검사와 yield는 협력적 실행이며, 임의 콜백의 강제 선점은 아니다. LiveStream은 같은 예약 경로로 재생 시각과 지연 한도를 적용한다.

`post`는 즉시 작업, `at`은 단조 시계 절대 시각, `after`는 상대 지연, `every`는 양수 주기이다. 과거 deadline과 0 이하 지연은 즉시 실행 가능하다. 비어 있는 콜백, 잘못된 우선순위, 0 이하 주기는 `SubmitError::invalid`이다. 표현 범위를 넘는 미래 시각은 최대 시각으로 포화시킨다.

세 등급이 모두 준비되면 high:normal:background를 4:2:1로 서비스한다. 준비되지 않은 등급은 건너뛰고, 같은 등급 안에서는 제출 순서를 보존한다. 높은 우선순위 작업이 계속 들어와도 background가 굶지 않는다. 실행 중 새로 제출된 작업도 남은 예산 내에서 처리할 수 있다.

`PumpBudget`의 콜백 수 또는 시간 한도에 도달하면 호스트로 반환한다. `poll`의 0 예산은 아무것도 실행하지 않으며 음수 시간은 예외이다. `run`과 `BrowserLoop`의 0 예산은 무한 공회전을 막기 위해 예외이다. 콜백을 선점하지 않으므로 단일 콜백과 큐 검색 시간이 예산을 넘을 수 있다. 긴 계산과 blocking I/O는 외부 작업 스레드에서 수행하고 완료를 `post`한다. 오디오 하드 리얼타임 콜백용 무잠금 런타임이 아니다.

주기 작업은 최초 위상을 유지하고 지연된 횟수를 `missed_ticks`로 기록해 건너뛴다. 과거 프레임을 한꺼번에 실행하지 않는다. 물리 시뮬레이션 고정 시간 적분이나 미디어 타임스탬프 동기화는 도메인 로직으로 구현한다.

현재 scheduler는 고정 슬롯을 순회한다. 작업 선택당 O(capacity), 저장 공간 O(capacity)이다. 소규모 코어의 명확한 의미론을 우선했으며, 타이머 수가 커질 때 heap/wheel 최적화는 실측 후 내부 구현을 교체할 수 있는 경계이다.

## 취소, 예외, 종료

토큰은 루프 식별자·슬롯·세대를 가진 불투명 값이다. 다른 루프의 토큰과 재사용 전 슬롯 토큰은 무효이다. token.cancel()/pending()은 실행자 제거 후 안전하게 false를 반환한다. 예약된 작업 취소는 내부 잠금 밖에서 캡처를 파괴하고, 파괴 완료 후 슬롯을 해제한다. 이미 선택되어 실행 중인 콜백은 선점할 수 없으며 취소 시 다음 주기만 막는다. 실행과 캡처 파괴 중 슬롯도 capacity에 포함하므로 capacity=1인 콜백·소멸자가 새 작업을 넣으면 `full`일 수 있다.

콜백 예외는 해당 작업을 해제한 뒤 `poll`/`run` 호출자에게 전달된다. 실패한 주기 작업은 재예약하지 않는다. 다른 작업은 유지되며 호출자가 재실행 또는 종료를 결정한다. BrowserLoop는 C 콜백 경계를 넘어 예외를 던지지 않고 예외를 저장하고 루프를 종료한다. 호스트는 `rethrow_if_failed()`로 확인한다.

상태는 `ready → suspended → ready` 또는 `ready/suspended → stopped`이다. 중단은 큐를 보존하고 새 제출도 받지만 콜백을 실행하지 않는다. 진행 중 콜백은 끝까지 실행된다. 중단 중 시간은 계속 흐르므로 재개 시 만료된 주기 작업은 한 번 실행하고 놓친 tick을 건너뛴다.

`request_stop()`은 최종 상태이다. 새 작업을 거절하고 대기 중 실행자를 깨우며, 이후 dispatch를 막는다. 미실행 작업과 타이머의 캡처를 즉시 취소·파괴하고 슬롯을 반환한다. 진행 중 콜백은 끝까지 실행되고 이후 캡처와 슬롯을 해제한다. 외부 실행 드라이버를 정지·join한 뒤 EventLoop를 파괴한다. 종료 후 재시작은 새 EventLoop 인스턴스로 한다. 객체별 작업만 제거할 때는 [TaskScope](cleanup.md)를 사용한다.

## 메모리 재사용과 스트림

생성 시 EventLoop의 슬롯·free-list, Stream의 순환 버퍼 용량을 확보한다. 실행/취소/소비 후 같은 저장 공간을 재사용하고 자동 증설하지 않는다. 포화는 `SubmitError::full` 또는 `try_push`의 `false`로 반환한다. 호출자가 폐기·재시도·생산 속도 조정을 결정한다.

`Stream<T>`는 mutex로 보호한 다중 생산자·다중 소비자 FIFO이다. `reject`는 가득 차면 새 값을 거절하고, `drop_oldest`는 가장 오래된 값을 버려 최신 샘플을 유지한다. 폐기/거절 횟수를 조회할 수 있다. `close` 후 생산은 거절하지만 남은 데이터는 소비한다. `drained`는 닫혔고 비어 있음을 뜻한다. 생산 함수는 값 인자를 받으므로 실패하더라도 전달한 값이 소비될 수 있다. 재시도할 payload는 호출자가 보존한다.

T는 noexcept 이동 생성·이동 대입이 필요하다. T의 이동/소멸은 버퍼 잠금 중 실행될 수 있으므로 같은 Stream으로 재진입하지 않아야 한다. `std::array`로 고정 크기 프레임을 보관하거나, 수명을 외부에서 보장할 때 `std::reference_wrapper`로 이미 할당한 데이터를 전달할 수 있다.

큐/버퍼 재사용과 전체 애플리케이션 무할당은 다른 보장이다. 콜백 캡처와 T 내부 메모리, 브라우저 및 OS는 별도이다. `tests/reuse.cpp`는 준비된 루프와 int 스트림을 1만 회 사용해 일반 `operator new` 추가 호출이 0인지 검증한다. malloc 전체나 임의 payload의 무할당을 검사하는 벤치마크는 아니다.

## 호스트와 플랫폼

독립 네이티브 프로세스는 `run()`을 사용한다. 조건 변수는 가장 이른 deadline까지 대기하고 제출·취소·상태 변경에 깨어난다. iOS·Android 및 OS 이벤트 루프를 소유한 GUI에서는 프레임/타이머/이벤트 처리 지점에서 `poll()`한다. `next_deadline()`은 다음 wake 시각을 정하는 참고 스냅샷이며 다른 스레드의 제출로 바뀔 수 있다. 외부 루프의 wake 연결은 호스트가 제공해야 한다. 이 버전은 CFRunLoop/Win32 메시지/ALooper/epoll 등록기를 포함하지 않는다.

BrowserLoop는 requestAnimationFrame으로 `poll()`을 호출한다. 객체는 main 반환 후에도 살아 있어야 하며 브라우저 메인 스레드에서 start/stop/조회한다. 여러 루프를 독립적으로 연결할 수 있다. 브라우저가 백그라운드 탭의 프레임을 제한하면 실행도 제한된다. 지속 백그라운드 실행은 보장하지 않는다. Node 테스트는 공통 poll 계약을, 실제 브라우저 예제는 프레임 드라이버를 검증한다.

공식 플랫폼 근거:

- [Emscripten runtime environment](https://emscripten.org/docs/porting/emscripten-runtime-environment.html): 브라우저에 제어권을 돌려주는 반복 실행.
- [Emscripten HTML5 API](https://emscripten.org/docs/api_reference/html5.h.html): animation frame 등록과 취소.
- [Apple CFRunLoopWakeUp](https://developer.apple.com/documentation/corefoundation/cfrunloopwakeup(_:)): native host의 source와 wake 관리 경계.
- [Android NDK Looper](https://developer.android.com/ndk/reference/group/looper): Activity 호스트의 native event polling 경계.

## 교차 빌드

각 구성이 독립적인 `build/` 디렉터리를 갖도록 루트 `build/` 아래에 둔다.

```sh
# 브라우저 / Node
emcmake cmake -S . -B build/wasm/build -DCMAKE_BUILD_TYPE=Release
cmake --build build/wasm/build --parallel 4
ctest --test-dir build/wasm/build --output-on-failure
python3 -m http.server 8765 --bind 127.0.0.1 --directory build/wasm/build
# http://127.0.0.1:8765/iiSDKCoreLive.html
# http://127.0.0.1:8765/iiSDKCoreBrowserTests.html

# iOS 기기 라이브러리
cmake -S . -B build/ios-arm64/build -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DBUILD_TESTING=OFF \
  -DIISDKCORE_BUILD_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/ios-arm64/build --parallel 4

# Android. ANDROID_NDK는 설치된 NDK 디렉터리이다.
cmake -S . -B build/android-arm64/build \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/android-arm64/build --parallel 4
```

WASM은 Emscripten 대상이며 IISDKCORE_WASM_THREADS 기본값은 ON이다. pthread 워커에서는 Runtime, Thread와 EventLoop::run을 사용할 수 있으며 브라우저 메인 스레드에서는 BrowserLoop/poll을 사용한다. 단일 스레드 제한 구성은 명시적으로 OFF를 지정한다. 빌드·호스트 설정은 [스레드 계약](thread.md), 실제 실행 결과는 [검증 기록](validation.md)에 정의한다. WASI는 미지원이며 iOS·Android Core 빌드는 앱 스토어 배포 또는 물리 장치 검증과 동일하지 않다.
