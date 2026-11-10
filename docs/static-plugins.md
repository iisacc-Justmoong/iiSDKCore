# 앱별 정적 플러그인

`IISDKCORE_STATIC_PLUGINS=ON`은 Core의 플러그인 동적 로더를 빌드에서 제외하고
앱별로 생성한 정적 진입점 목록을 사용한다. 이 모드는 공유 Core를 허용하지 않으며
Observation도 정적으로 연결한다. CPython 호스트는 이 모드와 함께 켤 수 없다.

`IISDKCORE_STATIC_PLUGIN_SOURCES`는 플러그인 ABI의
`ii_sdk_plugin_v1`을 구현하는 C++ 소스 목록이다. 빌드는 각 진입점을 고유한
심볼로 바꾸고 목록과 구현을 동일한 `iiSDKCore` 정적 아카이브에 넣는다. 외부 앱
소스가 필요한 include 경로는 `IISDKCORE_STATIC_PLUGIN_INCLUDE_DIRS`로 제공한다.
`Plugin::open_static(identifier)`은 기술자의 식별자로 인스턴스를 생성하며,
중복 식별자는 오류이다. 이 모드의 `Plugin::load(absolute_path)`는 파일을 열지
않고 파일명에서 식별자를 추출해 정적 목록을 조회한다. 파일명과 식별자가 다른
앱은 `open_static`을 사용해야 한다.

`IISDKCORE_STATIC_PYTHON_SOURCES`와 `IISDKCORE_STATIC_PYTHON_COMPILER`는
호스트 컴파일러가 지원하는 Python 부분 집합을 C++ 플러그인으로 생성하는 경계이다.
변환 실패는 CMake 구성 실패이며 실행 시 CPython으로 대체하지 않는다. 빌드 도구가
앱별 소스와 컴파일러를 제공하며 Core는 빌더 모듈을 찾거나 링크하지 않는다.
데스크톱 기본 구성의 기존 동적 플러그인 경로는 정적 모드 밖에서만 유지된다.
