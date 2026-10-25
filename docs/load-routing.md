# Application·Classic 로드 라우팅과 비동기 예약

Core 0.19.0은 기존 계산 작업 분산에 Application(L7) 요청 선택과 Classic의
리스너별 분배 정책을 추가한다. Network(L4)의 IP·포트·전송 프로토콜과 백엔드 주소
관리는 독립 상위 모듈 iiNetwork의 `LoadBalancer.h`가 소유한다. Core는 iiNetwork를
include·링크·검색하지 않는다. 세 유형은 같은 bounded 대상 표·건강 상태·자원 예약과
취소·드레이닝 계약을 공유하며 별도 워커나 관리 스레드를 만들지 않는다.

이름은 AWS의 분류와 요구사항을 참고한 것이다. AWS 서비스나 API 호환 구현이 아니다.
실제 HTTP 파싱·소켓 전달·TLS 종료·DNS·배포·서버 생성은 해당 네트워크/호스트 어댑터가
담당한다. Core에 전달하는 것은 정규화한 요청 메타데이터와 실행기 참조이다.

## 대상 그룹과 분배 정책

`LoadTargetOptions.group`과 `LoadRequest.group`은 64비트 애플리케이션 지정 그룹이다.
기본 0은 이전 계산 분산과 같은 단일 그룹이다. 하나의 요청은 같은 그룹의 정상·접수
가능 대상만 선택한다. 그룹마다 독립적인 순환 위치를 유지하며 다른 그룹으로 넘기지
않는다. 그룹 표도 `max_targets`로 제한되고 대상 등록/제거 때 슬롯을 재사용한다.
한 실행기를 여러 그룹에 등록하면 예약량은 등록별로 독립적이다. 공유 실행기 자체의
실제 큐 한도는 여전히 해당 실행기가 강제한다.

| 정책 | 선택 기준 |
|---|---|
| `round_robin` | 접수 가능한 대상을 그룹 내 순서대로 선택 |
| `least_loaded` | 기존 `(현재 units + 요청 units) / capacity` 최소화 |
| `least_outstanding` | 현재 접수 후 미완료인 **논리 요청 수** 최소화, 동률 순환 |
| `flow_hash` | 명시한 64비트 affinity 키에 대해 capacity 가중 rendezvous 선택 |

기본 일반 LoadBalancer 정책은 이전과 같은 `least_loaded`이다. `LoadRequest.policy`로
요청별 정책을 명시할 수 있다. 모든 정책에 건강·드레이닝·기능·신고 메모리·용량 제한을
먼저 적용한다. `flow_hash`에 affinity가 없거나 유효하지 않은 정책이면 `invalid`이다.
해시 가중치는 대상의 선언한 capacity이며 자동 CPU 측정값이 아니다. 매우 큰 부동소수점
비율의 동률은 기존과 같이 순환 순서로 처리한다.

## Application 요청 라우팅

`<iiSDKCore/Runtime/LoadRouting.h>`의 `ApplicationLoadBalancer`는 다음을 제공한다.

- 낮은 priority부터 규칙을 평가하고, 첫 일치 규칙의 그룹을 선택한다.
- host는 ASCII 대소문자를 구분하지 않는 `*`, `?` glob, path는 대소문자를 구분하는
  바이트 접두사, method는 정확한 값으로 비교한다.
- header 이름은 ASCII 대소문자를 구분하지 않고 값은 정확히 비교한다. query 이름과
  값은 정확히 비교한다. 규칙의 조건은 모두 만족해야 하며 반복 필드는 하나라도 맞으면 된다.
- 일치 규칙이 없을 때만 기본 그룹을 사용한다. 일치 그룹이 포화·비정상이어도 다른
  서비스 그룹으로 요청을 넘기지 않고 `overloaded`로 반환한다.
- 기본 정책은 `least_outstanding`이다. 처리 시간이 다른 백엔드에서 진행 중 요청이
  적은 대상에 새 요청을 보내 긴 작업 뒤에 요청이 쌓이는 현상을 줄인다.

어댑터는 host의 포트 제거와 국제화 도메인 정규화, path와 query 분리, URL 디코딩,
신뢰할 수 있는 프록시의 메타데이터 처리를 소유한다. Core의 path prefix는 정규식이나
AWS path-pattern 문법이 아니며, `/api`는 `/apix`에도 일치한다. 경로 단위 분리가
필요하면 `/api/` 또는 해당 어댑터의 정규화 규칙을 사용한다. NUL·CR·LF와 `/`로
시작하지 않는 요청 path는 거절한다.

`configure(routes, default_group)`는 규칙을 검증·정렬한 불변 스냅샷을 한 번에
교체한다. 중복/0 priority, 조건 초과, 잘못된 prefix는 기존 설정을 바꾸지 않는다.
읽기는 짧은 공유 잠금으로 스냅샷 소유권만 복사하고 잠금 밖에서 일치 검사를 한다.
host와 host 패턴은 각각 253바이트로 제한하여 glob의 재검사 비용도 제한한다.
설정과 요청에는 규칙 수·조건 수·메타데이터 바이트·필드 수 한도가 적용된다. 임의
메타데이터나 규칙을 무제한 축적하지 않는다.

```cpp
ApplicationLoadBalancer routing;
auto target = routing.targets().add_target(worker, {.capacity=32, .group=10});
auto configured = routing.configure({
    {.priority=1, .group=10, .host="api.example.com", .path_prefix="/v1/"}
}, 20);
auto request = routing.acquire({.host="api.example.com", .path="/v1/jobs"});
if (request) {
    // request->target()로 어댑터의 실제 백엔드를 찾는다.
    // 응답·스트림·자원을 정리한 다음 완료한다.
    request->complete();
}
```

`submit(metadata, task, resources)`는 동기 콜백의 끝을 요청 완료로 취급한다.
콜백이 별도 비동기 I/O만 시작한다면 `acquire()`의 예약을 응답 완료까지 소유한다.
메타데이터 오류처럼 공통 대상 선택 이전에 거절한 요청은 대상별 통계에 포함하지 않는다.

## Classic 정책

`ClassicLoadBalancer`는 TCP·TLS 리스너에 `round_robin`, HTTP·HTTPS 리스너에
`least_outstanding`을 적용한다. `acquire(protocol)`는 비동기 연결/응답 예약,
`submit(protocol, task)`는 동기 작업용이다. 기존 연결의 토큰을 패킷마다 다시
획득하지 않는다. Classic의 프록시 연결 재사용·쿠키·TLS 구현까지 제공하는 객체는 아니다.

## 비동기 수명과 취소

`LoadBalancer::acquire()`는 move-only `LoadLease`를 반환한다. 같은 대상에 후속 작업을
전달하는 `post()`도 제공하여 TaskExecutor 계약과 조합할 수 있다. 대상 건강이나
드레이닝이 바뀌어도 이미 획득한 예약의 대상은 바뀌지 않는다. 대상 범위가 취소/종료되면
후속 post는 거절되며 `stop_token()`으로 실제 I/O 소유자에게 협력적 종료를 요청한다.

`complete()`는 성공, `fail(exception_ptr={})`는 실패, `cancel()`과 미완료 예약의
소멸은 취소이다. 같은 논리 요청의 통계는 한 번만 확정된다. post한 콜백이 남아 있으면
complete 이후에도 그 콜백과 캡처가 제거될 때까지 예약을 보유한다. 콜백의 예외를 뒤의
complete로 성공 처리할 수 없다. 예외 포인터 없는 전송 실패도 `work_failed`로 관측된다.
예약 값 자체의 post·이동·완료를 서로 경쟁시키지 않는다. 다른 예약들은 동시 사용 가능하다.

자원 반환보다 실제 전송/응답 객체의 정리가 먼저 끝나야 한다. 외부 예약이 살아 있는
remove/close는 대상을 접수 중단한 상태로 유지하고 `would_block`을 반환한다.
cancel_pending은 모든 대상의 stop 요청과 대기 콜백 취소를 먼저 수행한다. 호출자는
외부 전송을 닫고 예약을 반환한 뒤 remove/close를 다시 호출하여 최종 정리를 완료한다.
동일 호출자가 보유한 연결을 기다리며 교착하지 않는다. 소멸 전에 이 장벽을 완료해야 한다.
일반 `wait_idle()`은 기존처럼 모든 예약 반환을 기다리므로 자신이 보유한 예약을 먼저 반환한다.

`stats().reservations`와 대상별 동일 필드는 살아 있는 외부 요청/연결 수를 표시한다.
이들은 in_flight·units·신고 메모리와 전체 capacity에 포함된다. 예약 내부의 후속 post는
새 논리 요청으로 세지 않으며, 실행기 자체의 bounded 큐가 후속 콜백 수를 제한한다.

0.19.0은 공개 options/request/stats 레코드를 확장하므로 이전 바이너리 소비자는 재빌드한다.
기존 계산 분산 기본 정책과 post/submit 호출 형태는 유지한다.

## 검증과 참고

`tests/load_routing.cpp`는 그룹 격리, 독립 순환, 조건·설정 교체, 실패 보존, 취소·드레이닝,
캡처 선행 제거와 Classic 프로토콜별 정책을 검사한다. 512회 입력에서 한 대상은 매 tick,
다른 대상은 8 tick마다 처리하는 결정적 서비스 시뮬레이션으로 순환 대비 최소 미완료
요청의 대기량을 비교한다. tick 결과는 물리 서버 처리량이나 네트워크 RTT 측정이 아니다.
실행 예제는 `example/load_routing.cpp`, 실제 TCP·UDP와 flow 선택 검사는 iiNetwork에 있다.

요구사항 참고 자료는 [AWS listener rules](https://docs.aws.amazon.com/elasticloadbalancing/latest/application/listener-rules.html),
[분배 알고리즘](https://docs.aws.amazon.com/elasticloadbalancing/latest/application/edit-target-group-attributes.html),
[유형별 분배 계약](https://docs.aws.amazon.com/elasticloadbalancing/latest/userguide/how-elastic-load-balancing-works.html)이다.

### 2026-10-25 검증 기록

| 범위 | 결과 |
|---|---|
| macOS arm64 Release 전체 빌드·CTest | 60/60 통과 |
| 작업 공간 설치 패키지 소비자 | 22/22 통과 |
| ASan/UBSan 기존 계산 분산·새 라우팅 | 2/2 통과 |
| TSan 기존 계산 분산·새 라우팅 | 2/2 통과, 경합 보고 없음 |
| 변경 구현의 C++23 엄격 경고 | `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror` 통과 |
| Windows x64·Linux arm64·WASM32 pthread 변경 구현 문법 검사 | 3개 구현 × 3개 대상, 9/9 통과 |

512 tick의 결정적 서비스 비교에서 round_robin은 최대 점유 65·남은 요청 64·완료 448,
least_outstanding은 최대 점유 2·남은 요청 1·완료 511이었다. 요청은 같은 입력률로
들어오고 빠른 대상은 매 tick, 느린 대상은 8 tick마다 한 요청을 완료한다. 검사 끝에
남은 모든 예약도 반환하여 총 512개 논리 요청을 정리한다. 이것은 정책의 대기량 비교이며
물리 서버의 지연·처리량·CPU 개선 비율이 아니다.

XML·빌드/설치 로그·패키지는 `build/load-routing-20261025/`에 있다. 설치 검증 위치는
그 안의 `stage/iiSDKCore`이며 기본 `~/.local` 설치본과 실행 앱은 교체하지 않았다.
공개 레코드를 확장한 0.19 소비자는 재빌드해야 한다.
교차 검사는 `-fsyntax-only`이며 링크나 대상 장치 실행이 아니다. Windows는 묶음 Clang의
시작 지연으로 Homebrew Clang과 LLVM-MinGW 헤더를 사용했다. 구체 명령·초기 도구 실패와
최종 결과는 같은 폴더의 `cross-syntax.json`과 로그에 보존한다.
