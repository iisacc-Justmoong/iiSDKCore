# 동적 타입과 명시적 캐스팅

`<iiSDKCore/Runtime/Value.h>`의 `DynamicValue`는 `std::any`의 별칭이다. 표준 저장소를 재구현하지 않고, 런타임이 필요한 변환 오류와 손실 없는 숫자 캐스팅 계약을 추가한다. int, double, bool, std::string, 사용자 정의 copy-constructible DTO, 명시적인 reference_wrapper 등을 저장하고 이후 다른 타입의 값으로 대입할 수 있다. 원래 C++ 타입을 유지하므로 int와 long도 별개이다. 타입 조회는 value.type(), 값 존재 여부는 has_value(), 제거는 reset()이다. 빈 값과 nullptr_t는 다르다.

```cpp
#include <iiSDKCore/Runtime/Value.h>
using namespace ii::runtime;

DynamicValue value = 42;
value = std::string("42");
auto number = value_cast<int>(value);       // expected<int, CastError>, 값 42
auto text = value_ref<std::string>(value); // expected<reference_wrapper<const string>, CastError>
auto selected = cast_value(value, typeid(double)); // expected<DynamicValue, CastError>
```

value_cast는 새 값을 반환하며 원본을 변경하지 않는다. cast_value는 실행 시 결정된 std::type_index를 목적 타입으로 받는다. value_ref는 정확한 타입의 const 참조만 빌리고 복사나 변환을 하지 않는다. 임시 DynamicValue에서 참조를 빌리는 호출은 컴파일 단계에서 거절한다. 빌린 참조는 원본의 대입·reset·소멸 이후 사용할 수 없다. 타입 이름과 type_index는 현재 프로세스의 C++ 타입 식별이며 영구 저장 형식이나 네트워크 스키마 ID가 아니다.

## 지원 변환

| 입력과 출력 | 계약 |
|---|---|
| 동일한 C++ 타입 | 사용자 정의 DTO, null, 참조 래퍼를 포함한 값 복사 |
| 표준 정수·float·double·long double 사이 | 범위와 정밀도가 보존될 때만 성공 |
| bool과 숫자 사이 | bool 출력은 정확한 0/1만 허용 |
| std::string → 표준 정수 | 전체 문자열을 10진수로 파싱. 공백, 선행 +, 소수점, 뒤따르는 문자를 거절 |
| std::string → float/double | locale 독립 파싱. 소수·지수 표기 허용. 대상 타입으로 반올림하며 범위 초과·0으로의 언더플로를 거절하며 NaN/무한대 표기는 허용하지 않음 |
| std::string ↔ bool | true/false 및 입력 1/0. 대소문자를 구분하며 출력은 true/false |
| 표준 정수·float·double → std::string | locale 독립 출력. 부동소수점은 같은 타입으로 왕복 가능한 표기 |
| long double ↔ std::string | 모든 플랫폼에서 unsupported. 일부 WASM 표준 라이브러리의 double 축소 출력으로 인한 조용한 정밀도 손실을 방지 |
| 서로 다른 사용자 정의 타입·enum·포인터 타입 | 자동 변환 없음. 사용자 코드가 명시적으로 변환한 결과를 새 값으로 구성 |

정수 타입은 bool, char, signed/unsigned char, short, int, long, long long과 각 unsigned 타입이다. char 변환은 문자 코드의 숫자 변환이며 유니코드 인코딩 변환이 아니다. C 문자열이나 string_view는 저장 시 std::string으로 자동 소유권 변환되지 않는다. 텍스트 변환에는 명시적인 std::string을 사용한다.

숫자 사이 캐스팅은 42.5 → int, 9007199254740993 → double처럼 정보를 잃는 변환을 precision_loss로 거절한다. 범위 검사 후에만 C++ 캐스트를 실행하여 float → 정수의 범위 초과를 방지한다. 같은 부동소수점 타입의 NaN/무한대 복사는 허용하지만 다른 숫자 타입이나 문자열로 변환할 때는 out_of_range이다. 문자열 → 부동소수점 파싱의 반올림은 숫자 사이의 무손실 변환과 구분한다. 부동소수점 텍스트는 각 스트림에 classic locale을 적용하고 max_digits10 정밀도로 출력한다. 전역 locale을 바꾸지 않으며 기존 iOS 배포 대상과 WASM에서 부동소수점 charconv 지원을 요구하지 않는다.

CastError는 code, source, target을 포함한다. 오류는 empty, type_mismatch(참조 타입 불일치), unsupported, invalid_text, out_of_range, precision_loss이다. 메모리 할당 실패와 사용자 DTO 복사 생성자 예외는 예외로 전파한다. QueryPort 안에서 발생하면 기존 handler_failed 계약을 따른다.

## 객체 속성과 쿼리

PropertyPolicy::typing의 기본값은 fixed이다. dynamic을 지정한 값 노드는 PropertyWrite마다 저장 타입을 바꿀 수 있다. private/queryable/writable 및 검증기는 그대로 적용한다. 타입 변경을 허용한 노드의 검증기는 여러 입력 타입을 처리하거나 명시적으로 거절해야 한다. 빈 그룹에 값을 추가하거나 스키마를 바꾸는 작업은 기존 PropertyDefine을 사용한다. 빈 DynamicValue 쓰기는 거절하며 명시적인 null은 nullptr_t로 쓴다.

```cpp
ObjectProperties object(queue, {
    {.name="payload", .value=0,
     .policy={.writable=true, .typing=PropertyTyping::dynamic}},
    {.name="count", .value=0, .policy={.writable=true}}
});

// payload의 저장 타입을 string으로 변경한다.
PropertyWrite dynamic_write{{"payload"}, std::string("ready")};
// count의 현재 int 타입으로 명시적으로 변환한 후 검증·저장한다.
PropertyWrite converted_write{{"count"}, std::string("42"), true};
// 응답만 string으로 변환하고 저장된 count는 int로 유지한다.
PropertyRead projection{{{.name="count", .cast_to=typeid(std::string)}}};
```

위 요청은 기존 object.queries().post(request, completion) 경로로 전달한다. PropertyWrite::convert=true는 fixed/dynamic 모두 현재 저장 타입을 목적 타입으로 사용한다. false에서는 fixed가 정확한 타입만 허용하고 dynamic은 새 타입을 그대로 저장한다. 변환·검증 실패 시 값, 타입, revision, 변경 알림을 갱신하지 않는다. PropertyErrorCode::cast_failed와 cast_error에 원래 CastError를 보존한다.

동적 payload를 직접 전달하는 QueryPort<DynamicValue, DynamicValue>도 지원한다. completion은 `const QueryReply<DynamicValue>&`로 받으면 응답 복사를 피할 수 있다. 검증한 Emscripten 표준 라이브러리에서는 expected<any>의 값 전달과 중첩 expected의 생성자 제약 검사에 컴파일 오류가 있으므로 이 참조 형태를 사용한다. 오류 세부사항을 값으로 전달할 경우 기존 PropertyResult처럼 이름 있는 응답 DTO 안에 값과 오류 정보를 구성한다. 콜백 밖에서 결과가 필요하면 콜백 안에서 필요한 값만 복사한다.

PropertySelection::cast_to는 선택한 응답 값에만 적용한다. include_value=false이면 캐스팅도 수행하지 않는다. 실패 시 경로와 원인을 반환하며 부분 응답을 성공으로 내보내지 않는다. 메서드도 DynamicValue 인자·반환값에서 같은 value_cast/value_ref를 사용할 수 있으며, 기존 std::any 소비 코드는 호환된다.

## 동시성·메모리

DynamicValue 자체가 가변 값의 동시 접근을 동기화하지는 않는다. 별도 값은 독립적으로 사용할 수 있으며 공유 가변 값은 SerialQueue 또는 명시적인 잠금으로 보호한다. 사용자 정의 복사 생성자와 참조 래퍼의 대상도 작성자가 해당 계약을 지켜야 한다. 전역 변환 레지스트리나 공유 가변 캐시는 없다.

기존 런타임 슬롯 재사용은 유지한다. 임의 타입의 저장과 값 복사·문자열 변환은 할당할 수 있다. 복사 없는 정확한 타입 접근은 value_ref를 사용한다. 동적 타입 전체를 무할당 경로로 보장하지 않는다.
