# 플랫폼 독립 스레드 관리

`ii::runtime::Thread`는 하나의 실행 스레드를 소유하는 final 클래스이다. 앱은 작업 콜백이나 EventLoop 인스턴스를 연결하며, 상속·OS별 핸들·Qt를 사용하지 않는다. 내부 실행은 표준 `std::thread`, 동기화는 mutex/condition_variable을 사용한다. Core의 다중 생산자·소비자 테스트와 쿼리 실행 테스트도 이 클래스로 스레드를 관리한다.

협력적 중단, 대기 깨우기, 실행 상태, 작업 예외의 값 반환, 소멸 시 종료 대기, EventLoop 중단 연결, WASM 호스트의 지원 여부를 같은 API로 다룬다. 가상 머신이나 별도의 스케줄러를 만들지 않는다.

## 기본 사용

```cpp
#include <iiSDKCore/Runtime/Thread.h>
using namespace ii::runtime;
using namespace std::chrono_literals;

Thread worker;
auto started = worker.start([](ThreadContext& context) {
    while (!context.stop_requested()) {
        // 유한한 작업 단위를 처리한다.
        if (context.wait_for(10ms)) break;
    }
});
if (!started) { /* started.error()를 처리한다. */ }
worker.request_stop();
auto joined = worker.join();
if (!joined) { /* joined.error()와 cause를 처리한다. */ }
```

ThreadContext는 작업 실행 중에만 빌리는 참조이다. 복사하거나 작업 반환 이후에 보관하지 않는다. stop_requested()는 중단 요청 여부, wait_for(duration)는 중단으로 깨어났는지 여부를 반환한다. false는 대기 시간 경과이다. 대기 전에 중단 요청이 도착해도 놓치지 않는다. 임의 I/O·다른 조건 변수 대기·긴 계산을 강제로 중단하지는 않는다. 그러한 작업은 직접 중단을 확인하거나 해당 I/O의 취소 기능을 조합한다.

## 이벤트 루프와 쿼리

```cpp
EventLoop loop(128); // worker보다 먼저 생성하여 더 오래 살게 한다.
Thread worker;
auto started = worker.start(loop);
// 다른 객체가 QueryPort::post(loop, ...) 또는 loop.post(...)로 전달한다.
worker.request_stop(); // 루프의 idle/suspended 대기도 깨운다.
auto joined = worker.join();
```

작업 콜백에서 단순히 loop.run()을 호출하는 경우에는 자동 중단 연결이 없다. 루프를 관리할 목적이면 start(loop, budget)를 사용한다. 이 형태의 작업에서 예외가 발생하면 루프를 stopped로 바꾸고 join()에 원래 예외를 전달한다. 기존 직접 EventLoop::run()의 예외 전파 계약은 유지한다.

참조하는 EventLoop는 실행과 종료 대기가 끝날 때까지 살아 있어야 한다. 자동 중단 연결이 ObjectProperties 등의 사용자 객체에 잠금이나 스레드 소속을 부여하지는 않는다. 상태 변경 쿼리를 하나의 루프에 제출하여 직렬 처리한다. 플랫폼 UI 호출의 메인 스레드 요구도 그대로 유지한다.

실제 예제 `examples/thread.cpp`는 쿼리 입력 21을 별도 관리 스레드에서 처리하고, 응답 42와 호출자와 다른 실행 스레드를 검증한다. `./build/iiSDKCoreThreadExample`로 실행한다.

## 상태와 재사용

| 상태 | 의미 |
|---|---|
| idle | 아직 시작한 실행이 없다. |
| running | 작업이 실행 가능하거나 실행 중이다. |
| stop_requested | 협력적 중단을 요청했다. 작업은 아직 반환하지 않았을 수 있다. |
| completed | 작업과 캡처 정리가 정상 완료되었다. |
| failed | 작업이 예외로 끝났으며 예외를 보존하고 있다. |

상태와 joinable 여부는 별도이다. completed/failed도 아직 join하지 않았다면 자원을 회수해야 한다. 이때 새 start는 busy이다. join 완료 후 같은 Thread 인스턴스를 새 작업에 재사용할 수 있다. 새 실행은 중단·실패 상태를 초기화하고 제어 블록은 재사용한다. 종료한 EventLoop는 terminal 상태이므로 새 작업 루프가 필요하면 새 EventLoop를 연결한다.

join()은 중단을 요청하지 않고 완료를 기다린다. idle에서 호출하면 성공하며, 완료 후 재호출하면 같은 작업 결과를 반환한다. 작업 실패를 소비하거나 지우지 않는다. id()는 가장 최근에 시작한 실행의 표준 thread id이고 current_id()는 호출 스레드의 id이다. 초기 id는 비어 있다. OS는 종료한 스레드 ID를 재사용할 수 있으므로 장기 객체 식별자로 사용하지 않는다.

start, request_stop, join, 상태 조회는 내부 동기화한다. 동시에 시작하거나 이미 진행 중인 join을 다시 요청하면 busy를 반환할 수 있다. 작업 자신이 join하면 self_join이다. 다른 호출자가 이미 join 중이어도 자기 대기를 거절한다. 다른 스레드가 join하는 동안 중단 요청과 상태 조회를 할 수 있다. request_stop은 호출 시 관측한 실행을 대상으로 하며, 미래 실행에 대한 예약 취소가 아니다.

request_stop()은 이번 실행에 새 중단 요청을 기록한 경우만 true이다. idle, 이미 요청한 실행, 완료한 실행에는 false이다. 실제 종료 여부는 join이나 상태로 확인한다.

## 실패와 소멸

ThreadResult는 expected<void, ThreadError>이다.

- unsupported: 현재 빌드/호스트가 실제 워커를 지원하지 않는다. 호출 스레드에서 대신 실행하지 않는다.
- invalid_work: 작업 콜백이 비어 있다.
- busy: 이전 실행을 아직 join하지 않았거나 다른 join이 진행 중이다.
- self_join: 실행 스레드가 자신을 기다리려 한다.
- would_block: 브라우저 메인 스레드에서 소유 스레드 시작/종료 대기를 시도했다.
- start_failed / join_failed: 실행 생성/종료 대기 실패이다. cause에 원래 예외를 보존한다.
- work_failed: 작업 예외이다. cause에 원래 예외를 보존한다.

Thread 생성과 사용자 콜백 구성 과정의 할당 예외까지 모두 오류 값으로 바꾸지는 않는다. 시작 거절/실패는 마지막 실행의 상태와 실패 기록을 유지한다.

소멸자는 중단을 요청하고 join한다. 작업 예외를 소멸자 밖으로 던지지 않으므로 결과가 필요한 호출자는 명시적으로 join한다. 협력하지 않는 작업은 소멸도 지연시킬 수 있다. 실행 중인 자신의 Thread를 파괴하거나 다른 호출자가 객체를 사용하는 중에 파괴해서는 안 된다. 정상적인 종료 대기를 수행할 수 없는 소멸은 terminate이며 detach하여 수명 문제를 숨기지 않는다. 사용자 참조 캡처와 해당 자원의 수명도 호출자가 보장한다.

Thread는 개별 스레드 수명의 기반 클래스이며, 애플리케이션 기본 실행에는 [Runtime의 재사용 워커 집합](parallel-runtime.md)을 사용한다. 강제 종료, 스레드 일시 정지, CPU affinity, detach는 제공하지 않는다. 실행/중단/재개 가능한 이벤트 흐름은 EventLoop와 조합한다. Thread 제어 블록을 재사용해도 새 OS 스레드 생성 비용은 있으므로 반복 작업은 Runtime에 제출한다.

## WASM

WASM은 pthread가 기본이다. 명시적으로 IISDKCORE_WASM_THREADS=OFF를 지정한 제한 빌드에서만 Thread::supported()가 false이다. pthread 빌드는 다음과 같이 구성한다.

```sh
emcmake cmake -S . -B build/wasm-threads/build \
  -DCMAKE_BUILD_TYPE=Release -DIISDKCORE_WASM_THREADS=ON
cmake --build build/wasm-threads/build --parallel 4
ctest --test-dir build/wasm-threads/build --output-on-failure
python3 tests/serve_wasm.py build/wasm-threads/build --port 8765
# http://127.0.0.1:8765/thread-browser.html
# http://127.0.0.1:8765/iiSDKCoreBrowserTests.html
```

라이브러리는 -pthread를 소비자의 컴파일과 링크에 전파한다. 작업을 생성·join하는 테스트/예제 실행 파일은 PROXY_TO_PTHREAD=1, PTHREAD_POOL_SIZE=16, EXIT_RUNTIME=1, ALLOW_BLOCKING_ON_MAIN_THREAD=0으로 main을 워커에서 실행한다. 이 실행 정책과 풀 크기를 모든 SDK 소비자에 강제하지 않는다. BrowserLoop 예제와 브라우저 호스트 테스트는 메인 스레드에 남는다.

supported()는 호스트의 기능 지원 여부이며 어느 호출 위치에서든 start가 허용된다는 뜻은 아니다. 브라우저 메인 스레드에서는 Thread의 소유/종료 대기 계약이 호스트를 막지 않도록 start와 활성 join을 would_block으로 거절한다. 스레드 관리 코드는 애플리케이션 워커에서 실행한다. EventLoop::run도 pthread 워커에서만 허용하고 브라우저 메인 스레드에서는 예외로 거절한다. 메인 스레드의 프레임 처리는 BrowserLoop/poll을 사용한다.

브라우저 pthread는 SharedArrayBuffer와 적절한 cross-origin isolation 설정이 필요하다. 개발용 serve_wasm.py는 localhost에서 COOP/COEP 헤더를 제공한다. 배포 호스트의 설정, 사용 가능한 워커 수, 작업 시작 정책은 애플리케이션의 책임이다. 근거는 [Emscripten pthread 공식 문서](https://emscripten.org/docs/porting/pthreads.html)이다. WASI 백엔드는 포함하지 않는다.

검증 코드: tests/thread.cpp, tests/runtime.cpp, tests/query.cpp, tests/browser.cpp. 실제 환경별 결과는 docs/validation.md에 기록한다.
