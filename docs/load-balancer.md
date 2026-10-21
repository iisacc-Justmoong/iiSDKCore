# 네이티브 컴퓨팅 리소스 로드 밸런서

`Runtime/LoadBalancer.h`의 `ii::runtime::LoadBalancer`는 여러 기존 실행기에
계산 작업을 배분하는 C++23 객체이다. `Runtime`, `SerialQueue`, `EventLoop`의
`TaskExecutor` 계약을 그대로 받는다. 공통 `post(Task, Priority)`를 제공하므로
연결하지 않은 `QueryPort`, `TaskScope`, `Operation`의 실행기로도 사용할 수 있다.
같은 가변 객체의 직렬 처리는 기존 `SerialQueue`로 유지한다. 서로 독립적인 계산을
분산하는 LoadBalancer에 직렬 객체의 큐를 우회하여 제출하지 않는다.

AWS ELB의 [분산 알고리즘과 대상 드레이닝](https://docs.aws.amazon.com/elasticloadbalancing/latest/application/edit-target-group-attributes.html),
[상태 확인과 복구 임계값](https://docs.aws.amazon.com/elasticloadbalancing/latest/application/target-group-health-checks.html)을
요구사항의 참고 자료로 사용했다. AWS SDK, 클라우드 계정, Qt, 가상머신 공통 런타임,
인터프리터, 전용 관리 스레드는 추가하지 않는다. 실제 작업은 지정한 실행기의 기존
네이티브 워커 또는 호스트 루프에서 실행한다.

post의 비동기성·실행 위치는 지정한 실행기를 따른다. 기본 Runtime은 네이티브 워커에서
비동기로 실행한다. 사용자가 직접 인라인 실행기를 등록하면 그 실행기는 호출 스레드에서
작업을 수행할 수 있으며, 로드 밸런서는 이를 별도 비동기 실행으로 위장하지 않는다.

## 사용

```cpp
#include <iiSDKCore/Runtime/LoadBalancer.h>
using namespace ii::runtime;

Runtime foreground({.workers=2, .capacity=16});
Runtime batch({.workers=4, .capacity=32});
LoadBalancer compute({.max_targets=4, .capacity=48});
auto a=compute.add_target(foreground, {.name="foreground", .capacity=16}).value();
auto b=compute.add_target(batch, {.name="batch", .capacity=32}).value();
auto work=compute.submit([] { /* 독립적인 네이티브 계산 */ },
                         {.units=2, .memory_bytes=4096});
if (!work) { /* full, overloaded, stopped, invalid를 처리한다. */ }
auto removed=compute.remove_target(a, ShutdownMode::drain);
auto finished=compute.close(ShutdownMode::drain);
```

완성된 [실행 예제](../examples/load_balancer.cpp)는 서로 다른 용량의 두 네이티브
Runtime에 12개 계산을 배분하고 결과 650을 확인한 뒤 정상 드레이닝한다.
`./build/iiSDKCoreLoadBalancerExample`로 실행한다.

실행기는 참조로 빌린다. 실행기와 실제 실행 자원은 해당 대상의 `remove_target()`
또는 전체 `close()`가 정리를 완료할 때까지 살아 있어야 한다. 같은 실행기를 여러
대상으로 중복 등록하면 각각 독립적인 예약 한도로 취급하므로 보통 한 번만 등록한다.
같은 실행기를 다른 소비자도 사용한다면 그 소비자의 부하는 로드 밸런서의 예약량에
포함되지 않으며 실행기 자체가 `full`을 반환할 수 있다.

## 선택과 용량

`LoadBalancerOptions.max_targets`는 고정 대상 슬롯 수, `capacity`는 전체 접수 작업
수의 상한이다. 0이나 유효하지 않은 분산 정책은 생성 시 `std::invalid_argument`이다.
대상 슬롯은 제거 후 재사용하지만 `LoadTarget.id`는 재사용하지 않는다. 다른 인스턴스나
제거 전의 식별자로 대상을 조작할 수 없다. 등록은 `expected<LoadTarget, SubmitError>`이며
잘못된 용량·임계값은 `invalid`, 대상 표 포화는 `full`, 전체 종료 후는 `stopped`이다.

- `round_robin`: 자격이 있는 대상을 순서대로 한 번씩 선택한다.
- `least_loaded` 기본값: `(예약된 units + 요청 units) / 대상 capacity`가 가장 작은
  대상을 선택한다. 동률은 순환 순서를 적용한다. 비교는 `long double`이며 매우 큰
  정수 비율이 해당 플랫폼 정밀도에서 같아지면 동률로 처리한다.
- 대상의 `capacity`는 예약 가능한 작업량 단위이다. 스레드 수나 OS CPU 사용률이 아니다.
  요청 `units` 기본값은 1이다. 대기·실행·캡처 정리 중 예약량을 모두 합산한다.
- `memory_capacity`는 요청이 **신고한** `memory_bytes`의 동시 합계 한도이다. 0이면
  이 한도를 끈다. OS 메모리 할당 자체를 차단하거나 실제 RSS를 자동 측정하지 않는다.
  카운터 오버플로를 유발하는 요청은 선택에서 제외한다.
- 대상 `capabilities`는 상위 모듈이 정하는 64비트 기능 집합이다. 요청의 모든 비트를
  제공하는 대상만 선택한다. 이 값만으로 GPU·원격 실행이나 데이터 이동이 구현되지는 않는다.

전체 작업 수 한도에 도달하면 `full`, 정상·미드레이닝 상태와 자원 조건을 모두 만족하는
대상이 없으면 `overloaded`이다. 비어 있는 대상 표, 전부 비정상인 경우도 `overloaded`이다.
빈 작업·0 units·잘못된 우선순위는 `invalid`, 전체 접수 종료 뒤에는 `stopped`이다.
별도의 무제한 대기열은 없으며 호출자가 접수 실패에 대응한다. 대상 실행기의 거절은
그 오류 그대로 반환하고 예약량을 회수한다. 다른 실행기로 작업을 자동 재전송하지 않는다.
실행기 예외도 그대로 전파하며 이미 시작한 부수 효과를 중복 실행하지 않는다.
사용자 정의 실행기는 거절한 작업을 보관하거나 실행하지 않아야 한다.

대상 선택·예약·통계·상태 변경은 짧은 인스턴스 mutex 안에서 직렬화한다. 대상 탐색은
`O(max_targets)`이고 대상 표는 생성 시 확보한다. 사용자 작업과 실행기 호출, 작업 캡처
파괴, 대기는 해당 mutex 밖에서 수행한다. 작업과 TaskScope의 수명 레코드는 요청별로
할당하므로 전체 경로를 무할당 또는 잠금 없는 구현이라고 보장하지 않는다.

## 상태 확인

일반 대상은 `healthy`, `require_health_check=true` 대상은 `initial`로 시작한다.
`initial`과 `unhealthy`는 새 작업을 받지 않는다. 소유자의 실제 모니터링 결과를
`report_health(target, bool)`로 전달한다. 연속 실패가 `unhealthy_threshold`에 도달하면
제외하고, 연속 성공이 `healthy_threshold`에 도달하면 복구한다. 반대 결과는 누적 중인
연속 횟수를 초기화한다. 기본값은 실패 3회, 성공 2회이다.

검사 주기는 상위 모니터 또는 기존 EventLoop의 타이머가 소유한다. 디스패치 경로에서
연결 확인이나 블로킹 탐침을 실행하지 않는다. 검사 결과의 의미·시간 제한·CPU/GPU/원격
자원 측정은 실제 자원을 소유한 모듈이 결정한다. 작업 예외나 실행기 포화만으로 대상을
자동 비정상으로 판정하지 않는다. 상태 변경은 이미 접수한 작업을 취소하지 않는다.
AWS의 모든 대상 비정상 시 fail-open과 달리 이 구현은 `overloaded`로 접수를 거절한다.

## 취소·제거·종료

반환하는 `Submission.token`은 실제 대상 실행기의 토큰이다. `token.cancel()`은 해당
실행기의 대기 취소 규칙을 따르며 이미 실행 중인 C++ 작업을 선점하지 않는다. 실제
장시간 계산의 중간 취소는 기존 `Operation` 등의 협력적 취소 계약으로 처리한다.
취소·완료·실패 모두 사용자 캡처를 먼저 파괴하고 예약량을 반환한다. 파괴 중 재진입하여
새 작업을 접수해도 아직 정리 중인 작업의 용량을 앞당겨 재사용하지 않는다.

`request_drain(target)`은 해당 대상의 새 접수만 막는다. `remove_target(target, drain)`은
기존 작업의 종료를 기다린 다음 대상을 제거한다. 기본 `cancel_pending` 제거는 해당
대상의 TaskScope를 닫아 대기 작업을 취소하고 실행·캡처 정리를 기다린다. 실행기 전체를
중단하거나 다른 소비자의 작업을 취소하지 않는다. 제거된 식별자의 상태 보고는 false이다.

`request_stop()`은 전체 새 접수를 막고 모든 대상을 드레이닝 상태로 둔다.
`close(drain)`은 접수 작업을 모두 완료한다. 기본 `close(cancel_pending)`과 소멸자는
전체 대기 작업을 취소하고 활성 작업을 기다린다. 종료는 재시작할 수 없으며 close는
반복 호출할 수 있다. 스스로 실행되지 않는 EventLoop를 drain하면서 기다리면 완료할
수 없으므로 먼저 그 호스트에서 pump하거나 cancel_pending을 사용한다.

전체 취소는 모든 대상의 `TaskScope::request_close()`를 먼저 호출한 후 각 범위의
`close()`를 기다리는 두 단계이다. 첫 대상이 실행 중이어도 다른 대상의 대기 자원을
먼저 취소하며, 실행기 접수 반환과 토큰 등록 사이에 요청된 취소도 등록 시 적용한다.

`wait_idle()`은 접수를 닫지 않고 현재 예약 작업이 0이 될 때까지 기다린다. 반환 직후
새 작업이 들어올 수 있으며 외부에서 실행기에 직접 제출한 작업은 기다리지 않는다.
작업 예외는 실패 통계와 첫 `exception_ptr`로 보존하고, wait/제거/close에 `work_failed`를
반환한다. 이 결과는 정리가 완료된 상태이며 제거된 대상을 재사용하지 않는다. 실행기
Runtime도 원래 예외를 관측한다. 이후 성공이 과거 실패를 지우지 않는다.

자기 작업·캡처 소멸자에서 기다리거나 제거·종료하면 `self_join`이다. Runtime 워커와
브라우저 메인 스레드는 미완료 작업을 블로킹 대기하지 않고 `would_block`을 반환한다.
제거·close가 겹치면 `busy`이다. 이 경우 드레이닝·종료 상태를 유지하고 외부 소유자가
다시 정리해야 한다. 자기 콜백 안이나 다른 사용자가 접근 중일 때 객체를 소멸시키지 않는다.
일반 post/통계/상태 보고/드레이닝은 여러 생산자에서 호출할 수 있다.

## 관측과 검증

`stats()`는 전체 접수·완료·실패·취소·거절·현재 점유·최대 점유·대상 수를,
`target_stats(id)`는 대상 상태·작업량·신고 메모리량과 대상별 카운터를 복사해 반환한다.
아직 대상이 선택되지 않은 거절은 전체 거절에만 포함된다. `submitted`는 실제 접수 작업
수이고 `completed`와 `failed`는 서로 배타적이다. `cancelled`는 접수 뒤 실행하지 않고
캡처가 폐기된 작업이다. 정리가 진행 중인 작업은 아직 `in_flight`에 남는다.

`tests/load_balancer.cpp`는 이질적 용량 분산, 전역 상한, 순환 선택, 기능·메모리 조건,
초기 검사·제외·복구, 전체 비정상 거절, 드레이닝, 토큰 취소, 캡처 선행 정리, 오래된
식별자, 실행기 거절·예외·인라인 실행, 네이티브 병렬 생산자, QueryPort 연동을 검사한다.
설치 소비자도 같은 테스트와 예제를 설치된 공개 헤더·라이브러리만으로 빌드한다.
Core의 Qt/상위 모듈 경계와 공개 final/단일 상속 정책 검사도 계속 적용한다.

### 2026-10-21 검증 기록

| 검증 범위 | 결과 |
|---|---|
| macOS arm64 Release 전체 빌드·CTest | 45/45 통과 |
| 임시 설치 패키지의 Qt 검색 비활성 소비자 | 21/21 통과 |
| 기본 `~/.local/SDK/iiSDKCore` 설치본 소비자 | 21/21 통과 |
| Debug ThreadSanitizer 로드 밸런서 계약 | 1/1 통과, 데이터 경합 보고 없음 |
| WASM pthread 빌드·Node 실행 | 계약과 예제 2/2 통과 |
| 변경한 구현의 C++23 엄격 경고 검사 | `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror` 통과 |

기본 설치본 예제에서 `native load balancing complete: jobs=12 sum=650`을 확인했다.
dyld 기록으로 실제 기본 SDK 라이브러리를 불러왔는지 확인했고, 빌드·임시 설치·기본
설치의 라이브러리와 공개 헤더 SHA-256도 대조했다. macOS 동적 의존성은 libc++와
libSystem이다. Windows·Linux·모바일 실기기와 실제 브라우저 실행은 이 기록의 검증
범위에 포함하지 않는다. WASM의 실행 증거는 Node pthread 호스트이다.

이번 로컬 검증의 XML, 빌드 로그, 설치 전 라이브러리 백업, 의존성·dyld 기록과
`summary.json`은 `build/load-balancer-20261021/`에 있다. 재현은 저장소 루트에서
`cmake -S . -B build`, `cmake --build build`, `ctest --test-dir build --output-on-failure`를
사용한다. 설치 후 `tests/consumer`를 별도 `build/` 경로에 구성하고 `iiSDKCore_DIR`을
검증할 설치본의 `lib/cmake/iiSDKCore`로 지정한다.
