# 객체 제거와 자원 반환

버전 0.3.0의 제거 완료는 해당 객체에 등록한 작업과 객체가 소유한 자원을 정리하고, 다음 객체가 작업·노드 할당량을 사용할 수 있는 상태를 뜻한다. 이 계약은 Qt 없이 C++23의 소유권과 기존 실행자 위에서 구현한다.

## 제거 순서

1. 해당 객체의 새 작업 접수를 막는다.
2. 미실행 작업과 타이머를 취소하고 입력·완료 콜백·캡처를 파괴한다. 취소된 completion은 실행하지 않는다.
3. 이미 실행 중인 처리기·동기 호출·completion과 그 캡처의 파괴를 기다린다. 실행을 강제 선점하지 않는다.
4. 실행자의 슬롯을 반환하고 객체의 마지막 정리 콜백을 실행하여 소유 자원을 해제한다. Scene은 속성 자원을 먼저 정리한 뒤 노드 슬롯을 반환한다.
5. 성공한 close 또는 소멸이 반환하면 해당 객체의 등록 작업·소유 자원과 예약 할당량이 남아 있지 않다.

작업의 자원 파괴 중에도 해당 슬롯은 pending에 포함된다. 따라서 소멸자가 새 작업을 제출할 때는 이미 비어 있는 다른 슬롯을 사용해야 한다. 제거 중인 슬롯은 아직 사용할 수 없다. 사용자 캡처와 정리 콜백은 실행자의 내부 잠금 밖에서 파괴·실행한다.

## API별 적용

| API | 제거 동작 |
|---|---|
| TaskScope::close / 소멸 | 등록한 post를 취소하고 run·활성 콜백·캡처 및 슬롯 반환을 기다린 뒤 cleanup을 한 번 실행 |
| TaskScope::request_close | 접수 차단·등록 작업 취소를 요청하고 활성 작업을 기다리지 않음. 늦게 등록된 토큰도 취소하며 최종 수명 장벽은 close |
| QueryStream / Subscription::close | 발행·쿼리 접수 차단, 대기 출력·펌프 취소, 활성 선택·전달·캡처 파괴 대기 후 구독 슬롯 반환 |
| QueryPort::close / 소멸 | 대기 요청·completion 취소, 활성 send·처리기·completion 대기, 처리기 캡처 해제 |
| SerialQueue::request_close | 해당 큐의 접수 차단과 대기 작업 취소. 활성 작업 대기는 하지 않음 |
| SerialQueue::close / 소멸 | request_close 이후 해당 큐의 활성 작업·캡처와 슬롯 반환까지 대기 |
| ObjectProperties 소멸 | 루트 포트를 먼저 닫은 뒤 값·메서드·검증기·알림·하위 트리 해제 |
| Scene::erase | 하위 트리 전체 접근 무효화, 포트·속성 자원 정리, 간선 저장소 해제와 노드 할당량 반환 |
| AsyncIO::close / 소멸 | 해당 큐와 포트 정리, 요청 버퍼·completion 캡처 및 Channel 소유 참조 해제 |
| TaskGroup 소멸 | 대기 작업 취소와 활성 작업·캡처 대기. 기존 close는 정상 완료를 위한 봉인 계약 유지 |
| EventLoop::request_stop / 소멸 | 전체 접수 차단과 미실행 작업·타이머 취소. 외부 드라이버의 종료·join은 호스트 책임 |

SerialQueue와 TaskScope는 다른 객체의 작업이나 공통 Runtime을 종료하지 않는다. TaskToken은 실행자를 약하게 참조하므로 실행자 제거 후 cancel()/pending()은 false이며 새 객체의 재사용 슬롯을 건드리지 않는다. 세대와 실행자 식별자는 오래된 참조를 막기 위해 유지한다.

## 일반 객체의 조합

일반 C++ 객체의 작업은 TaskScope에 등록한다. TaskScope가 자동으로 다른 큐의 임의 람다나 직접 메서드를 발견하지는 않는다. 상태를 먼저 선언하고 포트·수명 범위를 나중에 선언한다. 소멸자 본문에서 상태를 수정하기 전에 수명 범위를 닫는다. 같은 객체의 변경을 직렬화할 때는 SerialQueue를 함께 사용한다.

```cpp
#include <iiSDKCore/Runtime/TaskScope.h>
#include <iiSDKCore/Runtime/Runtime.h>
#include <cstddef>
#include <exception>
#include <vector>

class WorkerObject final {
    std::vector<std::byte> data_;
    ii::runtime::SerialQueue queue_;
    ii::runtime::TaskScope work_;
public:
    explicit WorkerObject(ii::runtime::Runtime& runtime)
        : data_(4096), queue_(runtime), work_([this] {
            std::vector<std::byte>{}.swap(data_);
        }) {}
    ~WorkerObject() {
        if (!work_.close()) std::terminate();
    }
    ii::runtime::Submission update() {
        return work_.post(queue_, [this] { data_[0] = std::byte{42}; });
    }
    ii::runtime::ThreadResult close() { return work_.close(); }
};
```

이 객체의 update는 close 이후 stopped이다. 작업 취소·완료 및 슬롯 반환 후 cleanup이 data_의 할당을 해제한다. 외부에서 메서드를 부르기 위해 빌린 객체 참조는 객체 자체의 소멸보다 오래 사용할 수 없다. TaskScope::run은 동기 작업을 같은 제거 경계에 등록하지만 상태 접근을 자동 직렬화하지 않는다.

## 실패와 실행 영역

close는 반복 호출할 수 있으며 성공 후 다시 작업을 받지 않는다. cleanup은 한 번만 실행하고, 예외가 나도 캡처를 해제한 뒤 work_failed와 원래 예외를 반환한다. 정리 코드가 실패한 OS 자원까지 복구했다는 보장은 없으므로 명시적 close의 실패를 확인한다.

같은 범위의 처리기·completion·캡처 소멸자·cleanup에서 자기 close를 호출하면 self_join이다. 자신의 SerialQueue 실행 중 close도 self_join이다. Runtime 워커나 브라우저 메인 스레드에서 대기가 필요하면 would_block이며, close 성공으로 간주하지 않는다. 안전한 외부 실행 영역에서 다시 close한다. 이런 오류를 처리할 수 없는 문맥에서 객체를 파괴하면 terminate하여 해제한 상태에 작업이 접근하는 것을 막는다. Scene::erase는 활성 하위 속성을 busy로 거절하므로 다음 큐 작업에서 다시 제거한다.

실행 중인 무한 작업이나 끝나지 않는 OS 전송이 있으면 제거도 완료할 수 없다. 작업을 협력적으로 끝낼 수 있게 설계하고, 필요하면 I/O의 CancellationToken을 먼저 요청한다. request_close도 대기 작업의 캡처 소멸자를 동기 실행하므로 호출 전체가 항상 즉시 반환하는 API는 아니다.

## 소유권과 메모리의 범위

객체가 보유한 마지막 소유 참조를 정리한다. 외부 공유 소유자·응답 스냅샷·Buffer/Channel 별칭이 남아 있으면 해당 소유자의 자원은 유지된다. 명시적인 Channel::close 또는 IOQuery의 Close는 모든 채널 별칭을 닫는 별도 명령이다. 영속 파일·저장한 결과·전역 호스트 자원도 그 소유자가 관리한다.

공통 Runtime/EventLoop/Scene의 미리 확보한 슬롯 저장 공간은 비어 있는 재사용 용량으로 유지한다. 개별 작업의 payload·캡처와 제거한 속성·간선 저장소를 유지하는 캐시는 두지 않는다. 외부 TaskToken이 남으면 약한 참조 제어 블록의 메타데이터는 해당 토큰 수명까지 남을 수 있으나 작업 자원이나 실행자 자체를 소유하지 않는다.

일반 C++ 해제는 할당자를 통해 재사용 가능한 메모리를 반환한다. 프로세스 RSS가 즉시 감소하거나 OS 페이지를 모두 돌려주거나 해제한 바이트를 보안 목적의 0으로 덮는 계약은 아니다. 이러한 요구는 별도의 명시적 버퍼·할당자 정책이 필요하다.

## 호환성과 검증

`TaskScope::try_close()`는 `close()`와 같은 접수 차단·대기 작업 취소·캡처 및 슬롯 퇴역·
cleanup 경계를 사용하되, 아직 남은 작업이나 캡처가 있으면 기다리지 않고 `would_block`을
반환한다. 소유자는 기존 이벤트 루프에서 다시 검사할 수 있다. 자신의 활성 콜백에서는
`self_join`을 반환한다. 완료 시 cleanup은 한 번만 실행하며, cleanup 자체의 실행 시간과
대기 캡처 소멸자의 실행 시간까지 강제로 제한하는 계약은 아니다. 최종 소멸 전에는 성공
또는 보존한 cleanup 오류로 종료가 확정되어야 한다. `tests/task_scope_try_close.cpp`는
대기 취소·활성 작업·캡처 소멸 중 점유·자기 호출·cleanup 오류·반복 완료를 검사한다.

0.2.0에서는 포트 제거 후 대기 completion이 receiver_expired로 실행되고 SerialQueue 핸들 제거 후에도 작업이 남을 수 있었다. 0.3.0에서는 제거된 객체의 대기 작업을 취소하고 자원을 즉시 정리한다. completion이 반드시 한 번 호출되는 RPC 보장은 기존에도 없었다. 제거 후 오류 completion에 의존한 소비자는 취소·제거를 명시적으로 처리해야 한다. TaskToken의 내부 레이아웃도 변경하였으므로 헤더와 라이브러리를 함께 갱신하고 소비자를 다시 빌드한다.

`tests/cleanup.cpp`는 대기·활성 쿼리, 완료 콜백, 동기 호출, 접수 경쟁, 캡처 소멸 중 할당량 점유, 다른 객체 보존, 자체 제거 거절, Scene·TaskGroup·AsyncIO 제거, 실패한 접수·cleanup, 실행자 소멸 후 토큰과 1,000회 반복 제거·재사용을 검사한다. 실행 환경별 결과는 [validation.md](validation.md)에 기록한다.

## Additional family removal boundaries (0.8.0)

TaskScope::at registers timed one-shot captures with the same cancellation and retirement barrier as post. Animation uses it for phase-aligned frames; close retires track and completion captures after active frames finish. EventReceiver filters and handlers are owned QueryPort captures. Plugin close quiesces calls and completions, destroys its context, and then releases the module reference. ParallelResult cancellation settles discarded task tickets; future readiness itself does not promise executor slot retirement. SharedValue snapshots preserve explicitly shared external ownership. The external `iiFileProvider::core::AtomicFile` cancel/destruction removes the uncommitted temporary, while a committed target remains persistent user data. Filesystem cleanup errors are observable through explicit cancel. See [modern core contracts](modern-core.md).
