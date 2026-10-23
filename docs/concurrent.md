# 비동기 계산과 병렬 컬렉션 처리

`Runtime/Concurrent.h`는 기존 `Runtime`의 지속 워커에 계산을 제출한다.
`Future`·`Promise`는 다중 결과, 진행률, 협력적 취소·일시정지와 최종 상태를
공유하고, `FutureWatcher`는 지정 실행기에 상태 관찰을 전달한다.
전역 풀이나 작업별 스레드, Qt, 상위 SDK 의존성을 추가하지 않는다.

QtConcurrent가 해결하는 문제를 기준으로 다음 기능을 제공한다. Qt API나
QObject의 동작을 그대로 복제하는 호환 계층은 아니다. 참고 명세는
[Qt Concurrent 개요](https://doc.qt.io/qt-6/qtconcurrent-index.html)와
[promise 실행 모드](https://doc.qt.io/qt-6/qtconcurrentrun.html)이다.

| 요구사항 | iiSDKCore 객체·API |
|---|---|
| 풀에서 일반 함수 실행 | `Concurrent::run`, `run_with_priority` |
| 인수·우선순위 설정 후 실행 | `task`, `TaskBuilder::with_arguments/with_priority/spawn` |
| 결과와 상태 관찰 | `Future<T>`, `FutureSnapshot`, `result_at/results/get/get_all` |
| 여러 결과·진행률 보고 | `Promise<T>`, `add_result`, `set_progress_range/set_progress/advance_progress` |
| 완료·진행률 관찰 | `FutureWatcher<T>` |
| 여러 계산의 종료 대기 | `FutureSynchronizer<T>` |
| 입력별 병렬 변환·선별 | `mapped`, `filtered` |
| 소유 컬렉션의 병렬 수정 | `map`, `filter` |
| 변환·선별 후 순차 집계 | `mapped_reduced`, `filtered_reduced` |
| 호출자에서 결과 대기 | 각 컬렉션 API의 `blocking_` 형태, `Future::get/get_all` |

## 사용 예

```cpp
#include <iiSDKCore/Runtime/Concurrent.h>
using namespace ii::runtime;

Runtime workers({.workers = 4, .capacity = 64});
Concurrent compute(workers);

auto answer = compute.run([](int value) { return value * 2; }, 21);
auto squares = compute.mapped(std::vector{1, 2, 3, 4},
    [](int value) { return value * value; }, {.grain = 1});
auto sum = compute.mapped_reduced(std::vector{1, 2, 3, 4},
    [](int value) { return value * value; },
    [](int& result, int value) { result += value; }, 0);

// 콘솔의 외부 동기화 지점이다. GUI·워커에서는 watcher로 이어 처리한다.
auto scalar = answer.get();       // core::Result<int>{42}
auto values = squares.get_all();  // core::Result<vector<int>>{{1,4,9,16}}
auto total = sum.get();           // core::Result<int>{30}
auto closed = compute.close();   // 이 인스턴스의 콜백·캡처 제거 장벽이다.
```

실행 예제는 [examples/concurrent.cpp](../examples/concurrent.cpp)이다.
실제 코드에서는 `core::Result`, 접수 상태와 `close()` 결과를 처리한다.

## 소유권과 실행

`Concurrent`는 `Runtime&`, `TaskScope`, 소유자 중단 토큰을 조합한다.
Runtime이 Concurrent와 실행 중 작업보다 오래 살아야 한다. `run`은 함수와
인수를 값으로 저장하며 move-only 함수·인수·결과도 지원한다. 인수는 실행 시
이동하여 전달한다. 외부 참조는 `std::ref` 또는 명시적인 참조 캡처로만 빌린다.
그 경우 참조 대상의 수명·동기화는 호출자의 책임이다.

`TaskBuilder`는 값으로 함수·인수를 보관하며 rvalue의 `spawn(compute)`가
소유권을 넘긴다. 실행기는 spawn 시 명시하므로 전역 초기화 순서나 기본 풀에
의존하지 않는다. `with_priority`는 기존 Runtime의 가중치 예약에 전달된다.

`map`·`filter`를 포함한 컬렉션 API의 입력은 `std::vector<T>` 소유 값이다.
lvalue 입력은 복사되고 rvalue 입력은 이동된다. 호출자가 계속 사용하는 벡터를
백그라운드에서 빌려 수정하지 않는다. `map`·`filter` 결과는
`Future<std::vector<T>>`의 단일 소유 벡터이고, `mapped`·`filtered`는
`Future<T>`의 여러 결과이다. iterator·다른 범위는 호출자가 소유 벡터로
변환한다. 공유 비트를 수정하는 `vector<bool>`의 `map`은 거절하며 `mapped`를
사용한다. 다른 API의 bool 결과는 독립 저장소를 사용한다.

청크마다 mapper/predicate를 복사한다. 따라서 병렬 콜백은 복사 가능해야 하며,
복사본끼리 빌리는 가변 상태는 별도로 동기화한다. `ConcurrentOptions::grain`은
최소 청크 크기이며 0은 오류이다. 전체 청크 수가 Runtime의 슬롯 용량을 넘지
않도록 청크 크기를 키운다. 이미 사용 중인 슬롯 때문에 접수가 실패할 수 있으며
작업이나 대기열을 무제한 추가하지 않는다.

## 결과·오류·진행률

`Future`는 복사 가능한 관찰 끝점이며 소멸 시 계산을 취소하거나 기다리지 않는다.
`Promise`는 이동만 가능한 생산자이다. 같은 Promise의 메서드는 내부에서
동기화하지만 이동·소멸은 메서드를 호출 중인 생산자와 외부에서 동기화해야 한다.
일반 수동 사용은 `start()` → 결과·진행률 보고 → `finish()` 순서이다.
`run_with_promise<T>`는 이 시작·완료 과정을 자동으로 수행한다.

```cpp
auto future = compute.run_with_promise<int>([](Promise<int>& promise) {
    (void)promise.set_progress_range(0, 3);
    for (int i = 0; i < 3; ++i) {
        if (!promise.checkpoint()) return;
        auto added = promise.add_result(i * 10);
        if (!added) { (void)promise.fail(added.error()); return; }
        (void)promise.set_progress(i + 1, "calculating");
    }
}, {.max_results = 3});
```

`PromiseOptions::max_results` 기본값은 1024이고 인덱스 저장소의 상한이다.
알고리즘은 입력 개수 또는 단일 결과에 맞춰 한도를 설정한다. 결과는 한 번
공개하면 수정할 수 없다. 중복 인덱스, 한도 초과, 완료 후 쓰기는 구조화 오류이다.
`add_result(value, index)`는 서로 다른 인덱스의 완료 순서가 달라도 원래 순서를
보존한다. 인덱스를 생략하면 당시 저장소 끝에 추가한다.

`mapped`와 `filtered`의 결과 인덱스는 입력 인덱스이다. 필터에서 탈락한 위치는
비어 있으며 `result_at(index)`는 해당 위치가 아직 없거나 탈락했으면 빈
`shared_ptr`를 반환한다. `snapshot().result_count`는 실제 공개한 결과 수이다.
`results()`는 현재 공개된 결과만 입력 순서대로 모은 공유 핸들 목록이다.
아직 완료하지 않은 결과에 접근할 때 대기하거나 예외를 숨기지 않는다.

`get()`은 종료를 기다린 뒤 첫 공개 결과를 복사한다. `get_all()`은 공개 결과를
순서대로 복사한다. 두 메서드는 `core::Result`를 반환하고 실패·취소 상태에서는
오류를 반환한다. move-only 결과는 `wait_for`로 완료를 확인한 뒤
`result_at/results`의 `shared_ptr<const T>`로 읽는다. `Future<void>`는 값 없이
종료 상태를 표현하며 `get()`은 `core::Result<void>`이다. 빈 필터의 `get_all()`은
빈 벡터이고 `get()`은 `not_found`이다.

`FutureStatus`는 `pending`, `running`, `succeeded`, `failed`, `cancelled`,
`rejected`, `abandoned`를 구분한다. 기본 생성한 Future는 `invalid`이다.
함수 예외는 `core::Error::cause`로 보존하고 다른 Runtime 작업을 실패시키지
않는다. 접수 오류는 `snapshot().admission`에 남는다. 청크의 일부가 접수된 뒤
다른 청크가 거절되면 이미 실행한 사용자 부작용은 되돌리지 않는다. 접수가 끝난
계산만 성공하며, 기존 청크는 협력적으로 중단하고 남은 티켓을 정리한다.

최종 상태는 한 번만 확정된다. Promise가 완료 없이 제거되면 `abandoned`이고,
취소가 요청된 생산자는 `cancelled`이다. 실행기가 큐의 콜백을 실행하지 않고
폐기해도 생산자·청크 티켓의 제거가 Future를 확정하므로 무한 대기하지 않는다.
기존에 공개된 불변 결과는 실패·취소 후에도 명시적인 `results()`로 관찰할 수 있다.
계산의 최종 상태와 캡처 제거 완료는 다른 경계이며, 후자는 `Concurrent::close`이다.

진행률 범위는 `minimum <= value <= maximum`으로 검사한다.
`advance_progress()`는 여러 생산자가 같은 범위 안에서 한 단계씩 증가시키며
최댓값에서 멈춘다. 컬렉션 진행률은 처리한 입력 개수이다. reduce가 실행 중인
동안에는 입력 처리가 100%여도 Future는 아직 완료되지 않을 수 있다.

## 취소·일시정지·종료

`cancel()`은 협력적 취소 요청이다. 큐에서 기다리는 작업은 토큰으로 제거하고,
실행 중인 계산은 `checkpoint()`에서 중단을 확인한다. 일반 `run` 함수의 본문은
강제로 중단하지 않는다. 긴 계산에는 promise 모드를 사용하고 단계 사이에
checkpoint를 둔다. `stop_token()`은 Future의 취소 토큰이고
`owner_stop_token()`은 Concurrent 소유자 제거 토큰이다.

`suspend()`·`resume()`도 checkpoint에서 동작한다. 실제로 대기한 생산자 수는
`snapshot().suspended_producers`로 확인한다. 요청만으로 실행 중 함수를 정지했다고
간주하지 않는다. 취소·소유자 제거·명시적 완료는 정지 중인 생산자를 깨운다.
일시정지는 해당 Runtime 워커를 점유하므로 재개 제어는 같은 풀의 새 작업에
의존시키지 않는다. `HostLoop`/브라우저 주 스레드에서 정지 대기는 거절한다.

`close()`는 소유자 중단을 요청하고 대기 작업 취소, 실행 중 콜백 대기, 캡처 제거,
슬롯 반환을 TaskScope 장벽으로 수행한다. Runtime의 다른 소유자는 계속 사용할
수 있다. `Concurrent` 소멸자도 close를 수행한다. 반환하지 않거나 중단을 확인하지
않는 사용자 함수는 종료를 지연시킬 수 있다. 자기 소유 작업에서 자기 close는
`self_join`이며, 다른 Runtime 작업을 기다려야 하는 워커 close는 `would_block`일
수 있다. 제거는 외부 실행 영역에서 완료한다.

`Future::wait_for`의 시간 초과는 관찰 대기만 끝내며 계산을 취소하지 않는다.
아직 끝나지 않은 Future를 Runtime 워커·등록된 HostLoop 스레드·브라우저 주
스레드에서 기다리면 `would_block`이다. `get`도 이 규칙을 따르며 이미 완료한
Future는 해당 스레드에서도 읽을 수 있다. `blocking_` API는 이 실행 영역에서
작업을 제출하기 전에 `wrong_executor`로 거절한다. 동기 함수가 오류로 반환한
뒤에도 참조를 빌린 비동기 콜백이 남는 상황을 방지한다.

`FutureSynchronizer<T>`는 한도 내의 동형 Future를 모아 `wait/wait_for/cancel`을
수행한다. `wait_for`의 시간 예산은 그룹 전체가 공유한다. 실패·취소한 Future도
종료한 항목으로 계산한다. 소멸자에서 암묵적으로 대기하지 않으며 호출자가
대기 위치를 정한다. 그룹 변경 자체는 단일 소유자가 수행한다.

## 완료·진행률 전달

```cpp
#include <iiSDKCore/Runtime/FutureWatcher.h>
EventLoop ui;
FutureWatcher<int> watcher(future, ui, [&](const FutureSnapshot& state) {
    // 이 실행기 안에서 동작한다. 필요하면 객체의 QueryPort에 post한다.
    // Future의 result_at/results는 대기하지 않는다.
});
// 호스트가 ui.poll()을 구동한다.
```

Watcher는 Future마다 최대 16개이고, 관찰 변경을 실행기 작업 하나로 병합한다.
초기 스냅샷도 비동기로 전달한다. 중간 진행률을 모두 보존하는 이벤트 로그가
아니며 콜백은 전달 시점의 최신 스냅샷을 받는다. 콜백은 서로 겹치지 않고
Future 잠금 밖에서 실행되므로 조회·취소나 QueryPort 전달을 수행할 수 있다.
변경이 콜백 실행 도중 도착하면 후속 작업을 예약한다.

실행기 포화·종료 시 `last_submission()`에 오류를 남긴다. 이후 상태 변경에서
재시도하며, 더 이상 변경이 없으면 소유자가 `retry()`를 호출한다. 최종 알림이
큐 포화 때문에 보류된 경우도 이 계약을 따른다. 큐에 접수된 관찰 작업 자체가
외부에서 취소되어도 예약 상태를 반환하여 재시도할 수 있다.

실행기는 Watcher보다 오래 살아야 한다. `close()`·소멸은 대기 알림을 취소하고
실행 중인 콜백과 소유 캡처의 제거까지 기다린다. Watcher의 생성·close·소멸은
소유자가 직렬화한다. 자기 콜백에서 자기 close는 `self_join`이다. 잘못된 Future는
`invalid_argument`, 관찰자 한도 초과는 `length_error`로 생성이 거절된다.

## 범위와 검증

집계는 병렬 변환·선별이 끝난 후 입력 순서로 한 번에 하나씩 실행한다.
사용자 reducer는 비어 있지 않은 정상 배치의 마지막 워커에서 실행한다.
빈 입력은 mapper/predicate/reducer 호출 없이 빈 결과나 초기 집계값으로 완료한다.
unordered/parallel reduce, 자동 continuation 체인, iterator 오버로드,
QObject 신호·슬롯과 GUI 스레드 자동 추론은 제공하지 않는다.
별도의 독립 `Operation`은 마감·외부 부작용 상태를 추적하는 기존 계약을 유지한다.

WASM pthread 빌드는 네이티브와 같은 API를 사용한다. 명시적인 단일 스레드 WASM은
기존 Runtime 생성 제한을 유지하며 수동 Promise/Future의 비차단 관찰을 사용할 수
있다. OS·브라우저·실기기 실행 여부는 빌드 증거와 별도로 기록한다.

테스트는 `tests/future.cpp`, `tests/concurrent.cpp`에 있으며 결과 순서, move-only
데이터, 병렬 실행, 예외, 취소·정지·재개, 소유자 제거, 실행기 폐기, bounded 접수,
관찰 병합·재시도·콜백 캡처 장벽을 검사한다. 설치 소비자는
`tests/concurrent-consumer`로 Qt·파일 공급자를 비활성화하고 검증한다.
현재 실행 결과는 [validation.md](validation.md)에 기록한다.
