# iiSDKCore

iiSDK의 최하위 기반 모듈을 위한 초기 C++ 프로젝트이다. 현재 소스는 `hello()`를 내보내는 공유 라이브러리 골격이며, Qt에 의존하지 않는다. 기반 API와 플랫폼 기능은 아직 구현되지 않았다. 설계 및 작업 원칙은 [AGENT.md](AGENT.md)에 정의한다.

## 빌드

CMake 3.31 이상과 C++20 컴파일러가 필요하다. 생성 파일은 `build/`에 둔다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local/SDK/iiSDKCore"
cmake --build build --clean-first
ctest --test-dir build --output-on-failure
cmake --install build
```

설치는 공유 라이브러리, 공개 헤더와 CMake 패키지를 제공한다. 소비자는
`find_package(iiSDKCore 0.1 CONFIG REQUIRED)`와 `iiSDKCore::iiSDKCore` 타깃으로
연결한다. 헤더는 다른 SDK의 같은 파일명과 충돌하지 않도록 `<iiSDKCore/library.h>`로
포함한다. 기존 `<library.h>` 경로도 유지한다. 소스와 헤더는 `src/`에 함께 두며,
빌드 트리의 `build/include/iiSDKCore/`에 공개 헤더를 복사하여 설치 전 소비자에도
동일한 이름공간을 제공한다. 소비자 테스트는 두 포함 경로를 함께 컴파일한다. CMake 표준 설치 도구만
사용하므로 Qt나 새로운 외부 의존성은 추가하지 않는다.

공개 함수 호출 테스트와 다음 별도 설치 소비자로 패키지 검색, 헤더 사용, 링크 및
실제 공유 라이브러리 로딩을 검증한다.

```sh
cmake -S tests/consumer -B build/consumer/build \
  -DCMAKE_PREFIX_PATH="$HOME/.local/SDK/iiSDKCore"
cmake --build build/consumer/build
ctest --test-dir build/consumer/build --output-on-failure
```

IDE 설정, 빌드 산출물 및 비어 있는 설계 디렉터리는 저장소에 포함하지 않는다.

## Source layout

Implementation files and their headers live together under `src/`. Existing feature and platform subdirectories retain their responsibilities. Build configuration, tests, documentation, resources, and maintenance scripts remain at the project root. Configure and build using the repository-local `build/` directory.
