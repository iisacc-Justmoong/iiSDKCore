# Windows 빌드

iiSDKCore 0.12는 C++23 컴파일러와 일반 Windows SDK를 사용한다. C++/WinRT 장치 투영 헤더나 USB·Bluetooth·오디오·MIDI 라이브러리는 Core 의존성이 아니다. 해당 코드는 [iiDeviceMultimedia Windows 빌드](../../iiDeviceMultimedia/docs/Windows.md)로 이동했다.

Visual Studio 개발 환경에서 `cmake -S . -B build -G Ninja -DBUILD_TESTING=ON`으로 구성하고 빌드·CTest를 수행한다.

# 표준 컨테이너 회귀 테스트

고정 버퍼의 PMR 테스트는 각 반복에 새 자원 객체를 연결하여 MSVC와 GNU 구현 모두에서 동일한 외부 버퍼를 재사용한다. 작은 표준 컨테이너는 성공하고 용량을 초과하는 요청은 `std::bad_alloc`으로 실패하는지 검증한다.
