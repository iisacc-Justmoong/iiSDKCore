# Queryable and designable object properties

`ObjectProperties`는 객체가 private 멤버로 소유하는 **인스턴스별 동적 멤버 트리**이다. 생성자에 초기 정의를 전달하고, 생성 이후에도 같은 인스턴스에 값·메서드·자식 노드를 추가하거나 교체·삭제할 수 있다. 상속, Qt, 전역 클래스 레지스트리를 요구하지 않는다. 정적 C++ 멤버 트리와 함께 구성한다.

이 기능은 런타임에 바뀌어야 하는 스키마와 동작이라는 구체적인 요구를 위해 추가했다. C++ 클래스의 물리적인 멤버 배치, ABI, 컴파일된 일반 메서드 본문을 수정하는 기능은 아니다. 동적 노드의 값·타입·검증기·접근 정책·호출 처리기는 해당 인스턴스에서 교체할 수 있다. 같은 정의로 생성한 다른 인스턴스는 바뀌지 않는다. 다만 사용자가 명시적으로 공유한 참조와 캡처 대상은 공유 상태이다.

## 최소 사용법

멀티스레드 애플리케이션에서는 `ObjectProperties(queue, definitions, root_policy, on_change)`로 [SerialQueue](parallel-runtime.md)에 연결하는 구성을 기본으로 한다. 외부는 `queries().post(query, completion)`을 사용한다. 소유자 apply도 큐 밖에서는 wrong_executor이며, 연결된 쿼리 포트의 외부 send 역시 wrong_executor이다. revision은 atomic 조회를 제공한다. 아래의 큐 없는 생성자는 직접 사용과 외부 직렬화를 위한 저수준 API 예시이다.
```cpp
#include <iiSDKCore/Runtime/ObjectProperties.h>
using namespace ii::runtime;

PropertyPolicy editable{
    .queryable = true, .designable = true, .writable = true
};
ObjectProperties properties({
    {.name = "audio", .policy = editable, .children = {
        {.name = "output", .policy = editable, .children = {
            {.name = "level", .value = 20, .policy = editable,
             .validate = [](const std::any& value) {
                 const int n = std::any_cast<int>(value);
                 return n >= 0 && n <= 100;
             }}
        }}
    }}
}, editable);

// 다른 객체는 전체 객체 대신 포트 참조를 받는다.
const auto& port = properties.queries();
auto changed = port.send(PropertyWrite{{"audio", "output", "level"}, 42});
if (!changed) { /* 수신자/전달 실패 */ }
else if (!*changed) { /* 정책/타입/검증 실패: changed->error() */ }

auto selected = port.send(PropertyRead{{
    {.name = "audio", .children = {
        {.name = "output", .children = {{.name = "level"}}}
    }}
}});
// 성공 시 (**selected).fields[0].children[0].children[0].value가 int 42이다.

auto extended = port.send(PropertyDefine{{"audio", "output"}, {
    .name = "gain", .value = 0.5, .policy = editable
}});
auto redesigned = port.send(PropertyDefine{{"audio", "output"}, {
    .name = "gain", .value = std::string("automatic"), .policy = editable
}, true}); // 명시적으로 기존 하위 트리 전체를 교체한다.
```

실제로 컴파일·실행하는 소비자는 `examples/properties.cpp`이다. 예제는 `ObjectProperties`를 Device의 private 멤버로 두고 쿼리 포트만 공개한다. 생성 시 다른 초기 값을 설정하고, 실행 중 한 인스턴스에만 gain과 scale 메서드를 추가한다. `./build/iiSDKCorePropertiesExample`로 실행한다.

## 트리와 선택

각 `PropertyDefinition`은 동시에 값(`value`), 자식(`children`), 메서드(`method`), 값 검증기(`validate`)를 가질 수 있다. 값 전용/메서드 전용/그룹 노드 모두 가능하다. 형제 이름은 고유하고 비어 있지 않아야 한다. 경로는 문자열 하나가 아닌 `vector<string>`의 세그먼트이며 `.`이나 `/`를 특별 해석하지 않는다.

`PropertyRead`는 요청한 노드만 중첩 `PropertySnapshot`으로 반환한다. 빈 선택은 빈 응답이고, 자식 선택을 생략하면 하위 트리를 자동 탐색하지 않는다. 선택 노드의 `include_value=false`이면 해당 노드의 값도 복사하지 않는다. 전체 객체, private 값, 미선택 형제는 자동 첨부하지 않는다. 존재하지 않거나 허용되지 않은 경로가 하나라도 있으면 부분 데이터 없이 오류를 반환한다.

값은 `std::any`에 정확한 C++ 타입으로 저장한다. 스칼라, 문자열, 작은 DTO 등 copy-constructible 타입을 사용할 수 있다. `int`와 `double` 사이의 암묵적 변환은 하지 않는다. `PropertyPolicy::typing=dynamic`은 쓰기 시 타입 변경을 허용하며, `PropertyWrite::convert`와 `PropertySelection::cast_to`로 명시적 캐스팅을 요청할 수 있다. 자세한 계약은 [동적 타입과 캐스팅](dynamic-value.md)을 따른다. `std::any{}`는 값이 없는 그룹을 뜻한다. 명시적인 null 값이 필요하면 `std::nullptr_t` 또는 도메인 nullable 타입을 값으로 등록한다. 전체 객체 값이나 `std::reference_wrapper`도 명시적으로 등록·선택한 경우에만 전달된다. 빌린 참조의 수명은 호출자가 보장한다.

## 권한

기본 정책은 public, queryable, non-designable, read-only이다. 루트도 같은 정책을 가지며 생성자의 `root_policy`로 설정한다.

| 정책 | 의미 |
|---|---|
| `visibility=private_member` | 외부 포트에서 해당 노드와 모든 자손의 접근을 막는다. |
| `queryable` | 외부의 값 선택·쓰기·메서드 호출 경로를 허용한다. 경로의 모든 조상이 허용해야 한다. |
| `writable` | queryable 경로의 해당 값에 `PropertyWrite`를 허용한다. fixed 정책에서는 타입을 유지하고 dynamic 정책에서는 타입 변경도 허용한다. |
| `designable` | 외부의 메타데이터 조회와 구조 편집 경로를 허용한다. 경로의 모든 조상이 허용해야 한다. |

queryable과 designable은 독립적이다. 디자인 도구에 구조만 공개하거나, 앱에는 값 조회만 허용할 수 있다. designable 노드에는 정의를 바꾸는 권한이 있으므로 writable=false여도 명시적인 **스키마 교체**로 값을 바꿀 수 있다. 쓰기와 재설계를 모두 막으려면 writable/designable을 모두 false로 둔다.

`PropertyDescribe{path}`는 선택한 노드의 정책, 값 타입(`std::type_index`), 메서드·검증기 존재 여부, 접근 가능한 직계 designable 자식 이름만 반환한다. 값·메서드 본문·검증기 본문은 포함하지 않는다. 재귀 스키마 덤프가 아니며, private 자식의 이름도 노출하지 않는다. 타입 메타데이터는 같은 C++ 프로세스의 식별 정보이고 안정적인 직렬화 타입 ID가 아니다.

기존 노드 교체/삭제는 대상 하위 트리의 모든 노드가 public·designable이어야 한다. public 부모를 삭제하거나 교체해서 private 또는 잠긴 자손을 우회할 수 없다. 이런 거절은 요청 대상 경로만 반환하고, 숨겨진 자손의 이름을 오류에 첨부하지 않는다.

`apply(query)`는 소유자 권한으로 같은 연산을 수행하며 public/private, queryable/designable/writable 정책을 우회한다. 타입 검사와 값 검증은 우회하지 않는다. 따라서 ObjectProperties 자체를 외부에 공개하지 않고, 소유 객체의 private 멤버로 두며 `queries()` 참조만 전달한다. 이는 같은 프로세스 내 API 권한 구분이며 임의의 네이티브 코드에 대한 보안 샌드박스가 아니다.

## 변경과 메서드

- `PropertyWrite`: 존재하는 값을 typing 정책과 명시적 convert 옵션에 따라 변환·검증한 뒤 변경한다. 기본 fixed 정책은 정확한 타입을 유지한다.
- `PropertyDefine`: parent 아래에 새 정의를 추가한다. 동명 노드 교체에는 `replace=true`가 필요하다. 교체는 하위 트리 전체에 적용되며 값 타입·정책·검증기·메서드도 변경할 수 있다.
- `PropertyErase`: 선택 노드와 자손을 제거한다. 루트 자체는 제거하지 않는다.
- `PropertyInvoke`: 선택한 동적 메서드에 필요한 인자 `std::any` 하나를 전달하고 필요한 결과 `std::any` 하나를 받는다. 여러 인자가 필요하면 작은 DTO를 정의한다. 빈 인자·반환값도 가능하다.

새 정의는 전체를 준비하고 초기 값까지 검증한 뒤 반영한다. 타입 불일치, 검증 실패, 중복 이름, 권한 실패는 기존 트리와 revision을 바꾸지 않는다. 쓰기·정의·교체·삭제가 성공할 때마다 revision이 1 증가한다. 생성 완료 직후는 0이며, 초기 구성에 대해서는 변경 알림을 발생시키지 않는다. 동일 값의 성공적인 쓰기도 하나의 변경이다.

검증기는 순수 검사 함수로 작성한다. 검증·스냅샷 생성·편집 중 동일 트리에 재진입하면 `reentrant`를 반환한다. 사용자 검증기 자체의 외부 부작용을 롤백하지는 않는다. 초기 생성 정의가 잘못되면 생성자가 `invalid_argument`를 던진다. 런타임 검증기 예외는 `handler_failed`와 원래 exception_ptr로 반환한다.

메서드는 호출 시 해당 처리기 인스턴스를 유지한다. mutable 캡처의 상태가 다음 호출에 유지되며, 실행 도중 자신을 교체/삭제해도 현재 호출은 안전하게 끝난다. 다음 호출은 변경된 정의를 사용한다. 메서드에서는 소유자 쿼리로 같은 객체의 속성을 수정할 수 있다. 인자 타입은 도메인 메서드가 `any_cast` 등으로 검사한다. 메서드의 예외는 `handler_failed`이다. 메서드가 완료 전에 수행한 편집이나 외부 부작용 전체를 롤백하는 트랜잭션은 제공하지 않는다.

## 변경 알림도 쿼리

생성자의 선택적인 `ChangeHandler`는 내부 `QueryPort<PropertyChange>`의 수신 처리기이다. 편집을 반영한 후 `PropertyChange{kind, path, revision}` 쿼리를 동기 전달한다. 값이나 객체 전체를 자동 전송하지 않는다. 다른 객체의 쿼리 포트로 전달하거나 애플리케이션이 소유한 Stream/EventLoop에 넣을 수 있다. 구독 레지스트리나 별도의 signal/slot 시스템은 없다.

알림 처리기에서는 다시 조회·편집할 수 있다. 중첩 편집은 새로운 revision을 가지며, 원래 응답의 revision은 원래 편집의 revision이다. 무한한 알림 순환은 애플리케이션이 피해야 한다. 알림 처리기가 예외를 던지면 이미 반영한 편집은 성공으로 유지하고 `PropertyResponse::notification_error`에 별도로 보존한다. 이런 응답을 실패한 편집으로 간주해 재시도하면 안 된다. 알림은 소유자가 구성한 신뢰된 처리기에 전달되며 private 편집의 경로도 포함하므로, 외부로 중계할 때 공개 범위를 결정해야 한다.

## 수명, 실행, 비용

외부 포트의 응답은 `QueryReply<PropertyResult>`이다. 바깥 expected는 QueryPort의 수신자/처리 오류, 안쪽 expected는 속성 연산의 성공/거절이다. `send`와 `post`는 기존 쿼리 계약을 그대로 따른다. 큐에 제출한 쿼리는 **실행 시점의** 정의·타입·정책으로 검증한다. 제출 뒤 노드가 삭제되거나 타입이 바뀌면 성공을 보장하지 않는다. 경로를 다시 추가하면 그 경로의 새 정의를 대상으로 한다. 노드의 이전 세대를 고정하는 핸들은 제공하지 않는다.

ObjectProperties와 루트 쿼리 포트의 주소는 객체 수명 동안 유지되므로, 내부 트리 재설계 때문에 수신 객체의 포트를 다시 연결할 필요가 없다. 스냅샷은 복사한 값과 트리를 소유하므로 원본 노드 삭제·교체 후에도 유효하다. 명시적으로 넣은 참조나 공유 소유 데이터는 해당 타입의 수명·공유 규칙을 따른다. ObjectProperties는 복사·이동하지 않는다. 객체가 소멸하면 미실행 쿼리는 receiver_expired가 된다. 메서드·검증기·알림 실행 중 해당 객체를 파괴해서는 안 되며, 사용자 캡처의 수명도 호출자가 보장한다.

모든 접근은 하나의 이벤트 루프 실행 스레드에 한정하거나 외부에서 직렬화한다. 내부 mutex로 사용자 처리기를 잠그지 않으며, reentrant 검사는 스레드 동기화 장치가 아니다. 임의 네이티브 C++ 메서드나 private 필드를 자동 반사하지 않는다. GraphQL 문자열 파서, 네트워크 전송, UI 에디터는 포함하지 않는다.

정적 멤버와 선할당 EventLoop/Stream은 기존의 재사용 경로를 유지한다. 동적 트리는 실제 노드와 값만 소유하고, 쓰기는 해당 노드의 저장 위치를 유지하며, 변경하지 않은 하위 트리와 메서드 상태는 재사용한다. `std::map`, `std::any`, 응답 스냅샷, 큰 콜백에는 할당·복사가 발생할 수 있고 삭제한 노드의 저장 공간은 해제한다. 동적 설계/조회 전체를 무할당 또는 하드 리얼타임 경로로 주장하지 않는다. 주기마다 고정된 데이터만 처리하는 경로는 기존 typed QueryPort와 Stream을 사용할 수 있다.

할당이나 사용자 값 복사 실패는 예외로 전파될 수 있으며 외부 QueryPort는 이를 전달 실패로 보존한다. 편집은 할당과 검증을 커밋 전에 완료한다. 사용자 값/캡처의 소멸자는 C++의 정상적인 noexcept 소멸 계약을 따라야 한다.

계약 테스트는 `tests/properties.cpp`, 실제 사용 예는 `examples/properties.cpp`, 설치 소비자의 공개 헤더·심볼 사용은 `tests/consumer/main.cpp`에서 검증한다.
