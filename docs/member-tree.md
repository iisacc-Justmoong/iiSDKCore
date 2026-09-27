# 객체의 멤버 트리

iiSDK 객체는 C++23 중첩 인스턴스로 타입이 유지되는 멤버 트리를 구성한다. 각 노드는 값, 일반 메서드, QueryPort, 다른 노드를 함께 소유할 수 있다. public과 private 모두 임의의 유한 깊이로 구성할 수 있으며 상속은 필요하지 않다.

이 문서는 표준 C++의 소유와 접근 제어로 구성하는 정적 멤버 트리의 계약이다. 기존 QueryPort는 어느 깊이의 노드에서도 사용할 수 있다. 실제 예제 구현은 `examples/objects/Device.h`, 사용자는 `examples/query.cpp`이다.

생성 이후에도 인스턴스의 멤버 정의를 추가·교체·삭제해야 하는 경우에는 [ObjectProperties](object-properties.md)를 함께 소유한다. 이 동적 트리도 값·메서드·자식과 public/private를 지원하며, queryable/designable 정책을 쿼리 처리 시 검증한다. 정적 C++ 멤버의 물리적 구조와 접근 제어를 자동으로 수정하지 않는다. 공통 Object 베이스 클래스나 전역 레지스트리는 요구하지 않는다.

## 구현된 객체 구조

```text
Device
├─ public audio
│  └─ public output
│     ├─ public limits
│     │  ├─ minimum, maximum             값
│     │  └─ contains(value)              일반 메서드
│     ├─ public level, revision          읽기 전용 값 참조
│     ├─ public reset()                  일반 메서드
│     ├─ public set_level                쿼리 메서드 포트
│     ├─ private apply(query)            일반 메서드
│     └─ private state_
│        ├─ level                       값
│        ├─ set_level(value)             메서드
│        └─ history
│           ├─ revision                 값
│           └─ record()                 메서드
├─ public read                          중첩 필드 조회 쿼리 포트
└─ private internal_
   └─ identity
      ├─ name                           값
      └─ read_name()                    메서드
```

`Device::Audio::Output`은 타입 이름의 계층이며, `device.audio.output`은 실제 객체가 소유하는 인스턴스 경로이다. 중첩 타입을 선언하는 것만으로 노드가 생성되지는 않으므로 각 타입의 인스턴스를 멤버로 소유한다. 서로 다른 Device는 별도의 값과 상태 트리를 갖는다.

## 값과 메서드 접근

```cpp
Device device;
int maximum = device.audio.output.limits.maximum;
bool accepted = device.audio.output.limits.contains(42);
auto changed = device.audio.output.set_level.send(SetLevel{42});
int current = device.audio.output.level;
device.audio.output.reset();
```

값의 가변성은 노드가 정한다. 예제의 level/revision은 private 저장 공간을 바라보는 const 참조이고, limits는 const 인스턴스이다. 쓰기는 공개된 일반 메서드나 쿼리 포트를 통한다. 다른 객체가 public 값 자체를 직접 수정할 수 있어야 하는 도메인은 그 값을 public 비const 멤버로 선언할 수 있다. 트리가 일괄적으로 읽기 전용 또는 공개 상태를 강제하지 않는다.

private 노드도 내부에서 `state_.history.record()`처럼 값과 메서드를 동일하게 사용한다. 예제의 private `apply()`는 private 상태 노드의 메서드를 호출한다. 내부 구현을 한 평면에 모을 필요가 없다.

## 접근 제어

접근 가능한 경로의 모든 단계가 접근을 허용해야 한다. `audio`가 public이어도 `audio.output.state_`가 private이면 외부에서 그 하위 값을 읽거나 메서드를 호출할 수 없다. private `internal_`의 Identity가 내부적으로 public 필드를 갖더라도 루트 외부로 노출되지 않는다.

중첩 클래스의 독립적인 private 권한도 C++ 규칙을 따른다. 부모 클래스라는 이유만으로 자식 클래스의 private 멤버에 접근하는 특권을 만들지 않는다. 노드 간 동작은 공개 메서드나 명시적 권한 관계로 연결한다. C++ 접근 제어는 컴파일 시의 캡슐화이며, 같은 프로세스 안의 임의 메모리 접근을 막는 보안 샌드박스는 아니다.

쿼리도 private 트리의 자동 탐색을 제공하지 않는다. 공개 `read` 처리기가 `name`을 선택한 경우에만 `internal_.identity.read_name()`의 결과를 값으로 반환한다. 이는 객체 작성자가 명시적으로 허용한 공개 투영이다. 객체 전체 참조를 명시적으로 반환해도 C++ private 접근 권한은 바뀌지 않는다.

## 쿼리 트리와 필요한 필드 선택

```cpp
auto fields = device.read.send({.audio = {.output = {.level = true}}});
// fields->audio->output->level에만 값이 존재한다.
```

요청·응답은 중첩 구조를 보존한다. 요청하지 않은 revision/name/object는 비어 있으며, 어떤 자식도 선택하지 않으면 audio/output 응답 노드도 생성하지 않는다. 식별자를 `audio_output_level`로 합치거나 dotted string 하나로 바꾸는 절차는 없다.

다른 객체에 전체 트리를 넘기지 않고 말단의 포트 참조만 전달할 수 있다. 예제의 Controller는 `device.audio.output.set_level`에 대한 `QueryPort<SetLevel>&`를 받아 필요한 요청 값 하나만 전달한다. EventLoop에 큐로 제출해도 동일한 말단 포트에서 실행된다.

## 수명과 비용

트리의 값 노드는 부모 객체의 저장 공간과 수명을 따른다. 노드마다 별도의 런타임 등록이나 heap 할당을 하지 않는다. std::string 및 QueryPort 같은 개별 멤버의 할당 정책은 기존 계약을 따른다. 노드의 일반 값 구조가 복사/이동 가능한지는 그 멤버 타입이 결정하며, QueryPort가 있는 예제 Device는 복사/이동하지 않아 내부 참조와 this 캡처가 유지된다.

부모 객체가 소멸하면 자식 노드와 그 포트도 소멸한다. 아직 실행하지 않은 쿼리는 기존 약한 수신자 참조 계약에 따라 `receiver_expired`가 된다. 빌린 값 참조·노드 참조의 수명은 부모보다 길 수 없으며, 실행 중 처리기와 부모의 동시 소멸은 호출자가 동기화해야 한다.

## 검증

`tests/member_tree.cpp`는 public 값·일반 메서드·쿼리 메서드의 중첩 접근을 검사하고, private 루트/자식/손자 경로와 private 메서드 접근이 허용되지 않음을 C++ requires/static_assert로 검사한다. 같은 컴파일 단위에서 public 접근도 성립함을 확인한다.

실행 검사는 인스턴스 간 상태 분리, private 하위 메서드를 통한 값 갱신, 중첩 선택·응답, 미선택 하위 트리 생략, 비동기 말단 쿼리, 실패 시 상태 보존, 부모 소멸 후 자식 포트 만료를 포함한다.
