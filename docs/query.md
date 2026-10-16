# 객체 간 쿼리 통신

iiSDK에서 객체는 객체에게 쿼리를 전달해 통신한다. 쿼리는 GraphQL처럼 필요한 입력 값과 응답 필드만 명시하는 경량 메시지이며, 조회·명령·알림을 포함한다. 이 선택 원칙을 C++ 요청·응답 타입으로 표현한다. 메시지의 의미와 선택 가능한 필드는 제품/상위 모듈이 정의한다.

## 값 중심의 명시적 계약

통신의 주체가 객체여도 전달 데이터는 정수 하나, 식별자, 작은 값 구조체, 선택된 필드 묶음이면 충분하다. `QueryPort<int, int>`처럼 스칼라로도 통신한다. 포트 참조는 전달 경로이며 쿼리 payload에 송신·수신 객체가 자동으로 포함되지 않는다.

객체 전체의 값·스냅샷·참조는 필요할 때 명시적으로 선택하는 선택 사항이다. 아래 원칙을 적용한다.

- 입력은 작업에 필요한 인자만 포함한다.
- 고정된 조회는 최소 응답 타입을 정의한다. `ReadLevel → int`가 그 예이다.
- 동적으로 응답 필드를 선택할 때는 요청에 선택 필드를 명시하고, 응답에서 미선택 필드는 비워 둔다. 선택하지 않은 값은 읽기·계산·복사를 하지 않는다.
- 객체 참조나 전체 스냅샷은 별도 필드/쿼리로 명시적으로 요청한다. 기본적으로 생략한다.
- 부분 갱신에서는 필드 생략을 `std::optional` 등으로 구분한다. `0`, `false`, 빈 문자열도 명시적으로 전달 가능한 값이다. nullable 값의 null 설정과 생략이 모두 필요하면 각각 구분되는 도메인 타입을 정의한다.

실행 예제의 선택 계약은 다음과 같다.

```cpp
struct ReadOutput { bool level = false, revision = false; };
struct ReadAudio { ReadOutput output; };
struct ReadDevice { ReadAudio audio; bool name = false, object = false; };
struct OutputFields {
    std::optional<int> level;
    std::optional<std::size_t> revision;
};
struct AudioFields { std::optional<OutputFields> output; };
struct DeviceFields {
    std::optional<AudioFields> audio;
    std::optional<std::string> name;
    std::optional<std::reference_wrapper<const Device>> object;
};

auto fields = device.read.send(ReadDevice{.audio = {.output = {.level = true}}});
// audio.output.level만 존재한다. revision, name, object는 비어 있다.

auto whole = device.read.send(ReadDevice{.object = true});
// 명시적으로 요청했으므로 객체 참조만 포함한다.
```

수신 처리기는 선택된 필드만 응답에 채운다. QueryPort는 임의의 C++ 객체를 자동 탐색하거나 필드를 잘라내지 않는다. 선택 의미론은 타입이 정해진 도메인 처리기에서 구현한다. `std::optional` 응답 구조체는 각 필드의 고정 저장 공간을 갖지만, 미선택 필드의 값은 생성하지 않는다. 가변 크기 직렬화나 GraphQL 문자열 파서는 이 구현 범위에 포함하지 않는다.

실행 중 멤버 구성이 달라지는 객체는 [ObjectProperties](object-properties.md)의 PropertyQuery 처리기를 조합할 수 있다. 명시적으로 등록한 값·메서드·자식만 queryable/designable 정책으로 조회·변경하고, 선택 결과는 중첩 스냅샷으로 반환한다. 기존 고정 타입 포트의 동작은 유지하며, 동적 경로 검색이나 속성 컨테이너 사용을 모든 객체에 강제하지 않는다.

명시적으로 반환한 객체 참조는 복사가 아닌 빌린 참조이다. 수신 객체가 살아 있는 동안에만 사용한다. 값 응답은 수신 객체 수명과 분리할 수 있으므로 기본 통신 계약으로 우선한다.

## 인스턴스 구성

기본 애플리케이션 구성은 [Runtime/SerialQueue](parallel-runtime.md)이다. `QueryPort(queue, handler)`로 객체 실행 영역을 연결하고 `post(request, completion)`으로 비동기 전달한다. 같은 객체의 포트는 하나의 큐를 공유하고 서로 다른 객체 큐는 병렬 실행한다. 연결된 포트의 외부 send는 wrong_executor이며 다른 실행자로 우회 제출할 수 없다. 아래의 연결하지 않은 포트와 명시적인 EventLoop 전달은 저수준 사용법으로 유지한다.
객체는 [멤버 트리](member-tree.md)를 구성할 수 있다. 값, 일반 메서드, 쿼리 포트를 각 public/private 노드에 함께 배치한다. 아래는 하나의 작은 노드이며, 실행 예제에서는 `Device::Audio::Output`처럼 중첩한다. 부모 계층의 private 접근 제한을 포트가 자동으로 우회하지 않는다.

```cpp
struct SetLevel { int value; };
struct ReadLevel {};

class OutputNode final {
    int level_ = 0;
public:
    ii::runtime::QueryPort<SetLevel> set_level{
        [this](const SetLevel& query) { level_ = query.value; }
    };
    ii::runtime::QueryPort<ReadLevel, int> read_level{
        [this](const ReadLevel&) { return level_; }
    };
};
```

수신 객체는 포트를 소유하고, 송신 객체는 `QueryPort<SetLevel>&` 등의 참조를 전달받는다. 송신자는 수신 객체의 구체 클래스나 내부 메서드에 의존하지 않는다. 서로 다른 쿼리를 처리하는 객체는 여러 포트 인스턴스를 조합한다. 공통 부모 클래스, 가상 함수, 자동 리플렉션, 전역 버스, 이름 기반 라우팅은 필요하지 않다.

`QueryPort<Request, Response = void>`의 처리기는 `Response(const Request&)`이다. 요청 타입과 응답 타입은 컴파일 시 결정된다. `void`는 반환 데이터 없이 처리 완료 여부만 전달한다. 값 응답은 move-only 소유권 타입도 사용할 수 있다.

## 직접 전달과 큐 전달

`port.send(query)`는 호출 스레드에서 즉시 처리하고 `QueryReply<Response>`를 반환한다. 요청은 호출 중에만 빌려 쓰며, 처리기가 요청 참조를 저장해서는 안 된다.

`port.post(loop, query, completion, priority)`는 요청을 값으로 소유한 작업을 이벤트 루프에 제출한다. 큐 전달의 요청 타입은 기존 EventLoop의 복사 가능한 콜백 계약에 맞춰 copy-constructible이어야 한다. 요청 안의 참조·span·view까지 깊이 복사하는 것은 아니므로, 그런 필드가 가리키는 자원의 수명은 호출자가 보장한다.

`post`의 즉시 반환값 `Submission`은 큐 접수 결과이다. `full`, `stopped`, `invalid`는 즉시 호출자에게 반환하고 completion을 호출하지 않는다. 빈 completion도 `invalid`이다. 실제 처리 결과는 실행 스레드에서 completion에 전달한다. 수신 처리기와 completion은 같은 EventLoop 실행 스레드에서 실행하며, 응답을 송신 스레드로 자동 전환하지 않는다. 필요한 경우 송신 객체의 포트에 다시 쿼리를 제출한다.

`send`와 서로 다른 루프에서의 처리를 자동 직렬화하지 않는다. 객체 상태를 하나의 루프에서만 수정하거나 호출자가 동기화한다. 처리기 안에서 상대 객체에 동기 쿼리를 보내는 것은 일반 함수 재진입과 같은 계약이며, 순환 호출은 호출자가 피한다. 실행자를 막는 future 대기는 제공하지 않는다.

## 응답과 실패

`QueryReply<Response>`는 `std::expected<Response, QueryError>`이다.

- 성공: 값 응답 또는 `void` 완료.
- `receiver_expired`: 명시적으로 닫힌 포트에 `send`했다. 제거로 취소된 대기 쿼리는 completion을 호출하지 않는다.
- `handler_failed`: 수신 처리 또는 응답 값 생성에서 예외가 발생했다. 원래 `std::exception_ptr`를 `cause`에 보존한다.
- `wrong_executor`: 큐에 연결된 포트의 send를 해당 객체 실행 영역 밖에서 호출했다. 기본 비동기 post를 사용한다.

제품 수준의 거절은 응답 타입에 명시적으로 표현한다. 예를 들어 재고 부족, 잘못된 문서 상태 등은 도메인 응답이며 런타임 오류로 자동 변환하지 않는다. completion 자체의 예외는 QueryError로 재포장하지 않고 EventLoop의 예외 전파·정리 계약을 따른다.

## 수명과 취소

포트는 이동/복사하지 않는다. 각 포트의 TaskScope가 비동기 작업과 동기 send의 실행 수명을 관리한다. `close()`와 소멸은 새 제출을 막고 대기 쿼리를 취소하며, 실행 중 처리기·completion과 캡처 파괴 및 실행자 슬롯 반환을 기다린 뒤 처리기를 해제한다. 소멸한 포트가 있던 주소에 새 포트가 만들어져도 이전 작업은 새 수신자에게 전달되지 않는다.

포트 참조를 사용해 `send/post`를 호출할 때는 포트가 살아 있어야 한다. 살아 있는 포트의 close와 제출·처리는 동시 호출할 수 있다. 관련 상태 뒤에 포트를 선언하여 상태보다 먼저 파괴되게 하고, 소멸자 본문이 상태를 수정한다면 본문 시작에서 포트를 닫는다. 포트를 통하지 않는 직접 메서드·다른 큐 작업은 이 수명 경계에 포함되지 않으므로 호출자가 별도로 동기화한다.

처리기·completion·캡처 소멸자 안에서 같은 포트를 close하면 self_join이다. 브라우저 메인 스레드와 Runtime 워커에서 대기가 필요하면 would_block이다. 이때 제거 완료로 취급하지 않고 안전한 외부 실행 영역에서 다시 close한다. 안전하게 기다릴 수 없는 문맥에서 소멸자를 실행하면 terminate한다. 닫힌 포트의 post는 stopped이며, 올바른 실행 영역의 send는 receiver_expired이다. 자세한 순서와 조합 방법은 [제거 계약](cleanup.md)을 따른다.

completion이 캡처하는 송신 객체의 수명도 별도이다. 송신 객체가 먼저 사라질 수 있다면 토큰을 취소하거나 자체 수명 관리 정책을 적용한다. 포트의 약한 수신자 참조가 임의의 사용자 참조 캡처까지 보호하지는 않는다.

큐에 남은 쿼리를 `loop.cancel(submission.token)`로 취소하면 처리기와 completion을 모두 실행하지 않는다. 이미 선택되어 실행 중인 작업은 선점하지 못한다. 루프 종료도 미실행 쿼리의 completion을 자동 실행하지 않는다. 따라서 completion을 반드시 한 번 호출하는 RPC 보장은 없다.

## 메모리와 구현 경계

포트 생성 시 처리기와 수명 제어 상태를 확보한다. 작업마다 수명 추적 레코드와 요청·completion 캡처에 동적 할당을 사용할 수 있으며, 쿼리 전달 전체의 무할당을 보장하지 않는다. 작업 캡처는 완료·취소 때 해제하고, 실행자의 고정 슬롯은 자원 정리 이후 재사용한다. 수명 제어용 소유 포인터는 내부에만 있고 공개 연결 API는 참조이다.

QueryPort는 하나의 포트로 전달하는 타입 기반 통신이다. 지속 구독과 쿼리별 선택 방출은 [QueryStream](query-stream.md)이 QueryPort·TaskScope·기존 실행기를 조합하여 제공한다. 네트워크 직렬화와 분산 라우팅은 상위 모듈의 책임이다. 기존 EventLoop의 `Task`는 실행 단위로 유지하고, 객체 통신은 그 위의 QueryPort로 표현한다.

실행 가능한 예제: `examples/query.cpp`. 계약 테스트: `tests/query.cpp`. 설치 소비자도 `<iiSDKCore/Runtime/Query.h>`와 쿼리 응답을 검사한다.
