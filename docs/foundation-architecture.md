<a id="foundation-boundary-013"></a>
# 기반 경계와 모듈 책임

0.13에서 분리한 기반 계층의 책임과 의존 방향을 유지하며, 0.20 후속의 공유 관측 실행부를
포함한다. 0.14의 텍스트·오류 계약과 네이티브 MIDI 소비자는
[Qt 이전 우선순위](qt-migration-p0.md)를 참조한다.

iiSDKCore는 앱과 게임이 공유하는 C++23 값·수명·실행·오류 계약을 제공하며 SDK 의존
그래프의 최하위에 둔다. Qt 호환, 파일 제공·프리뷰, HTTP/TLS, SQL 엔진, 이미지 코덱,
문서 파서는 상위 모듈의 책임이다. 표준 C++만으로 구현할 수 없는 네이티브 대기와
핸들 소유권은 내부 플랫폼 구현으로 격리한다.

<a id="packages-and-responsibility"></a>
## 패키지와 책임

| 소비 목적 | CMake 컴포넌트 / 타깃 | 의존성 |
| --- | --- | --- |
| 공통 데이터·실행 계약 | `Contracts` / `iiSDKCore::Contracts` | C++23 헤더 |
| 값·CBOR·공유 값 유틸리티 | `Values` / `iiSDKCore::Values` | Contracts; Runtime·OS 서비스 없음 |
| 네이티브 getter의 동기 읽기 수집 | `Observation` / `iiSDKCore::Observation` | Contracts와 C++ 표준 실행부; 작업자 Runtime·Threads·GUI 없음 |
| 기존 런타임을 사용하는 앱·게임 | `Runtime` / `iiSDKCore::Runtime` | Contracts, Observation, 플랫폼 스레드와 네이티브 런타임 |
| 기존 기본 소비자 | 컴포넌트 생략 / `iiSDKCore::iiSDKCore` | 같은 Runtime; Values도 공개 |
| 파일 생성·원자적 저장·표시 | 별도 `iiFileProviderCoreIO` / `iiFileProvider::CoreIO` | Core Runtime + iiFileProvider Native |

헤더 전용 컴포넌트 조합은 Runtime·Threads·기능 SDK를 가져오지 않으며 패키지 버전의
호환 여부도 바이너리 포인터 크기로 제한하지 않는다. Runtime과 Observation 등 바이너리
소비자는 대상 ABI가 일치해야 한다. 알 수 없는 필수 컴포넌트는 패키지 탐색에서 거부한다.

선택적 CoreIO 어댑터는 독립적인 두 라이브러리 위에 둔다. Core와 기본 Native 파일
공급자는 어댑터나 서로를 찾거나 연결하지 않는다. 파일 공급자의 상위 빌드에서는
`IIFILEPROVIDER_BUILD_CORE_IO=ON`으로 활성화하며 독립 CMake 프로젝트로도 빌드할 수 있다.

<a id="common-data"></a>
## 공통 데이터

표준 컨테이너, 스마트 포인터, `std::string`, `std::span`, `std::chrono`, `std::expected`를
직접 사용한다. SDK 타입은 다음과 같이 별도의 의미가 필요한 경우에 제공한다.

- `Core/Bytes/Bytes.h`: 소유 `Bytes`와 읽기 전용 차용 `ByteView`이다. 소유자는 차용 뷰보다
  오래 살아야 하며 `byte_view`는 문자 인코딩을 변환하지 않는다.
- `Core/Text/Utf8.h`: 구조화 값과 기존 CBOR 코덱이 공유하는 유니코드 스칼라 UTF-8
  검증이다. 과잉 인코딩, 서로게이트, 잘림과 범위 초과를 거부한다. 내장 NUL 자체는
  유효한 텍스트이며 프로토콜·경로의 제한은 해당 계층이 적용한다. 정규화와 확장 인코딩은
  이 계약 밖에 둔다.
- `Core/Value/Value.h`: null, bool, 부호 있는·없는 정수, 유한 실수, UTF-8 텍스트,
  바이트, 배열, 객체를 소유한다. 생성과 정확한 `get<T>`는 명시적이다. 정수의 부호와
  bool, 필드 생략과 명시적인 null을 구분하며 실패한 조회는 타입 지정 결과로 반환한다.
  복사는 독립적인 논리 값을 소유하고 공유가 필요한 경우 `SharedValue<Value>`를 사용한다.
  포인터의 암묵적인 bool 변환과 축소되는 long double 변환을 거부하고, 임시 값에서 차용
  뷰를 꺼내는 사용은 컴파일 단계에서 거부한다. 외부에는 const 참조만 제공하며 재대입과
  제거는 기존 뷰를 퇴역시킨다. JSON 파싱이나 직렬화 자체는 수행하지 않는다. 파서는
  신뢰하지 않는 트리를 만들기 전에 입력·깊이·크기 예산을 적용해야 한다.
- `Core/Result/Result.h`, Time/Deadline과 PathRoot는 기존 오류·단조 시간·어휘적 경로
  계약을 유지한다. PathRoot 자체를 파일 시스템 보안 경계로 간주하지 않는다.

`runtime::DynamicValue`는 타입 지정 객체 값과 사용자 레코드를 위한 프로세스 내부의
`std::any` 저장소이다. 명시적인 캐스트와 등록한 CBOR 코덱은 기존 목적을 유지한다.
구조화 `core::Value`가 임의의 C++ 쿼리 payload를 대체하거나 전역 타입·직렬화 등록소를
만드는 것은 아니다.

<a id="execution-and-lifetime"></a>
## 실행과 수명

`Runtime/Execution.h`는 EventLoop·I/O·작업자 구현 없이 Task, Priority, Submission,
TaskToken, PumpBudget과 구조적 `TaskExecutor` 개념을 정의한다. EventLoop, Runtime,
SerialQueue가 이를 만족하며 QueryPort와 TaskScope도 같은 계약을 사용한다.

접수, 전달과 작업 결과를 구분한다. Submission은 큐 접수를, 타입 지정 QueryReply와
표준 future는 결과를 표현한다. TaskToken은 대기 작업을 취소하는 수명 안전한 불투명
핸들이며 실행 중인 작업의 중단은 협력적으로 이루어진다. TaskScope·QueryPort·SerialQueue는
접수 종료, 취소, 실행 중 호출 대기와 캡처 퇴역 경계를 유지한다. 차용 실행기는 이를
호출하는 작업보다 오래 살아야 한다.

객체는 인스턴스별 타입 지정 쿼리로 통신하며 통지 쿼리와 EventReceiver 필터도 같은
계약을 사용한다. 포트·수신자를 닫으면 전달을 분리한다. QObject 기반 클래스, 상속 기반
실행기, 전역 이벤트 버스나 별도 신호·슬롯 시스템을 요구하지 않는다. 프레임 단계,
고정 시간 갱신과 브라우저·네이티브 드라이버는 선택한 Runtime 구현의 책임이다.

<a id="native-streams-and-storage"></a>
## 네이티브 스트림과 저장

Channel은 바이트 전송, 준비 상태, 커서, flush, 공유 수명과 close를 소유한다. Core는
일반 파일을 열거나 생성하거나 원자적 저장 트랜잭션을 공개하지 않는다. `NativeFile.h`는
소유 POSIX descriptor 또는 intptr_t로 표현한 Windows 핸들을 인수하는 명시적 플랫폼
어댑터 확장이다. 일반 파일 여부를 검증하고 거부한 자원을 닫으며 할당 실패에서도
소유권을 정리한다. 공통 Channel API로 핸들을 돌려주지 않는다. 마지막 소유자 제거를
포함한 네이티브 close 성공 후 noexcept 종료 통지를 한 번만 실행한다.

`iiFileProvider::core::open_file`은 Native 공급자로 열기·생성을 수행하고 종료 통지를
연결한다. 해당 계층이 독점 생성, 초기화, 프리뷰·상세 정보 예약을 소유한다. AtomicFile은
같은 디렉터리의 임시 파일 생성·flush, 공급자의 원자적 교체와 임시 표시 데이터 정리를
수행한다. 취소·실패 시 직접 덮어쓰기로 전환하지 않는다. Core의 rooted File은 opener를
주입받아 반환된 Channel을 소유한다. 시리얼·문자 장치 바이트 채널은 기존 플랫폼 계약을
유지하고 USB·BLE·오디오·MIDI는 iiDeviceMultimedia에 둔다.

<a id="migration-and-enforcement"></a>
## 이전과 강제 검사

```cmake
find_package(iiSDKCore 0.13 CONFIG REQUIRED COMPONENTS Contracts)
target_link_libraries(MyDataModule PRIVATE iiSDKCore::Contracts)
# 작업을 실행하는 호스트는 COMPONENTS Runtime과 ::Runtime을 선택한다.
```

Core 0.12 일반 파일 소비자는 `<iiFileProviderCoreIO.h>`와 `iiFileProvider::CoreIO`를
사용하고 해당 네임스페이스의 `open_file`·`AtomicFile`로 이전한다. `core::File::open`에는
opener를 전달한다. 업그레이드 시 예전 생성·설치 Core AtomicFile 헤더를 제거한다.
기존 SDKBuilder 명세·보고서는 이미 소유한 Native 공급자의 `File::write`를 직접 사용한다.

필수 `iiSDKCoreFoundationBoundary` 빌드 타깃은 테스트가 비활성이어도 상위 SDK·Qt
include, 패키지 의존성과 기능 연결을 거부한다. CTest에서도 같은 경계를 검사한다.
설치 소비자는 Qt·파일 공급자를 비활성화하며 Contracts 소비자는 Threads도 비활성화한다.
네이티브 핸들 실패·수명, 값, 기존 Runtime 회귀와 재배치 저장 검사는 각 구현 경계를
확인한다.

이는 비GUI 모듈 이전의 기반이다. 기존 Qt 모듈과 GUI facade는 소비자별 이전이 필요하며
공개 API와 앱 바이너리에서 Qt를 모두 제거했다는 뜻이 아니다. 구조도에 있는 `iiStorage`와
`iiGraphics`는 역할을 나타내며 새 빈 SDK를 만들었다는 뜻이 아니다. 실제 분리는 QtCore
기능 목록의 복제보다 관측한 소비자와 검증한 교체 경계를 따른다.

## 공유 관측 실행부 (0.20 후속)

`ObservableReadScope`의 현재 수집 범위는 스레드마다 분리하되 앱과 숨김 공유
라이브러리 사이에서는 같은 `Observation` 실행부에 둔다. 헤더마다 따로 만든 TLS는
private 모델의 getter 내부 읽기를 놓치므로 범위 진입·종료·기록을 작은 컴파일 구현으로
분리한다. 별도의 이벤트 버스나 작업 스레드, 상위 GUI 의존성은 추가하지 않는다.

호스트 플랫폼은 큰 Runtime의 정적·동적 연결 선택과 관계없이 공유 Observation을
사용한다. iOS·WASM의 정적 모듈 정책에서는 단일 이미지에 정적으로 연결한다. 실제
관측을 사용하는 모델과 앱은 같은 실행부와 일치하는 헤더를 연결해야 한다.
`Contracts`만 가져오는 소비자는 Observation 타깃이나 바이너리를 가져오지 않는다.
`Runtime`은 공개 의존성으로 이를 연결하며, Observation만 필요한 모델에는 Runtime을
역으로 요구하지 않는다.

필수 의존성 검사는 타깃별 역할을 구분하여 Contracts의 구현 의존성과 Observation의
Runtime·Threads·GUI 의존성을 거부한다. 설치·재배치 검사는 숨김 공유모듈의 수집·
통지·중첩·예외·스레드·깊이 한도·수명과 Contracts 단독 소비자를 별도로 실행한다.
상세한 동기 범위·변경 발행 계약과 플랫폼 검증 한계는 [observable.md](observable.md)를 따른다.

<a id="resource-byte-foundation-017"></a>
## 자원 바이트 기반 (0.17)

Core는 메모리 내 자원 팩, 불변 공유 뷰와 네이티브 읽기 전용 Channel 매핑을 제공한다.
빌드 호스트의 자원 컴파일러는 Builder가, 파일 공개는 FileProvider가 소유한다. Core는
이들을 연결하거나 가져오지 않는다. 준비한 코덱·GPU payload는 불투명한 바이트로 유지하며
네이티브 형식 변환은 해당 기능 모듈에 둔다. Contracts·Values만 가져오는 소비자는
Runtime과 자원 구현을 가져오지 않는다. 상세 계약은 [resources.md](resources.md)를 따른다.
