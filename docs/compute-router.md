# 동적 가용 영역·범위와 컴퓨팅 프로세스 라우팅

Core 0.20의 `Runtime/ComputeRouter.h`는 Runtime, LoadBalancer/LoadLease,
TaskScope, OperationOwner와 네이티브 Process를 조합한다. 실행 자원은 참조로 등록하며
별도 워커·관리 스레드·네트워크 의존성을 만들지 않는다. 가용 영역은 등록한 로컬 Runtime
풀의 논리 영역이다. 실제 클라우드 영역 생성·VM 증설·원격 프로세스 이주·CPU affinity
설정은 수행하지 않는다. IP·연결 라우팅은 iiNetwork가 계속 소유한다.

## 동적 할당과 한도

`AvailabilityZone`과 `ComputeScope`는 제거 후 재사용하지 않는 불투명 식별자이다.
다른 인스턴스나 제거한 식별자로 자원을 조작할 수 없다. 영역·범위·대상 표는
ComputeRouterOptions의 상한으로 확보한다. 영역/범위 이름은 중복 없는 1~256 UTF-8
바이트이다. 대상 이름은 비워 둘 수 있다. NUL과 잘못된 UTF-8은 거절한다.

- add_zone/configure_zone/remove_zone으로 영역의 접수 여부와 공유 예산을 관리한다.
- add_scope/configure_scope/remove_scope으로 실행 가능한 영역 집합과 공유 예산을
  관리한다. 빈 zones는 이후 추가할 영역도 포함하는 전체 범위이다. 기본 범위는 제거할 수 없다.
- add_target(runtime, zone)은 실제 Runtime을 한 번만 등록한다. load.capacity=0이면
  Runtime의 큐 용량을 사용한다. assign_target은 신규 작업의 할당 영역을 바꾼다.
- enable_target은 신규 배치를 일시 중지/재개한다. 제거 중인 대상은 되살리지 않는다.

기존 예약은 접수 당시 영역·범위의 할당량을 보유한다. 대상 이동·범위 축소가 기존
작업을 취소하거나 이주시키지 않는다. 예약, 대상 또는 명시적인 범위가 참조하는 영역은
먼저 그 사용을 끝내야 제거할 수 있다. ComputeLimits.jobs/units/memory_bytes의 0은
해당 차원의 별도 한도를 해제하며 전체 작업 수는 항상 router.capacity에 제한된다.
한도를 현재 사용보다 낮출 수 있다. overcommitted로 관측하고 점유가 낮아질 때까지
추가 접수를 막는다. 그룹·capability·신고 메모리·건강 조건도 동시에 적용한다.

```cpp
Runtime ui({.workers=2, .capacity=16}), batch({.workers=4, .capacity=32});
ComputeRouter compute;
auto a = compute.add_zone({.name="interactive"}).value();
auto b = compute.add_zone({.name="batch"}).value();
auto first = compute.add_target(ui, a).value();
auto second = compute.add_target(batch, b).value();
auto scope = compute.add_scope({.name="export", .zones={b}, .limits={.jobs=16}}).value();
auto admitted = compute.submit(scope, [] { /* 독립적인 계산 */ });
auto changed = compute.configure_scope(scope, {.name="export"});
auto drained = compute.close(ShutdownMode::drain);
```

preferred_zone은 범위 안의 선호 영역이다. 자격을 만족하는 선호 대상이 없을 때만
cross_zone=true에서 같은 범위의 다른 영역을 사용한다. 범위 밖 영역을 지정하면
invalid이다. 잘못된/오래된 식별자는 invalid, 사용 중 제거는 overloaded, 표/전체 접수
포화는 full, 종료 뒤 변경/접수는 stopped이다. 설정 검증 실패는 기존 할당을 보존한다.
cross_zone은 preferred_zone이 있는 요청에 적용하며 선호 영역을 생략하면 범위 전체에서 선택한다.

## 실제 처리 현황과 라우팅

monitor()는 Runtime의 실제 workers·pending·active·executed·failed·cancelled와
영역/범위/대상별 예약을 복사한다. Router 외부에서 Runtime에 직접 넣은 작업도 포함한다.
라우팅 직전에도 같은 수집을 수행한다. 자격·예산을 만족하는 대상에서
`(max(Runtime pending, 예약 units) + 요청 units) / workers`를 비교하고, 신선한 CPU와
메모리 관측이 있으면 남은 자원 여유로 보정한다. 정지·실제 큐 포화·비정상·드레이닝과
CPU ceiling 이상인 대상은 제외한다. 최소 압력 후보 안에서 기존 LoadRequest 정책으로
최종 대상과 예약을 확정한다. 명시적으로 빈 후보 집합은 다른 대상으로 우회하지 않는다.
관측 직후 다른 소비자가 큐를 채울 수 있다. 실제 실행기의 거절은 그대로 전달하고 예약을
회수하며 사용자 작업을 다른 실행기로 자동 재실행하지 않는다.

CPU 사용률이나 RSS를 큐 통계로부터 추측하지 않는다. 실제 측정기는 report_load로
ComputeObservation을 공급한다. cpu_utilization은 선택 값이며 유한한 0~1만 허용한다.
external_memory_bytes는 **Router 예약 외부에서 사용한 메모리**이며 신고 예약량과
합산한다. 전체 RSS를 그대로 넣으면 예약과 중복될 수 있다. 샘플은 단조 timestamp와
valid_for를 가지며 미래·역순·잘못된 입력은 거절한다.

마지막 값과 observation_fresh를 함께 보존한다. require_observation 대상은 누락·만료 시
접수하지 않는다. 선택 관측이 만료되면 해당 CPU/메모리 보정을 적용하지 않고 실제 Runtime
정보로 판단한다. 값을 0이나 정상으로 위장하지 않는다. runtime_sampled_at은 실제 표본
시각이다. 전체 종료 뒤에는 Runtime 참조를 끊고 마지막 표본을 보존한다.

대상별 active_callbacks·callbacks_started/finished·queue_time·execution_time과
active_processes도 제공한다. 시간은 단조 시계의 누적 대기/콜백 벽시계 시간이다.
CPU 실행 시간이나 네트워크 RTT가 아니다. 무제한 샘플 이력 대신 누적 카운터를 사용한다.
revision은 설정 변경 버전, monitor_samples는 수집 횟수이다.

start_monitoring(loop, interval)은 기존 EventLoop의 bounded 주기 예약 하나를 사용한다.
호스트가 loop를 pump해야 한다. stop_monitoring은 타이머와 활성 수집을 정리하며 다시
시작할 수 있다. 종료도 자동 중지한다. 잠금 안에서 호출하는 Runtime.stats는 Core의
콜백 없는 함수이며 사용자 작업·캡처 파괴·프로세스 I/O는 잠금 밖에서 실행한다.

## 예약과 종료

ComputeLease는 접수 당시 placement를 보존하는 move-only 예약이다. 후속 콜백과 캡처가
끝나기 전까지 영역·범위·대상 할당량을 보유한다. complete/fail/cancel 및 미완료 소멸은
성공/실패/취소이며 실패를 뒤의 완료로 덮을 수 없다. 같은 값의 이동/완료/post를 경쟁시키지
않는다. 서로 다른 예약은 동시 사용 가능하다. submit/post는 실제 콜백 성공 뒤에만 완료로
계산하며 실행 전 취소를 성공으로 세지 않는다.

외부 acquire 예약은 소유자가 실제 자원을 먼저 정리한다. 외부 예약이 남은 remove/close는
would_block을 반환하고 접수 중단을 유지한다. 예약 반환 후 종료 장벽을 재호출한다.
자기 작업/캡처에서의 종료는 self_join, 겹친 종료·대상 제거는 busy이다. Runtime은 성공한
대상 제거 또는 전체 종료까지 살아 있어야 한다. 종료 뒤 monitor는 마지막 표본만 읽는다.

## 실제 자식 프로세스

route_process(scope, Launch, input, CommunicationOptions, ComputeRequest)는 자원을
예약한 뒤 선택한 기존 워커에서 Process.start/communicate를 실행한다. Launch·입력·
콜백은 작업이 소유하며 셸을 거치지 않는다. RoutedProcess는 placement, 진단용 native_id,
Operation<Communication>의 상태·결과·취소·완료 전달을 제공한다.

Operation 취소, Router 소유자 종료, 대상 취소와 communication.stop을 합친 협력적 중단
경계를 사용한다. 마감도 같은 단조 시간축이다. 자식 트리를 중단하고 direct child를 reap한
뒤 예약을 반환한다. 정상 exit 0은 완료, 비정상 exit/신호·spawn/I/O 오류는 실패,
취소·마감·소유자 제거는 취소 카운터이다.

Operation 성공은 Communication 결과 확보를 뜻하므로 자식 exit.code/reason도 확인한다.
취소한 자식의 부분 출력도 값으로 보존할 수 있다. owner_removed는 소유자 종료를 구분한다.
실제 launch 뒤 SideEffects는 possible이며 외부 효과를 롤백하지 않는다. PID만으로
취소하지 않는다. launch 없는 플랫폼은 기존 Process의 unsupported 결과를 보존한다.
실행 중 프로세스를 다른 호스트로 전송하는 RPC는 제공하지 않는다.

close(cancel_pending)은 OperationOwner와 일반 작업 범위의 중단을 먼저 요청한 뒤 대상
취소·실행·캡처 장벽을 완료한다. close(drain)은 외부 예약을 확인하고 소유한 내부 작업과
자식 프로세스 완료를 기다린다. 다른 소비자의 Runtime 작업은 취소하지 않는다.
work_failed는 실패가 기록된 정리 완료 결과이다. 소멸 전 다른 종료 오류를 해결해야 한다.

실행 예제는 example/compute_router.cpp, 동적 할당·한도 축소·실제 워커 관측·타이머·
동시 재설정·자식 프로세스 출력/중단 검사는 tests/compute_router.cpp이다.

## 2026-10-25 검증

| 범위 | 결과 |
|---|---|
| macOS arm64 Release 전체 빌드·CTest | 62/62 통과 |
| 설치한 Core 0.20 패키지 소비자 | 23/23 통과 |
| 기존 iiNetwork 0.6 패키지 + Core 0.20 소비자 | 31/31 통과 |
| ASan/UBSan ComputeRouter·LoadBalancer·LoadRouting·Operation | 4/4 통과 |
| TSan 같은 소유·라우팅 계약 | 4/4 통과, 경합 보고 없음 |
| Windows x64·Linux arm64·WASM32 pthread 엄격 문법 검사 | 구현·기존 분산기·검사·예제 4종 × 3대상, 12/12 통과 |

실제 Runtime에 넣은 외부 작업이 바쁜 동안 다른 풀을 선택하는지, 영역 이동·범위 변경
전후의 예약이 원래 예산으로 반환되는지, 한도 축소의 overcommit 표시와 만료 관측 배제를
검사했다. 세 생산자의 900개 예약을 관측·대상 이동과 동시에 완료했다. 실제 자식의 출력,
Operation 취소, 마감, 소유자 제거, spawn 실패의 결과와 최종 자원 반환도 확인했다.

교차 검사는 -fsyntax-only이며 링크나 대상 기기 실행이 아니다. 설치 검증 패키지는
build/compute-router-20261025/stage/iiSDKCore, XML·로그와 최종 verification.json은
build/compute-router-20261025에 둔다. iiNetwork의 로컬 소켓 fixture는 macOS의 경로
길이 제한을 넘지 않도록 해당 저장소 build/cr20/build에서 검증했다. Network 소스나
프로토콜의 검사를 완화하지 않았다. 기본 설치본과 실행 중 앱은 교체하지 않았다.
