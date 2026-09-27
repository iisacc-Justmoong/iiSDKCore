# 기반 객체와 모듈 경계

빈 `src/` 하위 폴더 18곳에 런타임과 함께 사용할 최소 객체를 구현했다. 공개 헤더와 구현 파일의 명세는 `cmake/Foundations.cmake`이다. 모든 객체는 인스턴스·값·명시적 참조로 조합하며 Qt와 외부 SDL/Vulkan 라이브러리를 연결하지 않는다. 표준 컨테이너를 별도 iiSDK 컨테이너로 대체하지 않는다.

## 폴더별 책임

| 폴더 | 공개 객체 | 제공하는 계약 |
|---|---|---|
| API | `ii::api::Services` | 애플리케이션 인스턴스별 타입 지정 서비스 참조 등록·검색·해제 |
| Core/Assert | `ii::core::Contract` | 릴리스에서도 동작하는 조건 검사와 인스턴스별 위반 보고, 소스 위치 |
| Core/Event | `Event<T>`, `EventSequence` | 값 payload, 순번, 단조 시각을 가진 이벤트 봉투 |
| Core/File | `File` | 정규화된 파일 경로와 공유 소유 `io::Channel`의 결합 |
| Core/IO | `ByteReader`, `ByteWriter` | span 위의 정수 바이트 인코딩·디코딩, 범위·엔디언 검사 |
| Core/Path | `PathRoot` | 절대 루트와 상대 리소스 경로의 결합, 어휘적 상위 경로 이탈 차단 |
| Core/Platform | `PlatformProfile` | 운영체제·CPU·포인터 크기·바이트 순서·스레드/I/O 지원 조회 |
| Core/Property | `PropertyLens` | 멤버 트리 경로를 재사용하는 선택 조회·쓰기·호출·설명 쿼리 생성 |
| Core/Result | `Result<T>`, `Error` | expected 기반 결과, 오류 코드·메시지·원인 예외 |
| Core/Task | `TaskGroup` | 기존 Runtime에 제출한 작업 묶음의 봉인·취소·비차단 완료 관찰 |
| Core/Time | `Deadline` | 단조 시계의 만료·남은 시간, 무기한과 오버플로 처리 |
| Core/Version | `Version` | uint32 범위 major.minor.patch 파싱과 숫자 순서 비교 |
| Debug | `ii::debug::Diagnostics` | 제한 용량 진단 스트림, 오래된 기록 폐기와 통계 |
| Graph | `ii::graph::DependencyGraph` | 고정 노드 슬롯, 세대 식별자, 유향 비순환 의존 관계와 위상 순서 |
| Scene | `ii::scene::Scene` | 직렬 실행 영역에 속하는 인스턴스 속성 계층, 부모 변경·하위 트리 제거 |
| Scripting | `ii::scripting::Bindings` | 동적 값 인자·응답을 가진 네이티브 함수의 쿼리 호출과 인스턴스별 변경 |
| SDL | `ii::sdl::HostSession` | 주입된 호스트 초기화·이벤트 펌프·종료 함수와 EventLoop의 결합 |
| Vulkan | `ii::vulkan::Timeline` | 제출 식별 값 발급과 주입된 백엔드 완료 카운터의 관찰 |

`Core/IO`는 바이트 표현을 다룬다. 실제 파일·시리얼 접근과 비동기 작업은 `IO/Channel`과 `IO/AsyncIO`가 책임진다. `Core/File`은 그 위에 경로 정보를 결합한 객체이다. 이미 존재하는 이벤트 실행·통신·스레드 기능을 다른 이벤트 시스템으로 중복 구현하지 않는다.

## 수명과 실행 영역

- `Services`의 등록 테이블은 내부 잠금을 사용하지만 서비스 자체의 상태를 동기화하거나 수명을 연장하지 않는다. 참조 대상은 등록 및 참조 사용 기간보다 오래 살아야 한다. `const T`와 `T` 등록은 별개이며 중복 타입의 암묵적 교체는 없다. 전역 싱글턴이 아니다.
- `Contract` 기본 보고자는 `logic_error`를 던진다. 보고자를 주입하면 실패를 보고한 뒤 false를 반환한다. 보고자 콜백의 스레드 안전성은 호출자가 보장한다.
- `EventSequence`의 순번은 한 인스턴스 내의 원자적 증가 값이다. 생산자 여러 개의 실제 큐 도착 순서나 분산 시계 순서를 보장하지 않는다.
- `File` 복사본은 같은 Channel의 커서와 종료 상태를 공유한다. I/O의 부분 전송·시간 초과·취소·플랫폼 오류는 기존 [I/O 계약](io.md)을 따른다.
- `ByteReader`와 `ByteWriter`는 버퍼를 빌린다. 버퍼 수명·동시 접근은 호출자 책임이다. 실패 시 위치와 출력 버퍼를 변경하지 않는다. 기본 순서는 little endian이며 프로토콜에 맞춰 big endian을 지정할 수 있다.
- `PathRoot`는 NUL·절대 입력·정규화 후 루트 이탈을 거부한다. **심볼릭 링크, 디렉터리 교체 경쟁 또는 OS 권한을 통제하는 보안 샌드박스가 아니다.**
- `PropertyLens`는 경로만 보관한다. 공개/비공개·queryable·designable·writable 정책, 값 검증, 실행 영역 검사는 수신 `ObjectProperties`가 수행한다. 조상 노드의 불필요한 값은 조회하지 않는다.
- `TaskGroup`은 Runtime을 빌리며 워커를 만들지 않는다. `close()` 이후 새 작업을 거절하고, 모든 승인 작업이 끝나야 `result()`에 결과가 생긴다. 취소 토큰으로 개별 작업이 제거되거나 Runtime이 `cancel_pending`으로 종료해도 작업 수가 정리된다. 수락되지 않은 제출은 묶음의 실패가 아니다. `cancel()`은 봉인과 미시작 작업 생략을 요청하며 이미 실행 중인 콜백을 선점하지 않는다. 실행 예외가 취소보다 우선 보고된다. 객체 소멸은 암묵적인 join/cancel이 아니며 이미 제출한 콜백은 계속 실행될 수 있다. 호출자가 캡처한 자원은 그때까지 유효해야 한다.
- `DependencyGraph`는 내부에서 잠그고 값 스냅샷을 반환한다. 다른 그래프의 NodeId와 제거 후 오래된 NodeId를 거부한다. 제거한 슬롯을 재사용하되 세대를 증가시킨다. 노드 수는 생성 용량에 제한되며 간선 및 순회 결과에는 동적 할당을 사용한다.
- `Scene`의 생성은 워커 시작 전에 가능하다. 이후 모든 멤버 접근은 지정 `SerialQueue` 안에서 수행한다. 부모 순환을 거부하고 실패한 부모 변경은 기존 관계를 보존한다. `properties()`가 반환한 참조는 해당 노드 삭제 전, 같은 실행 영역에서만 유효하다. 활성 속성 호출 중 그 속성 자신을 제거하지 말고 다음 큐 작업으로 삭제를 예약한다. 삭제 시 먼저 전체 하위 계층을 분리하고 사용자 속성 값을 소멸시킨다. Scene은 진행 중인 콜백을 기다리는 객체가 아니므로 큐 작업을 종료한 뒤 파괴해야 한다.
- `Bindings`는 초기 정의 이후 등록·교체·삭제를 같은 SerialQueue 안에서 수행한다. 외부에서는 `queries().post(Call{...}, reply)`를 사용한다. 콜백의 자기 삭제/교체 중에도 현재 실행 함수는 살아 있다. 반환형 `CallResult`가 네이티브 함수 오류를, 바깥 `QueryReply`가 쿼리 전달 오류를 표현한다. Runtime·SerialQueue 및 캡처한 서비스의 수명은 완료 콜백까지 보장해야 한다.

## 호스트와 그래픽 경계의 실제 범위

`HostSession`은 주입한 세 함수가 실제 백엔드 동작을 수행해야 한다. 생성 스레드에서 open/step/close를 호출하고 네이티브 호스트 초기화 이후 `step()`으로 이벤트 펌프와 iiSDK 이벤트 루프를 함께 진행한다. 재진입과 다른 스레드 접근은 오류이다. pump의 bool은 계속 실행할지 나타내며 false가 자동 close를 뜻하지 않는다. 초기화 실패의 부분 자원 정리는 초기화 함수가 책임진다. close는 한 번만 종료 함수를 실행하며 실패 시에도 세션은 비활성화된다. 명시적 close에서 오류를 확인해야 하며 소멸자의 종료 오류는 반환할 수 없다. 활성 세션을 다른 스레드에서 파괴하면 terminate한다.

`Timeline::issue()`는 애플리케이션이 백엔드에 제출할 단조 증가 값을 발급한다. **issue가 GPU 제출·signal·완료를 수행하지 않는다.** 호스트는 같은 값으로 실제 timeline semaphore를 신호하도록 제출하고, Counter 콜백에서 실제 드라이버 값을 읽는다. 시작 카운터 0, 이 인스턴스가 발급한 값만 사용하는 단일 타임라인을 전제로 한다. poll은 동시/재진입 실행을 busy로 거절하며, 역행하거나 발급 범위를 넘은 결과는 기존 완료 상태를 덮어쓰지 않는다. ready는 마지막 성공한 poll의 캐시를 읽고 GPU를 기다리지 않는다.

SDL 라이브러리 로딩·창/입력 구현, Vulkan 인스턴스·디바이스·렌더러, 스크립트 언어 파서·VM은 이 변경에 포함하지 않는다. 단위 테스트의 주입 함수는 경계의 계약을 검증하며 실제 SDL 창이나 GPU 장치 실행을 증명하지 않는다. Scene에도 렌더링·좌표계·게임 규칙이 없다.

## 실행 예제와 검증

`examples/foundations.cpp`는 2바이트 센서 값을 읽어 네이티브 스크립트 바인딩에 필요한 값 하나만 비동기로 전달하고, Scene 인스턴스의 `sensor.reading`을 갱신한다. 완료 후 제한 용량 진단 기록을 읽는다.

```sh
cmake -S . -B build
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/iiSDKCoreFoundationsExample
```

기본 병렬 구성의 기대 출력은 `foundation pipeline complete: sensor.reading=42`이다. WASM 단일 스레드 구성에서는 병렬 예제를 실행할 수 없음을 출력하며, 기초 값·그래프·I/O 계약은 별도로 검사한다.

`tests/foundations.cpp`는 18개 폴더의 동작과 실패 경로를 함께 검사한다. 각 공개 헤더는 별도 번역 단위로 컴파일해 include 순서 의존성을 검사한다. 설치 소비자는 같은 계약 테스트를 설치 헤더·라이브러리만으로 다시 빌드하고 실행한다. 실제 플랫폼별 실행/교차 빌드 범위는 [검증 기록](validation.md)에 기록한다.
