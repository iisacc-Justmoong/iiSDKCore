# Windows 빌드

Windows 하드웨어 백엔드는 Windows SDK의 C++/WinRT 헤더를 사용한다. Visual Studio 개발 환경에서 MSVC C++23으로 `cmake -S . -B build -G Ninja -DBUILD_TESTING=ON`을 실행한 뒤 빌드하고 CTest로 런타임과 하드웨어 계약을 검증한다. MinGW Qt 키트와 별도로 빌드한다. 속성 키 헤더를 명시적으로 포함하여 Windows SDK 헤더의 암묵적 포함 순서에 의존하지 않는다.
# USB 장치 인터페이스 GUID

Windows SDK의 `GUID_DEVINTERFACE_USB_DEVICE` 정의는 Microsoft가 지정한 순서대로 `initguid.h` 뒤에 `usbiodef.h`를 포함한다. USB 장치 열거를 사용하는 DLL과 별도 실행 파일을 함께 빌드하고 하드웨어 런타임 테스트를 실행하여 링크 및 실제 장치 열거를 검증한다.
# 표준 컨테이너 회귀 테스트

고정 버퍼의 PMR 테스트는 각 반복에 새 자원 객체를 연결하여 MSVC와 GNU 구현 모두에서 동일한 외부 버퍼를 재사용한다. 작은 표준 컨테이너는 성공하고 용량을 초과하는 요청은 `std::bad_alloc`으로 실패하는지 검증한다.
