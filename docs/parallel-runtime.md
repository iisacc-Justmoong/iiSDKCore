# 멀티스레드·병렬·비동기 기본 런타임

iiSDKCore의 애플리케이션 기본 실행자는 `Runtime`이다. 생성 시 여러 `Thread` 워커를 시작하고 유지하며, `post()`로 받은 작업을 호출자와 다른 워커에서 비동기로 처리한다. 기본 워커 수는 hardware_concurrency 힌트를 2~4개로 제한한 값이며 옵션으로 지정할 수 있다. 개별 작업마다 OS 스레드를 만들지 않는다.

기본 전제는 멀티스레드 실행, 독립 작업의 병렬 처리, 객체 간 비동기 쿼리이다. 공유 객체의 가변 상태는 객체별 `SerialQueue`에 귀속한다. 서로 다른 객체와 일반 작업은 병렬로 진행하고, 같은 객체의 쿼리는 한 번에 하나씩 처리한다. 큐별로 전용 OS 스레드를 만들거나 모든 객체를 하나의 전역 잠금으로 묶지 않는다.

## 기본 구성

```cpp
#include <iiSDKCore/Runtime/Query.h>
using namespace ii::runtime;

Runtime runtime;                 // 워커를 즉시 시작한다.
SerialQueue object(runtime);     // 객체의 상태 변경 실행 영역이다.
int value = 0;
QueryPort<int, int> update(object, [&](const int& next) {
    value = next;
    return value;
});

auto accepted = update.post(42, [](QueryReply<int> result) {
    // 같은 객체 큐에서 비동기로 완료된다. 결과에는 필요한 값만 있다.
});
// 호출자는 UI·입력·다른 작업을 계속 처리할 수 있다.
auto completed = runtime.wait_idle(); // 콘솔/테스트의 동기화 지점 예시
auto joined = runtime.join();         // 객체·캡처가 소멸하기 전에 종료한다.
```

일반 독립 작업은 `runtime.post(task)`로 제출한다. 공유 가변 상태를 가진 객체는 한 SerialQueue를 소유하고 그 객체의 여러 QueryPort를 같은 큐에 연결한다. post는 호출자에서 처리기를 실행하지 않지만, 다른 워커가 빠르게 실행하면 post가 반환하기 전에 작업이 시작하거나 끝날 수 있다. 비동기는 반환 이후에만 시작한다는 의미가 아니다.

완성된 소비자 `examples/parallel.cpp`는 계산 객체의 쿼리 결과를 장치 객체의 동적 속성 변경 쿼리로 이어 전달한다. 작업 중 스레드 생성이나 future 대기가 없으며, 계산 큐와 장치 큐가 같은 워커 집합을 공유한다. `./build/iiSDKCoreParallelExample`로 실행한다.

## 객체 실행 영역과 쿼리

SerialQueue의 작업은 접수 순서대로 실행하고 서로 겹치지 않는다. 여러 생산자가 동시에 제출했을 때의 순서는 런타임에 접수된 순서이다. 앞선 작업의 상태 변경은 같은 큐의 다음 작업에 보인다. 작업마다 다른 워커에서 실행될 수 있으므로 OS thread id나 thread_local 값을 객체의 고정 소속으로 사용하지 않는다. `is_current()`로 현재 작업이 해당 객체 큐에 속하는지 확인한다.

큐에 연결된 QueryPort의 기본 호출은 `post(request, completion)`이다. 실행자를 매번 선택할 필요가 없다. 완료 처리기도 같은 큐 안에서 실행하며 앞선 작업의 일부로 간주한다. 다른 객체로 이어지는 동작은 그 객체의 포트에 다시 post한다. 같은 큐의 다음 완료를 동기적으로 기다리는 대신 완료 처리기로 흐름을 연결한다.

연결된 포트의 send는 현재 작업이 같은 큐에 있을 때만 허용한다. 외부 동기 접근은 `QueryErrorCode::wrong_executor`이다. post에 다른 Runtime/EventLoop/SerialQueue를 지정해 연결된 큐를 우회하려 하면 `SubmitError::invalid`이다. 하나의 객체가 여러 포트를 가진 경우 동일한 큐를 공유하여 포트 사이의 상태 접근도 직렬화한다.

연결하지 않은 기존 QueryPort는 명시적인 저수준 기능이다. 직접 send와 EventLoop/Runtime/SerialQueue를 지정하는 post를 유지한다. 연결하지 않은 포트를 병렬 Runtime에 제출하면 처리기의 재진입 가능성과 공유 상태 동기화는 작성자의 책임이다. 큐에 연결한 객체를 애플리케이션 기본 구성으로 사용한다.

## 동적 객체 속성

`ObjectProperties(queue, definitions, policy, on_change)`로 객체의 동적 트리를 실행 영역에 연결한다. 외부는 `properties.queries().post(query, completion)`으로 조회·편집·호출한다. 소유자 apply도 같은 큐 안에서만 허용하고 다른 실행 영역에서는 `PropertyErrorCode::wrong_executor`를 반환한다. 상태 변경·검증·알림이 같은 객체 큐에서 실행되므로 기존 트리 계약을 유지한다.

revision()은 atomic 스냅샷으로 외부에서 읽을 수 있다. revision을 읽었다는 사실이 다른 필드의 잠금이나 일관된 전체 스냅샷을 제공하지는 않는다. 실제 값과 메타데이터는 쿼리로 선택하여 받는다. 생성 시 정의는 객체가 공개되기 전에 초기화한다. 큐를 지정하지 않는 기존 생성자는 직접 사용하거나 외부에서 직렬화하는 저수준 계약을 유지한다.

## 예약과 호스트 루프

Runtime은 작업 실행과 워커 수명, SerialQueue는 객체의 실행 순서를 담당한다. 기존 EventLoop는 타이머·주기·우선순위 이벤트와 호스트의 프레임 처리를 담당한다. GUI/브라우저 메인 스레드는 호스트 이벤트를 처리하고 긴 계산을 Runtime에 제출한다. 네이티브 타이머 루프는 필요하면 Thread가 소유할 수 있다. 같은 EventLoop를 여러 워커가 동시에 pump하도록 바꾸지 않는다.

의존 방향은 QueryPort → Runtime/SerialQueue → Thread → C++ 스레드이며, Runtime과 EventLoop는 공통 작업 토큰 식별자를 사용한다. EventLoop가 Runtime이나 QueryPort를 참조하지 않는다. 상속·전역 실행자·Qt 의존성은 없다.

## 용량·취소·메모리

RuntimeOptions는 workers와 capacity이다. workers=0은 기본 정책, 명시적인 1은 제한된 단일 워커 구성이다. capacity 기본값은 1024이고 0은 생성 실패이다. capacity는 대기와 실행 중인 작업을 모두 포함하며 Runtime의 모든 SerialQueue가 공유한다. 포화 시 post는 full을 반환한다. 객체 큐마다 별도의 무제한 대기열을 만들지 않는다.

생성 시 작업 슬롯·free-list·Thread 인스턴스를 확보하고 반복 사용한다. SerialQueue 생성은 실행 영역 상태를 한 번 확보한다. 실제 선택은 고정 슬롯을 순회하는 O(capacity) 검색이고 현재 구현은 단순한 mutex 기반이다. 준비된 큐 사이에서 high:normal:background 가중치 4:2:1로 선택하며, 같은 객체 큐에서는 우선순위와 관계없이 FIFO를 보존한다. 실행 중 작업을 선점하지 않는다.

post의 Submission은 접수 결과이며 작업 완료 결과가 아니다. 취소 토큰은 실행자와 슬롯 세대를 구분하므로 EventLoop/다른 Runtime/재사용 전 슬롯에 쓸 수 없다. cancel은 대기 중인 작업만 제거하며 이미 실행 중인 작업에는 false이다. 취소한 쿼리의 처리기와 completion은 호출하지 않는다. 작업 캡처는 내부 잠금을 해제한 뒤 파괴하므로 소멸 중 새 작업을 제출할 수 있다.

준비된 Runtime/SerialQueue에 작은 빈 작업을 1만 회 제출·완료하는 tests/reuse.cpp 측정은 추가 operator new 호출이 0인지 검사한다. 임의 콜백, 쿼리 payload, 결과 복사, 동적 객체 스키마 또는 OS 메모리 할당 전체를 무할당이라고 주장하지 않는다.

## 완료·실패·종료

wait_idle은 현재 pending이 0이 될 때까지 기다리며 새 제출을 막지 않는다. 다른 생산자가 계속 제출하면 대기가 길어질 수 있고 반환 직후에도 새 작업이 도착할 수 있다. 실행 중인 작업의 캡처 정리는 완료 집계 전에 수행한다. 별도 호출자가 취소하면서 정리하는 캡처의 수명까지 대신 소유하지는 않는다.

작업 예외는 워커를 종료시키지 않는다. 실패 횟수를 stats.failed에 누적하고 첫 예외를 보존하여 wait_idle/join에 work_failed로 반환한다. 나머지 작업은 계속 진행한다. 개별 요청의 모든 결과가 필요하면 QueryPort의 completion으로 받는다. 그 completion 자체가 던진 예외도 Runtime 작업 실패로 기록한다.

request_stop(drain)은 새 접수를 막고 이미 접수한 작업을 완료한다. request_stop(cancel_pending)은 새 접수를 막고 대기 작업을 취소하며 실행 중 작업은 완료하도록 둔다. drain에서 cancel_pending으로 강화할 수 있고 반대로 되돌리지 않는다. 이미 접수한 작업이 종료 중에 새 후속 작업을 제출하면 stopped가 될 수 있으므로 접수 결과를 처리한다.

join은 접수를 닫고 drain한 뒤 모든 워커를 join한다. 이미 cancel_pending 상태이면 해당 종료 정책을 유지한다. Runtime은 종료 후 재시작하지 않으며 새 인스턴스를 만든다. 소멸자도 drain/join한다. 실행 중 작업을 강제 종료하지 않으므로 영구적으로 반환하지 않는 작업은 종료를 막을 수 있다.

자기 Runtime의 워커에서 wait_idle/join하면 self_join을 반환하여 자기 풀 대기를 막는다. 동시에 여러 join을 요청하면 busy가 될 수 있다. 브라우저 메인 스레드의 blocking 대기는 would_block이다. 소멸은 자신의 워커나 다른 호출자가 객체를 사용하는 동안 수행하지 않는다.

## 수명

SerialQueue는 Runtime을 약하게 참조한다. Runtime 소멸 후 제출하면 stopped이다. 이미 제출한 작업은 SerialQueue의 실행 영역 상태를 보유하므로 큐 핸들을 없앤다고 자동 취소되지는 않는다. 큐에 연결한 QueryPort/ObjectProperties는 큐 참조를 빌리므로 큐가 해당 포트와 실행 중 처리보다 오래 살아야 한다.

QueryPort의 대기 작업은 기존 약한 수신자 계약을 유지한다. 아직 실행하지 않은 수신 포트가 사라지면 receiver_expired이다. 실행 중 객체의 소멸이나 사용자 참조 캡처까지 자동 보호하지 않는다. 객체·큐·캡처를 파괴하기 전에 생산을 멈추고 Runtime의 작업 완료/종료를 보장한다. Runtime을 먼저 생성했어도 단순한 역순 소멸만으로 나중에 생성한 객체의 작업 수명이 보장되지는 않는다.

## 플랫폼 기본값

네이티브에서는 같은 Runtime API가 표준 OS 스레드를 사용한다. WASM의 IISDKCORE_WASM_THREADS 기본값은 ON이다. 애플리케이션 main/스레드 관리는 pthread 워커에서 실행하고 브라우저 메인 스레드는 프레임·DOM 호스트로 남긴다. 테스트·예제는 PROXY_TO_PTHREAD와 16개 초기 호스트 워커를 사용한다. 이 풀 크기를 모든 SDK 소비자에게 강제하지는 않는다. COOP/COEP 등 호스트 조건은 [Thread 문서](thread.md)를 따른다.

IISDKCORE_WASM_THREADS=OFF는 명시적인 제한 빌드이다. Thread와 Runtime을 호출자 인라인 실행으로 흉내 내지 않는다. Runtime 생성은 거절하며 기존 EventLoop::poll과 BrowserLoop로 호스트 단일 스레드 기능을 사용할 수 있다. 이전 CMake 캐시의 OFF 값은 새 기본값으로 자동 덮어쓰지 않으므로 기존 빌드는 ON으로 재설정해야 한다.

브라우저 검증은 tests/serve_wasm.py로 pthread 빌드 디렉터리를 제공한 뒤 `parallel-browser.html`과 `iiSDKCoreBrowserTests.html`을 연다. 전자는 실제 병렬·직렬 큐 계약, 후자는 브라우저 메인 스레드의 blocking 실행 거절과 프레임 루프 계약을 검사한다. Node CTest와 실제 브라우저 검증은 별도로 기록한다.
