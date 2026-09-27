# 런타임 0.2.0 검증 기록

검증일: 2026-09-25, Asia/Seoul. 아래 결과는 이 변경을 작업한 macOS ARM64 환경의 실제 실행 기록이다. CI 구성 추가와 CI 실행 성공은 별개이며, 원격 CI는 아직 실행하지 않았다.

## 결과

| 대상 | 확인 결과 | 제한 |
|---|---|---|
| macOS ARM64 / Release / 공유 라이브러리 | 멤버 트리 포함 CTest 7/7, 라이브 120개 생산·소비 성공 | GUI/윈도우 호스트는 범위 밖 |
| macOS / ASan + UBSan / Debug 정적 라이브러리 | 경고를 오류로 처리한 빌드 및 CTest 7/7 성공 | 테스트가 실행한 경로의 검사 |
| macOS / TSan / Debug 정적 라이브러리 | CTest 6/6 성공, 경합 보고 없음 | 모든 가능한 실행 순서의 증명은 아님 |
| macOS 설치 소비자 | stage 설치본의 find_package, 공개 헤더, 링크, 동적 로딩, 실행 1/1 성공 | 전역 ~/.local 설치본은 교체하지 않음 |
| iOS ARM64 device SDK | 정적 라이브러리 교차 빌드 성공 | 물리 iPhone 및 서명된 앱 실행은 미검증 |
| iOS 27.0 ARM64 시뮬레이터 | 런타임·쿼리·멤버 트리 계약, 재사용, 라이브·객체 통신 예제 실행 성공 | UIKit 앱 생명주기 통합 검증과 구분 |
| Android NDK 30 / ARM64 / API 26 | 공유 라이브러리, 계약·재사용·소비자 테스트, 라이브 실행 파일 링크 성공 | 연결된 장치 없음, Android 실행 미검증 |
| WASM / Emscripten / Node | 공통·쿼리·멤버 트리·소비자·객체 통신 CTest 5/5 성공 | 스레드 테스트는 비활성화 |
| WASM / 실제 Codex 브라우저 | 라이브 120개 생산·소비 완료, BrowserLoop 계약 성공 | 백그라운드 실행과 browser pthread는 미검증 |
| Windows / Linux | 동일한 표준 C++ 코어와 CMake 경로, 네이티브 CI 구성 추가 | 이번 로컬 환경에서 빌드·실행하지 않음 |

`otool -L build/libiiSDKCore.0.2.0.dylib`의 외부 의존성은 libc++와 libSystem이다. Qt·JVM·.NET 의존성이 없다. Android 결과물은 AArch64 ELF 공유 객체, 브라우저 결과물은 WebAssembly 바이너리임을 `file`로 확인했다.

## 실제 출력

macOS와 iOS 시뮬레이터의 재사용 테스트:

```text
reuse: 10000 iterations, operator new calls=0
```

macOS, iOS 시뮬레이터, 실제 브라우저의 라이브 예제:

```text
live runtime complete: produced=120 consumed=120 capacity=32 high_watermark=2
```

브라우저 계약 페이지 `iiSDKCoreBrowserTests.html`:

```text
browser contracts passed
```

## 검증한 의미론

- 큐 포화 거절, 같은 우선순위 FIFO, 빈 콜백/0 주기 거절, 1만 회 슬롯 재사용.
- 다른 루프 및 이전 세대 토큰 거절, 예약·실행 중 취소, 캡처 파괴 시 큐 재진입.
- 미래 타이머 조기 실행 방지, 최대 콜백 수/0 시간 예산, 주기 지연 건너뛰기.
- background 기아 방지, 재귀/동시 pump 거절, 예외 전파 후 복구.
- 4개 생산자 스레드의 400개 작업, 중단/재개, idle 종료, active 취소.
- 스트림 FIFO, 용량 제한, 최신 데이터 유지 정책, 종료 후 drain.
- 4개 생산자·2개 소비자가 용량 8의 스트림에서 400개 값을 각각 정확히 한 번 소비.
- 브라우저 중복 시작, 콜백 내 정지/재시작, 드라이버 파괴 시 프레임 취소, 예외 저장·조회.
- 객체 쿼리의 동기/큐 전달, 값/void/move-only 응답, 요청 값 보존, 수신 포트 만료, 처리 예외 보존, 취소, 큐 포화/종료, completion 예외, 실행 스레드 일치.

쿼리 모델 추가 후 macOS와 iOS 시뮬레이터에서 `object query complete: level=42`를 확인했다. Android ARM64에서도 쿼리 계약 및 객체 통신 예제의 교차 컴파일·링크를 완료했다. 설치 소비자는 설치된 Query 헤더를 포함하고 값 응답을 검사한다.

### 필요한 값만 선택하는 쿼리 계약 보강

객체 전체를 전달하지 않는 기본 계약을 문서에 명시하고 예제를 `ReadDevice{.level = true}`로 변경했다. 추가 검증은 macOS Release 6/6, ASan·UBSan 6/6, WASM/Node 4/4를 통과했다. 이번 보강에서는 iOS·Android·TSan을 다시 실행하지 않았으며 위 해당 플랫폼 기록은 이전 쿼리 모델 단계의 결과이다.

새 계약 테스트는 빈 선택이 전체 객체로 확대되지 않음, 선택한 0 값과 미선택 필드의 구분, 미선택 필드의 읽기·복사 생략, 큐에 저장된 선택 조건의 보존, 복사 불가능한 객체의 명시적 참조 선택, 부분 갱신에서 생략과 0/false의 구분을 검사한다.

```text
object query complete: level=42 name_selected=0 object_selected=0
```

### public/private 멤버 트리 구성

`examples/objects/Device.h`에서 public `audio.output`과 private `internal_.identity`, `audio.output.state_.history`를 실제 인스턴스로 구성했다. 각 계층의 값·일반 메서드·쿼리 포트 접근과 private 경로 차단을 `tests/member_tree.cpp`에서 컴파일 시 검사한다. 실행 시에는 상태 분리, 중첩 선택/응답, 큐 전달, 실패 시 상태 보존, 부모 소멸 후 자식 포트 만료를 검사한다.

이 단계는 macOS Release 7/7, ASan·UBSan 7/7, WASM/Node 5/5를 통과했다. Android ARM64 테스트·예제 빌드도 성공했다. iOS 시뮬레이터에서는 아래 두 실행 결과를 확인했다. Windows/Linux 원격 CI와 Android 장치 실행은 여전히 미검증이며 TSan 기록은 이전 쿼리 모델 단계의 결과이다.

```text
member tree contracts passed on iOS
object query complete: level=42 name_selected=0 object_selected=0 path=audio.output.level
```

## Queryable/designable 동적 인스턴스 속성

`ObjectProperties`를 Core의 공개 헤더·라이브러리·설치 패키지에 추가했다. `tests/properties.cpp`를 먼저 추가해 헤더 미구현으로 빌드 실패함을 확인한 후 구현했다. 생성 시 정의, 실행 중 값·타입·메서드·하위 트리의 추가/교체/삭제, 인스턴스 분리, public/private·queryable/designable/writable 정책, private/잠긴 자손의 상위 경로를 통한 우회 차단을 검사한다.

검증 실패 시 기존 값/revision 유지, 미선택 값의 복사 생략, 메타데이터와 값 조회 분리, 삭제 후 스냅샷 수명, 명시적 전체 객체 참조, mutable 메서드 상태 유지, 메서드 자기 삭제, 검증기 재진입 거절, 커밋 후 알림 조회, 알림 예외의 편집 결과 분리, 큐 전달 시점의 스키마 검증, 객체 소멸 후 수신자 만료까지 실행 검사했다.

| 검증 환경 | 이 변경의 결과 |
|---|---|
| macOS ARM64 Release 공유 라이브러리 | CTest 9/9 통과 |
| macOS ASan·UBSan, 경고를 오류로 처리 | CTest 9/9 통과 |
| WASM / Emscripten / Node | CTest 7/7 통과 |
| iOS ARM64 시뮬레이터 | 전체 타깃 빌드, 동적 속성 계약 테스트와 예제 실행 성공 |
| Android ARM64 API 26 | 전체 타깃 교차 컴파일·링크 성공; 장치 실행 미검증 |
| 별도 설치 소비자 | build/stage 설치 후 find_package, 새 공개 헤더·심볼 링크 및 CTest 1/1 통과 |
| Windows / Linux | 기존 CI 구성이 새 테스트를 포함한다. 이 변경의 원격 빌드·실행 미검증 |

macOS와 iOS 시뮬레이터의 예제 실행 결과:

```text
dynamic instance complete: gain=0.75 scaled=1.5 other_instance_unchanged=1
```

iOS 시뮬레이터의 계약 테스트는 `dynamic property contracts passed on iOS`를 출력하고 종료 코드 0을 반환했다. 검사 후 시뮬레이터를 종료했다. macOS 공유 라이브러리의 동적 의존성도 다시 확인했으며 libc++와 libSystem만 있고 Qt는 없다. 이번 동적 속성 변경에 대해 TSan, iOS 실기기, 브라우저 UI 실행을 새로 수행하지 않았다. 이전 단계의 해당 기록과 구분한다.

## 플랫폼 독립 Thread 추가 검증

Thread 계약 테스트를 먼저 추가하고 공개 헤더 부재로 빌드가 실패함을 확인한 뒤 구현했다. idle 상태, 실제 별도 스레드 실행, join의 결과 가시성, 실행 재사용, 중복 시작 거절, 중단 요청과 대기 깨우기, 예외 보존, 자기 join 거절, RAII 종료, idle/suspended EventLoop 중단, 루프 예외 전달, 다른 join과 동시 중단·상태 조회를 검사한다. 기존 다중 생산자·소비자 및 쿼리 스레드 테스트도 Thread를 사용한다.

| 환경 | 이 변경의 검증 결과 |
|---|---|
| macOS ARM64 Release | CTest 11/11 통과 |
| macOS ASan·UBSan 및 경고 오류 처리 | CTest 11/11 통과 |
| macOS ThreadSanitizer | CTest 11/11 통과 |
| WASM 단일 스레드 / Node | CTest 9/9 통과; Thread unsupported와 인라인 대체 실행 없음 확인 |
| WASM pthread / Node | CTest 9/9 통과; 실제 워커에서 스레드·다중 생산자/소비자·쿼리 실행 |
| 실제 브라우저 / WASM pthread | 스레드 계약 전체 실행과 exit=0, 메인 스레드 차단 거절 및 BrowserLoop 계약 통과 |
| iOS ARM64 시뮬레이터 | Thread·Runtime·Query 테스트와 ThreadExample 실행 성공 |
| Android ARM64 API 26 | 전체 타깃 컴파일·링크 성공; 장치 실행 미검증 |
| 별도 설치 소비자 | build/stage의 새 Thread 헤더·심볼로 스레드 실행·join 및 CTest 1/1 통과 |
| Windows / Linux | 기존 CI가 새 테스트를 포함하며 이번 변경의 원격 실행은 미검증 |

브라우저에서 tests/serve_wasm.py의 COOP/COEP 헤더를 사용하는 페이지를 열어 실제 표시 결과를 확인했다.

```text
crossOriginIsolated=true
thread contracts passed on WASM
exit=0

browser host blocking correctly rejected
browser contracts passed
```

iOS 시뮬레이터 실행 결과:

```text
thread contracts passed on iOS
runtime contracts passed on iOS
query contracts passed on iOS
managed thread complete: answer=42 background=1
```

브라우저 테스트 탭·로컬 서버·시뮬레이터는 검사 후 정리했다. macOS 라이브러리의 동적 의존성은 libc++/libSystem이며 Qt가 없다. GitHub Actions의 WASM job도 pthread OFF/ON으로 분리했다. Node 성공과 실제 브라우저 성공을 별도로 기록하며, iOS 시뮬레이터 결과를 실기기 검증으로 간주하지 않는다.

## 멀티스레드·병렬·비동기 기본 런타임 검증

Runtime/SerialQueue 계약 테스트를 먼저 추가하고 공개 헤더 부재로 빌드 실패를 확인한 뒤 구현했다. 기본 워커가 2개 이상인지, 독립 작업 및 서로 다른 객체 큐가 실제로 겹쳐 실행되는지, 같은 객체 큐는 우선순위가 달라도 FIFO와 최대 동시 실행 1을 유지하는지 검사한다. 4개 생산자의 동시 제출, 바인딩된 쿼리·동적 속성의 외부 동기 접근 거절, 완료 처리기의 실행 영역도 검사한다.

고정 용량 포화, 실행자·슬롯 세대별 취소 토큰, 객체 큐의 선두·중간 취소, 대기 작업 일괄 취소, 수신자 만료, 예외 이후 워커 유지, 자기 풀 대기 거절, RAII drain/join 및 Runtime 소멸 후 큐 제출 거절을 실행 검사했다. 기존 테스트도 함께 수행했다.

| 환경 | 이 변경의 검증 결과 |
|---|---|
| macOS ARM64 Release | CTest 13/13 통과 |
| macOS ASan·UBSan 및 경고 오류 처리 | CTest 13/13 통과 |
| macOS ThreadSanitizer | CTest 13/13 통과 |
| WASM pthread / Node | CTest 11/11 통과; 기존 캐시 옵션을 제거하고 새 기본값 ON도 확인 |
| WASM 명시적 단일 스레드 / Node | CTest 11/11 통과; Runtime 생성 거절과 인라인 대체 실행 없음 확인 |
| 실제 브라우저 / WASM pthread | 병렬 런타임 계약 전체와 exit=0, 메인 스레드 차단 거절 및 프레임 루프 계약 통과 |
| iOS ARM64 시뮬레이터 | 전체 타깃 빌드, 병렬 계약·메모리 재사용 테스트와 비동기 쿼리 예제 실행 성공 |
| Android ARM64 API 26 | 전체 타깃 컴파일·링크 성공; 장치 실행 미검증 |
| 별도 설치 소비자 | build/stage의 Runtime/SerialQueue 및 바인딩된 쿼리로 비동기 실행, CTest 1/1 통과 |
| Windows / Linux | 기존 CI가 새 테스트를 포함하며 이 변경의 원격 실행은 미검증 |

macOS와 iOS 시뮬레이터에서 확인한 출력:

```text
parallel runtime complete: workers=4 result=42 asynchronous=1
reuse: 10000 iterations, operator new calls=0
parallel reuse: 10000 iterations, operator new calls=0
```

재사용 측정은 준비된 큐에 작은 작업을 제출하고 완료하는 경로에 한정한다. 임의 payload·콜백·동적 스키마와 OS 할당 전체가 무할당이라는 의미는 아니다. iOS 계약 테스트는 `parallel runtime contracts passed on iOS`와 종료 코드 0을 반환했다.

실제 브라우저 표시 결과:

```text
crossOriginIsolated=true
parallel runtime contracts passed on WASM
exit=0

browser host blocking correctly rejected
browser contracts passed
```

macOS 공유 라이브러리 의존성은 libc++/libSystem이며 Qt가 없다. iOS 시뮬레이터, 브라우저 테스트 탭과 로컬 서버는 검증 후 정리했다. 네이티브 실기기와 원격 CI 결과를 시뮬레이터·교차 컴파일 결과로 대신하지 않는다.

## 동적 타입·명시적 캐스팅 검증

tests/value.cpp와 CMake 타깃을 먼저 추가하고 Value.h 부재로 빌드 실패를 확인한 뒤 구현했다. 정확한 타입과 사용자 DTO, 동적 재대입, const 참조 조회, 빈 값/null 구분, 정수 부호·범위, float→정수 경계, 64비트 정수 정밀도, 부동소수점 축소와 언더플로, 문자열 전체 파싱·형식 오류·왕복을 검사한다.

속성의 fixed/dynamic 정책, 명시적 쓰기 변환, 응답만 변환하는 선택 조회, 실패 시 값·revision 보존, 검증기와 private 경로, 미선택 값의 변환 생략 및 오류 경로도 검사했다. 객체 큐에서 100개의 동적 타입 변경과 DynamicValue 입력·응답 쿼리를 실행한다. 마지막 동적 응답 콜백은 const 참조 형태로 검증했다.

| 환경 | 결과 |
|---|---|
| macOS ARM64 Release | 전체 CTest 14/14 통과; 마지막 쿼리 콜백 수정 후 Value 테스트 재통과 |
| macOS ASan·UBSan 및 경고 오류 처리 | 전체 14/14 통과; 마지막 Value 테스트 재통과 |
| macOS ThreadSanitizer | 전체 14/14 통과; 마지막 Value 테스트 재통과 |
| WASM pthread / Node | 전체 12/12 통과; 마지막 동적 응답 쿼리 테스트 재통과 |
| WASM 명시적 단일 스레드 / Node | 전체 12/12 통과 |
| 실제 브라우저 / WASM pthread | value-browser.html의 동적 값·캐스팅·비동기 쿼리 계약 통과, exit=0 |
| iOS ARM64 시뮬레이터 | 전체 타깃 빌드, 동적 값·캐스팅 및 속성 계약 실행 성공 |
| Android ARM64 API 26 | 전체 타깃 컴파일·링크 성공; 장치 실행 미검증 |
| 별도 설치 소비자 | 설치된 Value.h와 cast_value 심볼로 문자열→정수 변환 실행, CTest 1/1 통과 |
| Windows / Linux | CI에 새 타깃이 포함되며 이번 변경의 원격 실행은 미검증 |

브라우저 실제 표시 결과:

```text
crossOriginIsolated=true
dynamic value and casting contracts passed
exit=0
```

iOS 시뮬레이터는 `dynamic value and casting contracts passed`와 `dynamic property contracts passed on iOS`를 출력하고 종료 코드 0을 반환했다. iOS 실행은 마지막 동적 쿼리 콜백을 const 참조로 변경하기 전이며, 캐스팅·속성 구현은 동일하다.

교차 검증에서 부동소수점 charconv의 WASM 지원 부재 및 iOS 배포 대상 가용성 제한을 발견하여 classic locale을 사용하는 스트림 변환으로 교체했다. WASM long double 문자열 출력에서 double 정밀도로 축소되는 결과도 확인했으므로 이 변환 쌍은 전 플랫폼에서 명시적으로 unsupported이다. long double 저장과 숫자 사이 변환은 지원한다. 일부 Emscripten expected<any> 값 전달의 컴파일 제약과 참조 콜백 사용법은 docs/dynamic-value.md에 기록했다.

검증용 서버·탭·시뮬레이터를 정리했다. 공유 라이브러리는 libc++/libSystem에만 의존하며 Qt 의존성은 없다. 기존 런타임 메모리 재사용 회귀 테스트도 통과했다.

## 네이티브 I/O 추상화 검증

tests/io.cpp와 CMake 타깃을 먼저 추가하고 IO/AsyncIO.h 부재로 실패함을 확인한 뒤 구현했다. 파일 생성·배타 생성, 바이너리 왕복, 부분 읽기, EOF, 빈 전송, 접근 모드, 잘못된 옵션의 비파괴 처리, seek/size/flush, 공유 채널 닫기와 닫힌 자원 재접근을 검사한다. 테스트 파일은 네이티브에서 build 아래에 생성·정리한다.

비동기 쿼리는 별도 워커 실행, FIFO seek/write/flush, 실제 저장된 바이트, 취소, 버퍼 범위 검사, 임시 Buffer의 요청 수명 유지, close의 별칭 반영을 검사한다. macOS에서는 테스트가 소유한 PTY master/slave를 생성하여 raw 시리얼 양방향 전송, 준비 대기, would_block/timeout, 실행 중 취소와 busy/close 보호를 실행 검증했다. 물리 USB/시리얼 장치를 검사한 것은 아니다.

| 환경 | 이 변경의 검증 결과 |
|---|---|
| macOS ARM64 Release | 전체 CTest 15/15 통과; 최종 수정 후 IO/Consumer/Reuse 3/3 재통과 |
| macOS ASan·UBSan 및 경고 오류 처리 | 전체 15/15 통과; 최종 관련 3/3 재통과 |
| macOS ThreadSanitizer | 전체 15/15 통과; 최종 관련 3/3 재통과 |
| WASM pthread / Node | 전체 13/13 통과; 최종 IO/Consumer 2/2 재통과 |
| WASM 명시적 단일 스레드 / Node | 전체 13/13 통과 |
| 실제 브라우저 / WASM pthread | 가상 파일 I/O와 비동기 쿼리 계약 통과, exit=0 |
| iOS ARM64 시뮬레이터 | 전체 타깃 빌드 성공. 실행은 외장 볼륨 open의 호스트 TCC 권한 판정 대기로 중단; I/O 실행 성공 미검증 |
| Android ARM64 API 26 | 전체 타깃 컴파일·링크 성공; 장치 실행 미검증 |
| 별도 설치 소비자 | 설치된 IO/Channel.h와 라이브러리로 실제 파일 읽기, CTest 1/1 통과 |
| Windows / Linux | 네이티브 백엔드와 CI 타깃을 구성했으며 이번 변경의 원격 빌드·실행은 미검증 |

macOS 직접 실행 결과:

```text
native serial PTY roundtrip passed
I/O contracts passed on macOS
I/O buffer reuse: 10000 iterations, operator new calls=0
```

재사용 측정은 한 번 연 파일과 확보한 Buffer로 작은 read/seek를 반복하는 경로이다. 임의 비동기 콜백·사용자 데이터·OS 내부 할당까지 무할당이라는 의미는 아니다.

브라우저 실제 표시 결과:

```text
crossOriginIsolated=true
I/O contracts passed on WASM
exit=0
```

iOS 시뮬레이터 대기 원인은 build/ios-simulator/build/io-sample.txt에서 main → run → open_file → open → __open 호출 스택으로 확인했다. 같은 PID에 대한 호스트 tccd AUTHREQ_ATTRIBUTION 로그에서 SimulatorTrampoline 책임 프로세스와 I/O 테스트 프로세스의 접근 요청을 확인했다. 권한 대기 중인 테스트 프로세스를 종료하고 시뮬레이터를 정리했으며, 뒤따르는 iOS 재사용 테스트는 실행하지 않았다.

WASM 도구 체인에서 std::stop_token 지원 부재를 확인하여, 모든 플랫폼에 같은 최소 atomic 기반 CancellationToken/Source 계약을 적용했다. 브라우저 탭과 검증 서버도 정리했다. macOS 공유 라이브러리의 의존성은 libc++/libSystem이며 Qt는 없다.

## 회귀 검사 경로

- `build/Testing/Temporary/LastTest.log`: 네이티브 Release.
- `build/sanitize/build/Testing/Temporary/LastTest.log`: ASan + UBSan.
- `build/thread-sanitize/build/Testing/Temporary/LastTest.log`: TSan.
- `build/wasm/build/Testing/Temporary/LastTest.log`: Node/WASM.
- `build/wasm-threads/build/Testing/Temporary/LastTest.log`: Node/WASM pthread.
- `build/installed-consumer/build/Testing/Temporary/LastTest.log`: 설치 소비자.
- `build/ios-simulator/build/*.app/`: 시뮬레이터에서 실행한 바이너리.
- `build/android-arm64/build/`: Android 교차 빌드 결과물.

생성물과 로그는 git에 포함하지 않는다. `.github/workflows/runtime.yml`은 macOS/Windows/Linux 공유·정적 빌드와 소비자 실행, iOS 라이브러리, Android 4개 ABI 교차 빌드, WASM 공통 테스트를 재현한다. 원격 CI 성공이나 모든 Linux 배포판/CPU 지원을 미리 선언하지 않는다.

ASan/UBSan 재현:

```sh
cmake -S . -B build/sanitize/build -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build/sanitize/build --parallel 4
ctest --test-dir build/sanitize/build --output-on-failure
```

TSan은 별도 `build/thread-sanitize/build`에서 `-fsanitize=thread`로 빌드한다. iOS simulator는 `CMAKE_SYSTEM_NAME=iOS`, `CMAKE_OSX_SYSROOT=iphonesimulator`, `CMAKE_OSX_ARCHITECTURES=arm64`로 빌드하고, 부팅된 시뮬레이터에 `xcrun simctl spawn <device-id> <executable-path>`로 각 테스트를 실행한다.

## 빈 폴더 기반 객체 구현 검증

`cmake/Foundations.cmake`에 등록한 공개 헤더 18개를 각기 독립 번역 단위로 컴파일했다. `src/` 내 `.DS_Store`를 제외하고 실제 파일이 없는 하위 디렉터리는 0개이며, 18개 공개 헤더 모두 `tests/foundations.cpp`의 소비 대상이다. 기반 객체별 책임과 제한은 [foundations.md](foundations.md)에 정의한다.

| 검증 구성 | 확인 결과 |
|---|---|
| macOS Release | 전체 17/17 통과 |
| macOS ASan + UBSan, 경고를 오류로 처리 | 전체 17/17 통과, 최종 PTY 테스트 수정 후 I/O 재통과 |
| macOS TSan | 전체 17/17 통과, I/O 테스트 20회 연속 재통과 |
| WASM pthread / Node | 전체 15/15 통과 |
| WASM 단일 스레드 / Node | 전체 15/15 통과. 병렬 경로는 지원 거절/생략을 검증하며 병렬 실행 증거가 아님 |
| 실제 브라우저 / WASM pthread | `foundations-browser.html`에서 기반 객체 계약 통과, `exit=0` |
| Android ARM64 API 26 | 라이브러리·공개 헤더·테스트·예제 컴파일 및 링크 성공 |
| iOS ARM64 시뮬레이터 | 라이브러리·공개 헤더·테스트·예제 컴파일 및 링크 성공 |
| 별도 설치 소비자 | 기존 소비자 및 18개 기반 객체 계약 2/2 통과 |

이번 기반 객체 변경에서 Android/iOS 장치 실행, Windows/Linux 원격 빌드·실행, 실제 SDL 호스트 및 Vulkan 장치 연결은 검증하지 않았다. 위 모바일 결과는 교차 빌드 결과이다. 물리 시리얼 장치 대신 macOS PTY로 네이티브 시리얼 바이트 경로를 검사한다.

브라우저에서 실제 관찰한 출력:

```text
crossOriginIsolated=true
foundation contracts passed on WASM
exit=0
```

네이티브 기반 객체 조합 예제의 직접 실행 출력:

```text
foundation pipeline complete: sensor.reading=42
```

경계 조건으로 Runtime 취소/종료 시 TaskGroup 작업 수 정리, 수락 거절, 작업 예외 전달, 그래프 순환/타 소유자/오래된 세대 차단, Scene 실행 영역과 실패한 부모 변경의 원자성, 함수 교체·자기 삭제, 호스트 초기화 실패, 타임라인 완료 역행·초과·재진입을 검사했다. GPU 완료는 주입 카운터 계약만 검사했으며 드라이버 실행으로 간주하지 않는다.

TSan 첫 실행에서 기존 PTY 테스트가 실패했다. `size()` 관찰자가 잠금을 잡은 순간 워커 `wait()`가 busy로 종료되어, 대기 진입을 확인하지 못하는 테스트 경쟁 조건이었다. 워커의 진입 busy를 재시도하고 취소·join 이후 판정하도록 수정했다. 이후 전체 TSan 통과 및 I/O 20회 연속 통과를 확인했으며, 제품 I/O의 잠금·거절 계약은 바꾸지 않았다.

설치는 `build/stage`에 수행했다. `~/.local` 설치본은 이번 변경에서 갱신하지 않았다. 공유 라이브러리의 의존성은 libc++와 libSystem이며 Qt/SDL/Vulkan 의존성은 없다. 브라우저 검증 탭과 로컬 검증 서버는 종료했다.
