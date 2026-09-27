# iiSDKCore

iiSDK의 최소 C++23 애플리케이션 런타임이다. Qt, JVM, .NET, 외부 이벤트 라이브러리 없이 C++ 표준 라이브러리와 플랫폼의 스레드·시간 기반에서 실행된다. 앱, 게임, 센서, 스트림 처리는 같은 이벤트 루프와 값 타입을 조합한다. 패키지 버전은 0.2.0이다.

## 제공 기능

- 18개 기반 폴더에 경로·파일·바이너리 커서·작업 묶음·서비스 참조·진단·의존 그래프·Scene·스크립트 바인딩·호스트 펌프·타임라인 객체를 제공한다. [기반 객체 계약](docs/foundations.md), [센서 값 비동기 처리 예제](examples/foundations.cpp)에 책임과 수명 규칙을 정의한다.
- `ii::io::Channel`, `AsyncIO`, `Buffer`: 네이티브 파일·시리얼 바이트 I/O, 준비 대기·시간 초과·취소, 재사용 버퍼와 비동기 쿼리. OS 핸들을 숨기며 WASM 가상 파일과 실제 장치의 지원 차이를 명시한다. [I/O 계약](docs/io.md)에 정의한다.
- `DynamicValue`, `value_cast<T>()`, `value_ref<T>()`: 동적 C++ 값과 명시적 타입 변환·참조 접근. 숫자 범위·정밀도 검사, 문자열 변환, 속성의 동적 타입 정책과 조회 시 캐스팅을 지원한다. [동적 타입 계약](docs/dynamic-value.md)에 정의한다.
- 기본 실행 모델은 `Runtime`의 멀티스레드·병렬·비동기 처리이다. 생성 시 워커를 시작하고 작업 슬롯을 재사용한다. `SerialQueue`는 같은 객체의 쿼리를 순서대로 처리하며 서로 다른 객체는 병렬 실행한다. [기본 런타임 계약](docs/parallel-runtime.md)과 [객체 간 비동기 예제](examples/parallel.cpp)에 정의한다.
- `Thread`: 플랫폼 독립 스레드 소유·시작·협력적 중단·종료 대기·상태·예외 전달. EventLoop 실행과 중단을 연결하며 join 후 인스턴스를 재사용한다. [스레드 계약](docs/thread.md), [쿼리 실행 예제](examples/thread.cpp)에 정의한다.
- `ObjectProperties`: 생성 시와 생성 후에 인스턴스별 값·메서드·자식 트리를 정의·교체·삭제한다. queryable/designable 정책, private 경로 보호, 선택 조회, 타입·값 검증, 변경 쿼리를 지원한다. [동적 속성 계약](docs/object-properties.md)과 [실행 예제](examples/properties.cpp)에 정의한다.
- 객체 멤버 트리: 중첩 인스턴스의 각 노드에 값·일반 메서드·쿼리 포트를 구성하고, 계층별 public/private 접근 제어를 보존한다. [트리 계약과 구현 예제](docs/member-tree.md)에 정의한다.
- `QueryPort<Request, Response>`: 필요한 입력·선택한 응답 필드만 전달하는 객체 간 쿼리. 객체 전체 전달은 명시적 선택 사항이며, 동기/큐 전달, 값/void 응답, 수신자 만료·처리 실패를 지원한다.
- `EventLoop`: 다중 생산자·단일 실행자 작업 큐, 즉시/예약/주기 작업, 취소 토큰, 4:2:1 가중 우선순위, 콜백 수·시간 예산, 중단/재개/종료, 통계.
- `Stream<T>`: 선할당 순환 버퍼, FIFO, 거절 또는 오래된 데이터 폐기, 종료 후 잔여 데이터 소비, 역압력·폐기 통계.
- `BrowserLoop`: Emscripten에서 `requestAnimationFrame`으로 이벤트 루프를 구동하는 인스턴스.
- `run()`: 네이티브 독립 루프. 작업이 없으면 조건 변수로 대기하며 새 작업·타이머·종료에 깨어난다.
- `poll()`: 호스트가 루프를 소유하는 UIKit, Android, 데스크톱 GUI, 브라우저에 삽입하는 비차단 실행 단위.

공개 객체는 상속 없이 인스턴스와 콜백으로 조합한다. 의존 객체는 참조로 받고, 데이터는 값 또는 명시적 참조로 전달한다. 구현 내부의 RAII 소유권과 브라우저 C ABI 경계에는 포인터가 필요하지만 이를 소비자 소유권 API로 노출하지 않는다.

## 최소 사용 예

```cpp
#include <iiSDKCore/Runtime/Query.h>

int main() {
    using namespace ii::runtime;
    Runtime runtime;             // 기본 2~4개 워커가 자동 시작된다.
    SerialQueue object(runtime);
    QueryPort<int, int> calculate(object, [](const int& value) { return value * 2; });
    int answer = 0;
    auto accepted = calculate.post(21, [&](QueryReply<int> result) {
        if (result) answer = *result;
    });
    if (!accepted) return 1;
    if (!runtime.wait_idle() || answer != 42) return 2;
    return runtime.join() ? 0 : 3;
}
```

실행 가능한 [라이브 예제](examples/live.cpp)는 주기 생산자와 소비자가 같은 루프에서 120개의 샘플을 처리한다. 같은 파일을 네이티브 실행 파일 또는 WASM HTML로 빌드한다.

`./build/iiSDKCorePropertiesExample`은 생성 이후 한 Device 인스턴스에 gain 속성과 scale 메서드를 추가하고, 필요한 값만 쿼리한다. 같은 스키마로 생성한 다른 인스턴스에는 변경이 전파되지 않는다.

## 빌드

CMake 3.31 이상과 C++23 컴파일러가 필요하다. 생성 파일은 `build/`에 둔다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/iiSDKCoreLive
cmake --install build --prefix "$PWD/build/stage"
```

소비자는 `find_package(iiSDKCore 0.2 CONFIG REQUIRED)`와 `iiSDKCore::iiSDKCore`로 연결한다. C++23과 스레드 의존성은 타깃에서 전파된다. `BUILD_SHARED_LIBS=OFF`로 정적 라이브러리를 선택할 수 있다. iOS와 Emscripten은 정적 라이브러리를 생성한다. 기존 `hello()`는 유지한다. 헤더는 다른 SDK와 충돌하지 않도록 반드시 `<iiSDKCore/...>` 경로로 포함한다.

공개 함수 호출 테스트와 다음 별도 설치 소비자로 패키지 검색, 헤더 사용, 링크 및
실제 공유 라이브러리 로딩을 검증한다.

```sh
cmake -S tests/consumer -B build/consumer/build \
  -DCMAKE_PREFIX_PATH="$PWD/build/stage"
cmake --build build/consumer/build
ctest --test-dir build/consumer/build --output-on-failure
```

Windows 다중 구성 생성기에서는 빌드·설치에 `--config Release`, CTest에 `-C Release`를 추가하고, 공유 라이브러리 소비자를 실행할 때 설치 경로의 `bin`을 `PATH`에 넣는다.

## 플랫폼과 범위

| 대상 | 실행 방식 |
|---|---|
| macOS / Windows / Linux | 네이티브 C++ 실행 파일 또는 라이브러리. 독립 `run()` 또는 호스트의 `poll()` |
| iOS | 네이티브 정적 라이브러리. UIKit 호스트가 `poll()` 호출, 독립 작업 스레드는 `run()` 가능 |
| Android | NDK 라이브러리. Activity/NativeActivity 호스트가 `poll()` 호출하거나 작업 스레드에서 `run()` |
| WASM | pthread 기본 ON. 애플리케이션 워커에서 Runtime/Thread를 사용하며 브라우저 메인 스레드는 BrowserLoop/`poll()` 사용 |

Linux 코어에는 X11/Wayland, 특정 배포판 또는 CPU 명령어 의존성이 없다. 다만 모든 Linux 배포판·아키텍처를 검증했다는 의미는 아니다. 실제 검증 결과는 [검증 기록](docs/validation.md)에 따로 기록한다.

이 런타임은 네이티브 **사용자 공간** 기반이다. 커널 권한을 갖는 OS 자체가 아니며, WASM은 브라우저 실행 환경을 따른다. 시간 예산은 협력적 실행 제한이고 하드 리얼타임 보장이 아니다. 파일·시리얼 I/O와 채널별 readiness 대기를 제공한다. 플랫폼 GUI 시작점, 창, 입력, 다중 채널 I/O reactor, 렌더링, 오디오 및 앱 패키징은 아직 제공하지 않는다. 해당 호스트와 상위 모듈은 이벤트를 `post()`하고 `poll()`을 호출하여 조합한다.

세부 의미론과 교차 빌드는 [런타임 설계](docs/runtime.md), 프로젝트 원칙은 [AGENT.md](AGENT.md)에 정의한다. IDE 설정, 빌드 산출물 및 비어 있는 설계 디렉터리는 저장소에 포함하지 않는다.

객체 간 통신은 [쿼리 계약](docs/query.md)에 따라 구성한다. [객체 통신 예제](examples/query.cpp)는 Controller가 `device.audio.output.set_level`에 `SetLevel{42}`를 전달하고, `ReadDevice{.audio = {.output = {.level = true}}}`로 값 하나만 요청한다. 응답도 `audio.output.level` 구조를 유지하며 이름·객체 참조·미선택 형제 필드는 생략한다. `./build/iiSDKCoreQueryExample`로 실행한다.

## Source layout

Implementation files and their headers live together under `src/`. Existing feature and platform subdirectories retain their responsibilities. Build configuration, tests, documentation, resources, and maintenance scripts remain at the project root. Configure and build using the repository-local `build/` directory.
