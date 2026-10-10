# Forge 냉정한 평가와 Rust 110% 성능 개선 기획서

상태: 진행 중. 기준일 2026-10-09. 기준 compiler `f308497`, runtime `39ab3da`, stdlib `261ac79`, benchmarks `2ac8735`. 이전 세션의 평가·의미 분석·런타임·Lean 개선을 이어서 검증한다. 검증하지 않은 작업을 완료로 표기하지 않는다.

## 개발자의 판단

Forge는 C 기반 AOT, 실제 코루틴 런타임, 고정 버전 SDK/패키지 무결성, 자가 호스팅과 브라우저 실행을 갖춘 실험 언어다. 그러나 현재 safe ownership을 Rust 수준의 안전성으로 읽히게 표현할 근거는 없다. 알려진 서명 검사와 기본 의미 분석은 유용하나, 전체 수명/별칭/소유권 검사는 없다. 성능 향상으로 메모리 오류를 상쇄할 수 없으며, 잘못된 프로그램을 실행하지 않는 것이 첫 번째 출시 조건이다.

2026-10-08 평가 기록의 주요 개선은 이미 코드에 있다. 새로운 확인에서는 다음 반례가 남아 있다.

1. `own let payload: string = "literal"; send 0, Drop, move payload;`가 검사를 통과하나 런타임은 빌린 리터럴을 free하며 ASan에서 실패한다. move 이후 사용도 허용한다.
2. `fn choose(flag: int): int { if (flag) { return 7; } }`의 반환 누락을 --check/C/JS가 받아들인다.
3. 함수 이름을 정수 변수에 넣어 계산해도 함수 참조가 UNKNOWN 타입으로 처리되어 검사에 성공한다. JS는 NaN, C는 타입 경고가 발생한다.
4. 코루틴 100,000개의 현재 스케줄러 관측에서 워커 1/2/4의 wall 중앙값이 43.380/61.236/112.248ms다. 전역 잠금과 매 작업 완료 알림을 조사해야 한다. 이 3회 공유 호스트 관측은 원인을 단정하거나 Rust 우위를 증명하지 않는다.

## 목표 및 성능 판정 계약

사용자의 목표는 Rust 대비 **110% 처리 성능**이다. 동일 완료 작업 수에서 `Rust 시간 / Forge 시간 >= 1.10`으로 정의한다. 이는 Forge의 시간이 Rust보다 최소 약 9.09% 짧다는 뜻이며, 시간 10% 감소와 같은 기준으로 혼용하지 않는다.

판정에는 출력·작업량이 같은 CPU 연산, 문자열 접근·구축·할당, 코루틴/큐, 네이티브 병렬 작업, HTTP 요청 경로를 포함한다. 언어가 지원하지 않는 배열/vector API나 Rust와 의미가 다른 경로는 누락으로 공개하며 C FFI로 알고리즘을 대신 구현해 Forge 성능으로 집계하지 않는다. 이전 portfolio HTTP 측정은 DB JSON/차단 검사/libc/worker 조건이 달라 언어 성능 근거로 채택하지 않는다. 동일 조건으로 검증되지 않은 분야가 있으면 언어 전체 목표 달성으로 판정하지 않는다.

검증 순서: 동일 알고리즘·런타임 입력 → 독립 reference와 출력 대조 → release 최적화/CPU/라이브러리 조건 기록 → 예열 → 실행 순서 무작위 교차 반복 → wall/CPU/RSS 및 원시 표본 → paired ratio와 불확실성. 110% 판정은 반복 표본의 신뢰구간이 목표를 지지해야 한다. 한 종목의 큰 승리로 다른 종목의 퇴행을 숨기지 않는다. 빌드/테스트와 타이밍은 동시에 실행하지 않는다. 공유 호스트·실행시간이 짧은 종목은 제한을 명시한다.

Rust 옵션은 공식 rustc 문서를 기준으로 opt-level/LTO/codegen-units/overflow-checks/target-cpu를 기록한다. Forge의 빌린 문자열과 Rust의 소유 문자열, 불변 concat과 mutable builder를 같은 경로라고 취급하지 않는다. Rust가 이미 잘하는 알고리즘을 의도적으로 나쁘게 구현하거나, 검사를 제거해 얻은 C undefined behavior를 성능으로 인정하지 않는다.

## 수정 순서와 종료 조건

| 단계 | 작업 | 완료 증거 |
|---|---|---|
| P0 정확성 | 소유권이 입증되지 않은 payload의 해제/전달을 차단하고 유효한 소유 문자열 생성·이동·해제의 계약 구현; 이동 후 사용 및 분기 상태 검사 | 리터럴/arena/heap 생성·정상 전달·반송 실패·중복 이동·scope/반환·coroutine 수명 사례, ASan/UBSan, 기존 IPC 회귀 |
| P1 의미 분석 | 비외부 함수의 반환 경로와 함수 참조 타입 검증; stage0/자가 호스팅의 동작 차이 기록 및 해소 | 잘못된 입력은 check/C/JS에서 출력 파일을 보존하며 거부, 유효한 callback/분기/loop/match 허용, 기존 compiler/JS/selfhost suite |
| P1 산술 계약 | int64 overflow/division 정책을 명시하고 native/JS/constant folding의 일관성을 검증 | 경계값과 동적 연산 UBSan, 두 backend 대조, Lean 모델과 구현의 차이 기록 |
| P1 성능 | 완료 시 불필요한 scheduler wakeup 제거 후보를 측정; wakeup/stop/I/O/receive 불변식 유지 | 동일 revision·동일 바이너리 빌드 조건 before/after 반복, 정확한 완료 수, scheduler suite 및 await/restart/stop 사례 |
| P1 비교 근거 | 고정 입력 CPU/string suite와 동등한 concurrency/HTTP 비교 구축 | 입력/출력/source/toolchain 해시, 원시 표본, 실행 재현 명령, 불확실성·지원 누락 표시 |
| P2 개발 경험 | 비동기 진단, 정확한 위치, 타입·정의·참조 도구, native 소스 디버깅, capability 문서 | 편집 버전 뒤집힘/취소·정의 조회 회귀 및 실제 설치 consumer 검증 |
| 출시 게이트 | 새 runtime/stdlib pin, clean fetch SDK, native/JS/installed/selfhost 및 CI | 소스 override 없는 Release 빌드와 전체 CTest, selfhost fixed point, 외부 CMake consumer, GitHub CI |

각 단계는 실패 재현을 먼저 보존하고 수정 후 통과를 확인한다. 부분 구현은 범위와 미해결 사례를 공개한다. P0를 단순히 경고로 바꾸거나 기능을 숨기는 것을 소유권 구현 완료로 계산하지 않는다. 여러 저장소의 기존 사용자 파일·운영 서비스·인증 설정은 변경하지 않는다.

## 성능 목표가 불가능하다고 판단할 때

실패한 한 번의 벤치마크를 불가능의 증명으로 사용하지 않는다. 별도 `docs/rust110-feasibility.md`에 고정한 목표 범위, 실제 측정값, 병목의 근거, 시도한 개선/실패한 가설, 장비·알고리즘·API의 제약과 반증 가능한 후속 실험을 기록한다. 모든 프로그램에서 무조건 Rust보다 10% 빠르다는 약속과, 대표 suite의 실용 목표는 구분한다. 후자의 가능 여부가 미확정이면 미확정으로 남기고 목표를 낮춰 완료 처리하지 않는다.

## 출처와 이전 세션

- [이전 평가 및 수정](evaluation-and-fixes-2026-10-08.md)
- [parser 할당 개선](refactoring-performance-2026-10-06.md)
- [rustc codegen 옵션](https://doc.rust-lang.org/rustc/codegen-options/index.html)
- [Rust 정수 연산 계약](https://doc.rust-lang.org/reference/expressions/operator-expr.html)
- [이전 애플리케이션 비교의 한계](https://github.com/forge-language/forge-benchmarks/blob/main/docs/rust-forge-comparison-2026-10-04.md)

## 실행 기록

기획서 작성 후부터 아래에 검증된 변경·측정만 추가한다. 현재 전체 목표는 미완료다.

### 정확성 통합

- stage0는 비외부 non-void 함수의 도달 가능한 반환 누락을 거절한다. 상수 참
  무한 loop와 내부 loop의 break, 마지막 wildcard match를 구분한다.
- 함수 참조를 UNKNOWN 스칼라와 구분한다. 직접 opaque 외부 ABI와 단순
  전달 wrapper의 callback 인수 호환만 유지하며, 실제 외부 callback 서명은
  여전히 입증하지 않는다. 일반 정수 초기화·계산·출력·반환에 함수 참조를
  넣는 입력은 check/C/JS에서 거절한다.
- `send ... move binding`은 초기화된 `own` string만 받으며 이동 후 사용,
  중복 이동, 분기·match·loop의 재이동을 검사한다. C 생성기는 literal/arena
  원본을 free하지 않고 heap 사본을 mailbox에 전달하며 binding을 NULL로
  만든다. 일반 move 표현식은 타입·수명 계약이 없으므로 거절한다.
  **이것은 안전한 IPC 경계 사본의 부분 구현이다. P0 전체 ownership 완료,
  zero-copy 또는 Rust 수준의 lifetime/alias 안전성을 주장하지 않는다.**
  borrowed alias는 원본을 읽을 수 있고 integer process handle의 유효성은
  런타임/FFI 계약에 의존한다. 이 제한은 README와 예제 문서에도 반영했다.
- JS가 지원하지 않는 native ownership/send/coroutine 입력은 check에서는
  native-valid로 허용한다. JS 생성은 임시 stream에서 먼저 완료하므로
  backend 거절이 기존 출력 파일을 잘라내지 않는다. 디스크 I/O 실패의 완전한
  atomic output 계약을 주장하는 것은 아니다.
- 통합 Release 빌드, 15개 CTest, selfhost fixed point 통과. 새 ownership
  suite는 생성 C를 ASan/UBSan으로 실행하여 literal/arena 반송 실패 1000회,
  실제 receiver의 독립 heap 사본·해제, borrowed alias, yield 후 전송을 확인했다.
  전체 runtime은 별도 Release 및 ASan/UBSan 3개 suite를 통과했다.
- stage2 추가 반례는 unknown name, 잘못된 initializer, 잘못된 인수 개수,
  반환 누락을 C 출력까지 허용한다. 고정점이 semantic parity를 뜻하지 않는다는
  기존 문서 설명은 유지한다. 이 parity 작업은 아직 미완료다.

### 성능 및 배포 근거

- runtime 완료 알림은 동일 mutex 아래에서 `active_coros == 0 &&
  native_pending == 0`일 때만 발생한다. I/O 등록 신호와 stop broadcast는
  유지했다. mixed native/coroutine/delayed I/O 회귀를 추가했다.
- runtime `ddeba40e8c57b4e8bc46f273c519a4c497fe2a56` 공개 후 GitHub CI가
  성공했다. compiler의 runtime pin도 이 commit으로 갱신했다.
- 100,000개 코루틴의 15쌍 비교에서 워커 1/2/4 paired 중앙값 개선은
  1.887/1.569/1.577배다. 이는 **기존 Forge와의 비교**이며 Rust 비교가 아니다.
- Rust CPU/string 5종목의 첫 비교는 모두 110% 목표를 입증하지 못했다.
  불변 append와 builder의 큰 격차를 확인했으며, 동일 workload 재측정과
  문자열 구현 개선을 진행한다. 원시 표본과 근거는 별도
  [목표 검토 문서](rust110-feasibility.md)에 기록한다. 불가능 확정은 하지 않는다.

### 하위 에이전트 실행

사용자가 허용한 병렬 작업을 실제 환경의 4개 동시 slot(root 포함)에 맞춰
3개 native 하위 에이전트로 진행한다. OMO용 `opencode` 실행은 현재 설정의
`openai/gpt-5.6-sol` 모델을 찾지 못해 실패했다. 해당 호출을 성공한 OMO
작업으로 계산하지 않으며 계정·provider 전역 설정을 바꾸지 않았다.

### 2026-10-10 계속 진행

- 이전 공개 compiler `979f860`의 GitHub compiler CI와 container CI 모두 통과.
  문자열 view의 ABI를 유지한 inline 경로는 stdlib `e919bbe`로 공개됐으며
  stdlib CI도 통과했다.
- native/JS/folding의 wrapping int64 계약, guarded division/remainder를
  통합했다. 산술 oracle/UBSan 8개 suite와 optimizer 회귀 통과. 기존 parser의
  unary `-0.0`가 positive zero가 되는 문제는 별도 미해결 항목으로 확인했다.
- 소스 override 없이 공개 runtime `ddeba40`/stdlib `bd89481`를 새로 fetch한
  Release 빌드에서 17개 CTest, selfhost fixed point, 설치 SDK의 FG/C 소비자
  3개를 통과했다. compiler `78faaf1` 공개 후 compiler/container CI도 모두 통과했다.
- 문자열 view inline 결과와 네이티브 OS thread 비교를 추가했다.
  [목표 검토 문서](rust110-feasibility.md)에 원시 결과·범위·미달을 기록했다.
- TypeScript LSP의 blocking compiler 실행을 async/취소/버전·설정 generation
  검사로 바꿨다. 네이티브 FG LSP는 아직 동기식이며 이 차이를 문서화한다.
  18개 protocol·shutdown 회귀와 패키징 검토를 통과했으며 `f22383b`로 공개 후 CI도 통과했다.
- Builder byte append의 checked inline fast path는 stdlib `bd89481`로 공개 후 CI를 통과했다.
  같은 세션의 18회 교차 측정에서 기존 view-only 경로 대비 1.5059배
  (95% 구간 1.4897–1.5090), Rust 대비 0.9571배로 목표에는 미달했다.
  exported ABI, 경계 검사, snapshot 계약을 유지하고 inline caller의 ASan/UBSan 회귀를 통과했다.

### 정확한 소스 진단 checkpoint

- stage0 AST에 원본 byte span을 유지하고 parse/semantic 오류를 파일별 UTF-16
  범위로 전달한다. imported/transitive 선언과 decoded 문자열의 원본 위치를
  포함한 16개 진단 회귀를 통과했다. 기본 메시지의 첫 줄과 이전 compiler
  호환성을 유지하며 선택적 `--diagnostics-json`을 추가했다.
- 정상 종료의 AST parameter 누락 해제를 수정했다. 가져온 함수·extern 선언이
  있는 check/symbols/C 출력의 ASan/UBSan/LSan 및 declaration cleanup 회귀 통과.
  오류 즉시 종료의 전체 자원 정리나 모든 compiler 경로 검증을 의미하지 않는다.
- TypeScript LSP는 `2381fbe`에서 위치 정보를 중복 없이 반영하고 imported 오류는
  원본 파일을 메시지로 전달한다. 21개 테스트와 CI 통과.
- 로컬 override SDK에서 새 OS/framing primitives를 포함한 20개 CTest 통과.
  native FG event-loop, process-group cancellation 및 JSON Unicode 처리는
  검증 중이며 이 checkpoint의 공개 compiler 의존성은 검증된 `bd89481`을 유지한다.

### 네이티브 비동기 LSP 통합

- stdlib `8b896aa`에 POSIX 비동기 프로세스·별도 stdout/stderr·프로세스 그룹
  취소/회수, 단조 시계와 부분 입력을 받는 bounded LSP framing을 공개했다.
  JSON 문자열은 Unicode escape와 surrogate pair를 UTF-8로 복원한다.
  NUL을 포함한 헤더/본문, 중복 Content-Length는 거절한다.
- native/main.fg가 문서 버전·재열기 epoch·설정 generation, 150ms debounce,
  4개 child 제한, 공유 symbol 요청, 취소·timeout·종료 정리를 관리한다.
  C bridge에는 문서나 LSP method 처리를 넣지 않았다. POSIX 구현은 Linux에서
  검증했으며 Windows에서는 TypeScript 서버를 사용한다.
- 등록된 native async protocol 14개와 실제 compiler 진단 4개를 통과했다.
  한글/보충 Unicode 뒤의 UTF-16 범위, parse 위치, colon/Unicode 경로의
  imported 오류를 한 개 진단으로 전달한다. 문자열 API 출력 타입 등록,
  URI/control 문자 JSON roundtrip, compiler signal 오류도 수정했다.
- compiler/runtime 21개 CTest와 selfhost 고정점, 별도 runtime/stdlib
  ASan/UBSan 10개 suite 및 instrumented native LSP의 위 18개 protocol
  검사를 통과했다. TypeScript 21개 테스트도 통과했다.
- 동일 SDK로 만든 serial/native event-loop의 controlled 1.2초 compiler
  지연에서 hover 중앙값은 5개 표본씩 1202.36ms/0.410ms였다. 전체 원시
  표본·바이너리 hash·재현 script는 language-server 보고서에 있다.
  이것은 편집기 응답성 관측이며 Rust 110% 또는 compiler 처리량 증거가 아니다.
- P0 전체 ownership, stage0/stage2 의미 검사 동등성, Rust 대표 suite의
  전체 110% 목표는 여전히 미완료다.
- stdlib의 해당 공개 commit은 GitHub CI를 통과했다. source override 없는
  새 Release SDK에서도 21개 CTest와 selfhost 고정점을 통과했고, 그 SDK를
  설치한 뒤 native LSP 4개 suite 및 설치한 doctor의 실제 FG compile/link/run,
  initialize/shutdown 검사를 통과했다.
