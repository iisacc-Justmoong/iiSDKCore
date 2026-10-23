# 공통 시간 곡선

`ii::animation::ease`는 정규화한 진행률을 값 진행률로 바꾸는 C++23 함수이다.
기존 linear·in_quad·out_quad·in_out_quad·smooth와 함께 in_cubic·out_back을 제공한다.
입력 진행률은 0~1로 제한하고 양 끝은 정확히 0과 1이다. out_back의 중간 결과는 1을
넘을 수 있다. 불투명도처럼 범위가 제한된 소비자는 해당 결과를 자신의 영역에서 제한한다.

in_cubic은 `p³`, out_back은 `1 + (p-1)² × ((s+1)×(p-1)+s)`이다.
세 번째 인자의 overshoot 기본값은 1.70158이며 비유한 값도 이 기본값을 사용한다.
표준 시간 곡선의 수학적 계약이며 Qt·GUI·타이머 서비스에 의존하지 않는다.
`iiSDKCore::Values`만으로 사용하고 기존 Keyframes·Timeline에도 새 enum을 지정할 수 있다.

프레임 기반 소비자는 [FrameLoop](frame-loop.md)의 경과 시간으로 p를 계산한다.
일반 키프레임 또는 [Animation](modern-core.md)의 시간 기반 재생도 같은 곡선을 사용한다.
이 함수는 프레임을 만들거나 작업을 예약하지 않는다. 앱·게임 모두 같은 시간/실행 계약을
선택하며 물리 시간 간격, UI의 실제 시간과 게임 배속 시간은 호출자가 결정한다.

`tests/easing.cpp`는 다항식의 알려진 샘플, 입력 제한, 단조 cubic, back의 실제 overshoot,
Keyframes·Timeline 통합과 Values-only 사용을 검사한다. LVRS의 1.45 overshoot와
등장/퇴장 시간 선택은 해당 상위 프레임워크에 속하며 Core에 기본값으로 하드코딩하지 않는다.
