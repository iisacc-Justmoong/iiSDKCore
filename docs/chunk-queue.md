# CPU 소유 청크 작업 큐

`ii::runtime::ChunkQueue`는 애플리케이션이 소유한 독립 원소 작업을 하나의 용량 제한 큐에서 받는다. 여러 생산자가 동시에 `submit`할 수 있다. 영속 CPU 작업자와 호출자가 등록한 외부 장치 레인이 각자 한 청크씩 가져간다. 장치 레인은 장치별로 하나를 두는 구성이다. 모든 CPU 작업자가 기본적으로 사용자의 물리 코어에 고정되는 것은 아니다.

## 분할과 장치 연동

`ChunkWork`는 전체 원소 수, 청크 상한, CPU 콜백과 장치 제출 콜백을 받는다. 각 콜백이 실행할 수 있는 레인이 있어야 한다. 청크는 겹치지 않는 `[offset, offset+count)` 범위이며 할당 순서와 종료 순서는 다를 수 있다. 실패나 취소 시 미할당 청크는 실행하지 않는다. CPU 콜백의 결과와 셰이더의 결과가 동등해야 하며 원소 사이의 의존 관계, 입출력 메모리의 수명과 동기화는 호출자가 관리한다. 임의의 C++ 또는 Python 함수를 자동으로 GPU 코드로 바꾸지 않는다.

장치 레인의 `group_size`와 `max_groups`는 호출자가 장치 한도 및 실제 셰이더의 로컬 크기에 맞게 지정한다. 런타임은 `count`를 청크 상한과 `group_size * max_groups`로 제한하고 `groups = ceil(count / group_size)`를 계산한다. 셰이더는 남는 논리 스레드가 유효 범위 밖에 접근하지 않게 해야 한다. 이것은 물리 GPU 코어나 CPU 코어를 직접 지정하는 API가 아니다. 운영체제·GPU 드라이버가 실제 실행 장치를 배정한다.

장치 콜백은 제출이 완료된 뒤 `ChunkSubmission`을 반환한다. `wait`는 실제 장치 작업이 끝나거나 장치 실패로 종료되어 관련 자원을 안전하게 놓을 수 있을 때만 반환해야 한다. `collect`는 성공한 장치 작업의 결과를 CPU로 회수한다. 제출 후 완료 대기 객체를 만들지 못한 경우에도 어댑터가 완료를 기다린 뒤 자원을 놓아야 한다. iiGPU의 `Submission`을 `wait`에 캡처하고 출력 `Buffer`를 `collect`에 캡처하는 통합 예제는 `iiGPU/example/CooperativeChunks`에 있다. Core와 iiGPU는 서로 링크하지 않으며 애플리케이션이 두 API를 조합한다.

장치별 점유율, 물리 코어 사용률, 작업 소요 시간 측정이나 자동 성능 튜닝은 제공하지 않는다. CPU와 장치가 다음 청크를 경쟁하며 균형을 맞추므로 두 레인이 반드시 각각 작업을 받는 것도 아니다. 우선순위, 마감, 강제 선점, 외부 장치 간 메모리 이동은 이 큐의 계약에 포함되지 않는다. 작은 작업이나 전송 비용이 큰 작업의 GPU 사용이 더 빠르다는 보장은 없다.

## 상태·중단·자원 수명

`ChunkTicket::snapshot()`과 `wait_for_change(revision, timeout)`은 이벤트 기반으로 상태를 관측한다. `claimed_items`는 배정된 원소, `completed_items`는 CPU 계산 또는 장치 완료와 결과 회수를 마친 원소다. `device_submitted`는 누적 제출 건수, `device_in_flight`는 완료 대기 중인 제출 건수, `device_active`는 준비·대기·결과 회수를 포함해 레인이 점유된 건수다. 이는 애플리케이션의 제출 관측치이며 GPU 하드웨어 사용률은 아니다. 관측 시간 초과는 작업을 취소하지 않는다.

`pause()`는 새 청크 할당을 중지한다. 진행 중인 청크가 있으면 `pausing`, 모두 끝나면 `paused`가 되고 `resume()`이 재개한다. `cancel()`은 새 청크를 중단하고 CPU 콜백에 `std::stop_token`을 요청한다. 이미 제출된 장치 작업과 이미 시작한 결과 회수는 완료를 기다린다. 회수가 시작되기 전에 취소되면 회수를 생략한다. 이 동안 `cancelling`이며 작업과 동시 인터럽트가 모두 끝나야 `cancelled`가 된다. 중간 결과는 남을 수 있고 작업 부작용은 되돌리지 않는다.

`interrupt(type, bytes)`는 임의의 64비트 애플리케이션 인터럽트와 바이트를 등록된 콜백에 전달한다. 콜백은 호출한 스레드에서 동기로 실행되고 여러 호출이 겹칠 수 있다. 수락·성공은 콜백의 반환을 뜻하며 임의 코드를 강제 선점한다는 의미가 아니다. 핸들러가 없거나 작업이 종료 중이면 오류다. 작업당 동시 인터럽트 수는 `max_concurrent_interrupts`로 제한한다.

`close()`와 소멸은 수락된 작업에 취소를 요청하고 실행 중인 콜백과 장치 완료를 기다린다. 콜백과 stop 콜백은 큐의 내부 잠금 밖에서 실행한다. 자기 큐의 콜백에서 `close()`하면 `busy`이며 같은 자리에서 큐를 파괴해서는 안 된다. 작업의 최종 상태는 계산과 콜백 실행이 끝났다는 뜻이다. `ChunkTicket`이 보유한 콜백 캡처는 티켓이 해제될 때까지 살아 있을 수 있다. 스레드를 사용할 수 없는 웹 호스트에서는 이 작업자 기반 구성을 사용할 수 없다.

## C++·C·Python

C++은 `<iiSDKCore/Runtime/ChunkQueue.h>`와 `iiSDKCore::Runtime`을 사용한다. 장치 코드는 SDK 경계를 넘지 않도록 애플리케이션에서 연결한다. `<iiSDKCore/Runtime/ChunkQueueC.h>`는 동일한 큐와 콜백을 C로 노출한다. C 컨텍스트와 콜백은 해당 작업의 최종 상태까지 살아 있어야 한다. 장치 제출 콜백이 성공하면 대기·선택적 회수·해제 콜백은 소유한 제출을 하나씩 처리한다. 외부 대기 콜백의 실패 반환도 실제 장치 작업의 종료 또는 안전한 자원 해제를 뜻해야 한다.

설치된 `iisdkcore_resources.Library`의 `chunk_queue`는 Python 인터페이스다. `cpu(offset, count)` 또는 `device(lane, offset, count, group_size, groups)`를 제공한다. 장치 콜백의 반환 객체는 `wait()`와 선택적인 `collect()`를 제공한다. `interrupt(kind, bytes)`도 제공할 수 있다. Python 콜백은 네이티브 작업자 스레드에서 실행된다. CPython GIL 때문에 순수 Python CPU 계산이 코어 수만큼 병렬로 빨라진다고 보장하지 않는다. Python CPU 콜백은 stop token을 받지 않으므로 취소 후 이미 시작된 콜백이 반환될 때까지 기다린다. Python 장치 콜백은 제출이 이미 이뤄진 상태에서 실패하면 직접 안전하게 완료를 보장해야 한다. 현재 iiGPU Python의 동기식 `ComputeManager.execute`는 제출과 완료를 별도로 노출하지 않는다. 따라서 이를 이 큐의 비동기 장치 제출로 표시하지 않는다.

```python
from iisdkcore_resources import Library

lib = Library("/path/to/libiiSDKCore.dylib")
with lib.chunk_queue(cpu_workers=4, capacity=32) as queue:
    task = queue.submit(items=1000, chunk_items=64, cpu=process_range)
    state = task.snapshot()
    while state.phase not in ("completed", "cancelled", "failed"):
        state = task.wait_for_change(state.revision, 1.0)
    if state.phase == "failed":
        print(task.errors)
```

Python 큐는 활성 작업의 콜백을 유지하며 새 제출이나 `reap()`에서 끝난 작업에 대한 큐의 참조를 놓는다. 사용자가 보유한 티켓은 계속 상태를 읽을 수 있다. 동일 객체를 다른 스레드에서 수동 소멸시키는 동안 호출하지 않는다.

## 검증 경계

`tests/chunk_queue.cpp`와 `tests/chunk_queue_python.py`는 다중 생산자, 분할, 논리 그룹 제한, 제출 관측, 일시 정지·재개·취소, 임의 인터럽트, 실패, 완료와 결과 회수의 구분을 검사한다. `iiGPU/tests/cooperative_chunks.cpp`는 가짜 장치로 실제 iiGPU 명령·제출 객체와 Core 큐를 조합해 CPU와 장치 청크 결과를 비교한다. 이 모의 검사는 실제 RTX·Apple GPU 성능이나 장치 드라이버의 동작을 검증하지 않는다.
