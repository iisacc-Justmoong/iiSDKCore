# C++23 임베디드 실행 기반

`iiSDKCore::Embedded`는 MCU·베어메탈·RTOS·FPGA 소프트코어 CPU가 공유하는 작은 실행 기반이다. 고정 슬롯 콜백, 32비트 단조 시각의 일회·주기 타이머, 취소 티켓과 실행 예산을 제공한다. 실제 소비자는 iiSDKBuilder의 공통 EmbeddedHello와 Arduino 브리지이다.

`EventLoop<N>`의 슬롯·티켓은 객체가 소유한다. 복사·이동하지 않으며 티켓의 유효 수명은 소유 루프의 수명 안으로 제한한다. 같은 주소의 새 객체에 이전 티켓을 전달하지 않는다. 콜백 문맥은 예약된 작업보다 오래 유지한다. 메모리 할당, OS, 스레드, RTTI, 예외, 파일 시스템이나 인터프리터가 필요하지 않다. 일반 C++23 함수 포인터를 호출한다. 티켓은 루프 주소·슬롯·64비트 세대로 검증하며 재사용된 슬롯이나 다른 루프의 취소를 거절한다.

시각은 BSP가 공급한다. 예약 간격은 `INT32_MAX` 밀리초 이하이며, BSP는 이 범위보다 자주 `poll`을 호출해야 한다. 32비트 시각의 순환을 허용한다. 놓친 주기는 한 번 실행한 뒤 미래 주기로 이동하고, 한 번의 `poll`은 슬롯 수와 요청한 예산 이하만 처리한다. 중첩 `poll`은 실행하지 않는다. 콜백은 협력적으로 짧게 반환한다. ISR이나 다른 코어가 직접 같은 객체를 수정하려면 BSP가 별도의 임계 구역·전달 계약을 소유해야 한다.

```cpp
#include <iiSDKCore/Embedded/Embedded.h>
ii::embedded::EventLoop<8> loop;
auto ticket = loop.every(nowMilliseconds, 16, {update, &state});
loop.poll(nowMilliseconds, 4);
loop.cancel(ticket);
```

`-DIISDKCORE_EMBEDDED_ONLY=ON`은 이 구성만 빌드·설치한다. `find_package(iiSDKCore CONFIG REQUIRED COMPONENTS Embedded)`와 `iiSDKCore::Embedded`로 소비한다. 일반 구성에도 독립 타겟을 설치한다. 작은 구성에서 Runtime·Values·Contracts가 있다고 표시하지 않는다. 기존 Runtime의 멀티스레드 계약은 그대로 유지한다. 제한된 MCU에 기존 데스크탑 런타임을 자동으로 넣지 않는다.

두 구성 모두 원본과 선택한 실제 번역 단위의 전처리 결과를 검사하는 필수 `iiSDKCoreArchitectureCheck`를 실행한다. Arduino CLI는 같은 매크로 독립적인 Embedded 소스를 보드 컴파일러로 다시 컴파일한다. 호스트의 Core 정책 검사를 먼저 수행하며, 보드 CLI의 컴파일 DB argv로 실제 BSP 컴파일러·매크로·옵션의 필수 전처리 검사도 수행한다. 실제 보드 컴파일은 헤더의 C++23 검사와 ELF CPU 검증을 통과해야 한다. 구형 BSP 컴파일러가 C++23을 지원하지 않으면 해당 빌드는 실패하며 표준을 낮추지 않는다.

이 구성은 차량 안전 등급이나 RTOS·MCU 드라이버를 인증하지 않는다. 시작 코드, 링커 스크립트, 인터럽트, 주기 시각과 장치 드라이버는 실제 BSP가 소유한다. `tests/embedded.cpp`는 예산, 취소, 슬롯 한도, 반복 타이머 자기 취소와 시각 순환을 검증한다.

MSVC에서는 `/Zc:__cplusplus`가 없어도 C++23을 정확히 인식하도록 `_MSVC_LANG`를 우선 사용한다. 다른 컴파일러에서는 `__cplusplus`를 사용한다. `tests/embedded_language_macro.cpp`는 레거시 `__cplusplus=199711L`과 C++23 `_MSVC_LANG` 조합을 검사한다. [Microsoft 언어 버전 매크로 계약](https://learn.microsoft.com/en-us/cpp/build/reference/zc-cplusplus)을 따른다.
