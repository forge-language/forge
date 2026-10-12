# Rust 110% 성능 목표의 현재 근거와 한계

2026-10-09 진행 기록. **현재 목표 미달이며, 실용 목표가 불가능하다고 확정하지 않았다.**
이 문서는 목표를 낮추거나 작업 완료를 선언하기 위한 문서가 아니다.

동일한 일을 완료할 때 Rust 시간 / Forge 시간이 1.10 이상이어야 한다.
대표 CPU·문자열·동시성·네이티브 병렬·HTTP 경로를 검증해야 하며 한 종목만
빠르다고 언어 전체 목표 달성을 주장하지 않는다. 입력과 결과가 다른 과거 HTTP
애플리케이션 비교는 이 판정에서 제외한다.

## 첫 CPU 비교

기준 compiler `f308497`, 기존 Release SDK 바이너리의 불변 복사본을 사용했다.
실행 당시 다른 저장소 수정이 진행 중이었으므로 측정 바이너리·archive 해시와
관측한 소스 상태는 원시 보고서에서 구분한다. 같은 런타임 인수, 독립 Python
reference, 공통 C monotonic clock, 실행 순서 무작위 교차, 15쌍 측정이다.

| 종목 | paired 처리 성능 중앙값 | bootstrap 95% 구간 | 판정 |
|---|---:|---:|---|
| 정수 LCG | 1.0091 | 1.0074–1.0136 | 미달 |
| 소수 탐색 | 1.0130 | 0.7468–1.3844 | 잡음이 커 미확정 |
| 문자열 scan | 0.7859 | 0.7452–0.8219 | 미달 |
| 문자열 builder | 0.5497 | 0.5371–0.5584 | 미달 |
| 불변 문자열 append | 0.2340 | 0.2281–0.2632 | 미달 |

[전체 CPU 결과와 재현 조건](https://github.com/forge-language/forge-benchmarks/blob/main/docs/cpu-rust-forge-2026-10-09.md),
[원시 CPU 표본](https://github.com/forge-language/forge-benchmarks/blob/main/docs/cpu-rust-forge-2026-10-09.json).
이 표는 개선 전 구현의 관측이며 수정 후 결과로 재사용하지 않는다.

## 확인된 개선과 그 범위

스케줄러는 매 작업 완료 때 메인 스레드를 깨우는 대신 완료 조건을 충족할 때만
깨우도록 수정했다. 같은 100,000개 코루틴 작업의 15쌍 비교에서 워커 1/2/4의
paired 성능 중앙값은 기존 Forge 대비 1.887/1.569/1.577배다. 별도 관측에서
메인 스레드 wait 수가 4307/6095/11765에서 각각 1로 줄었다.
이는 Forge 내부 개선 근거이며 **Rust 동시성 비교 결과가 아니다**. 4워커의
실행 시간은 여전히 1워커보다 길다. 공유 호스트·짧은 실행·단일 큐 구조의 한계가
있으므로 전체 병렬 확장성을 해결했다고 해석하지 않는다.

[스케줄러 결과 및 원시 자료](https://github.com/forge-language/forge-benchmarks/blob/main/docs/scheduler-completion-notify-2026-10-09.md).

## 목표를 아직 판정할 수 없는 이유

문자열 scan/builder는 Forge의 정적 라이브러리 호출이 out-of-line인 반면 Rust의
표준 라이브러리 연산은 inline될 수 있다. 불변 append는 Forge arena가 중간
문자열을 reset까지 유지하는 반면 Rust는 이전 소유 문자열을 더 일찍 해제한다.
이 차이는 실제 API·메모리 관리 비용이므로 숨기지 않는다. 원인별 개선과 동일
workload 재측정이 필요하다. 숫자 계산의 near-parity만으로 추가 10%를 얻을 수
없다고 단정할 수도 없다.

배열/vector API, 동등한 코루틴/큐 및 HTTP suite가 아직 없다. CPU 표의
신뢰구간은 해당 호스트·실행의 불확실성을 나타내며 다른 머신에 대한 보증이
아니다. 완전한 ownership/lifetime 검사나 native 정수 overflow 계약도 아직
완료되지 않았다. 잘못된 실행이나 검사를 빼서 얻은 속도는 개선으로 인정하지
않는다.

## 다음 반증 가능한 실험

1. 문자열 접근·증설·snapshot의 비용을 분리하고 실제 구현 병목을 수정한 뒤
   같은 입력·reference·도구 체인으로 재측정한다.
2. 정확한 int64 계약과 optimizer를 검증한 뒤 CPU 코드를 다시 비교한다.
3. 같은 작업량·완료 확인·워커 조건의 Rust 동시성 및 HTTP 기준을 추가한다.
4. clean pinned SDK와 다른 장비에서 반복한다. 개선 한계가 확인되면 실패한
   시도와 제약을 이 문서에 추가하고, 실용 목표의 불가능 여부를 다시 판단한다.

## 2026-10-10 후속 검증

stage0의 native add/sub/mul, 상수 folding을 기존 JS와 같은 modular int64
계약으로 수정했다. 잘못된 정수 division/remainder는 명시적으로 실패한다.
O0/O3 UBSan, native/JS Python oracle 대조, 부동소수점·라이브러리·코루틴
회귀를 포함한 8개 산술 테스트를 통과했다. stage2는 이 계약을 아직 공유하지
않는다. [산술 계약](integer-arithmetic.md)에 범위를 명시한다.

문자열 view의 경계 검사를 유지하면서 호출자에 inline 경로를 노출했다.
새로 컴파일한 scan/builder의 Rust 대비 처리량은 각각 0.9662/0.6373이다.
compiler/runtime/stdlib archive 해시는 동일하나 개선 전후 실험 날짜가 다르므로
인과적 paired improvement 신뢰구간으로 사용하지 않는다.
[상세 비교](https://github.com/forge-language/forge-benchmarks/blob/main/docs/string-view-inline-2026-10-10.md).

네이티브 OS thread creation/join 비교도 추가했다. 동일 총 64,000,000회 LCG
연산을 1/2/4개 물리 코어에 분배한 16쌍 측정에서 paired 처리량 중앙값은
1.0055/1.0104/1.0304이며 95% 구간은 1.0036–1.0105,
0.9915–1.0533, 0.8783–1.1715다. 모든 reference와 완료 수 검사를 통과했지만
110% 목표를 입증한 행은 없다. 이 측정은 원래 CPU baseline의 불변 SDK를
사용했으며 최신 산술 수정의 결과가 아니다. coroutine/mailbox 또는 HTTP
비교로 일반화하지 않는다.
[전체 조건과 원시 결과](https://github.com/forge-language/forge-benchmarks/blob/main/docs/native-thread-rust-forge-2026-10-10.md).

Builder byte append의 checked inline 경로를 같은 세션에서 18회씩 세 가지
구현의 순서를 균형 있게 교차해 측정했다. 기존 view-only Forge 대비
1.5059배 (95% 구간 1.4897–1.5090), Rust 대비 0.9571배
(0.9489–0.9606)다. 다른 네 workload는 실질적인 개선을 입증하지 않았다.
[원시 결과와 조건](https://github.com/forge-language/forge-benchmarks/blob/main/docs/builder-byte-inline-2026-10-10.md).

현재는 실용 목표 미달이며 불가능 여부는 미확정이다. 최신 SDK의 재측정,
동등한 coroutine/queue·HTTP 비교와 다른 호스트 반복이 남아 있다.

## 코루틴의 전체 실행 수명 비교

동일 물리 코어 집합의 1/2/4개 작업 스레드에서 Forge와 Tokio 1.53.0을
비교했다. 관찰 버퍼·런타임 생성, 작업 생성, 모든 작업 완료, 종료와 완료
검사를 타이머에 포함했다. 18개 소규모 reference 검사, 288회 prewarm 및
288회 측정에서 각 작업 ID의 결과와 완료 수가 독립 Python reference와
일치했다. 스케줄러 중심 및 reduction budget을 넘는 사례의 처리량 구간은
해당 구성에서 1.10을 넘었다. 그러나 CPU 혼합 사례의 paired 중앙값은
1.0715/1.0466/1.0013으로 전체 목표 달성을 입증하지 못했다.

Forge는 reduction budget 안에서 yield 이후 계속 실행할 수 있으며 Tokio의
`yield_now`는 별도 정책을 가진다. Forge는 작업을 먼저 큐에 넣고 작업 스레드를
시작하며 Tokio는 스레드 생성 후 작업을 넣는다. 따라서 논리 yield 횟수가 같아도
동일 context switch 횟수를 측정한 결과가 아니다. mailbox·HTTP·I/O 및 다른
호스트에 일반화하지 않는다.
[전체 조건과 원시 결과](https://github.com/forge-language/forge-benchmarks/blob/main/docs/coroutine-rust-forge-2026-10-10.md).

## 2026-10-12 동일 세션 후속 실험

로컬 검증 SDK의 wrapping-int64 baseline과 문자열 변경, 나눗셈 변경 및
두 변경의 조합을 불변 바이너리로 보존하고 같은 Rust control과 20개
위치 균형 paired group으로 비교했다. 문자열 builder의 내부 NUL 저장을
최종 snapshot까지 미루고 checked append/view 경로를 inline한 후보는
기존 Forge 대비 1.1060배 (95% 구간 1.0884–1.1088)였다. 기존 Rust의
snapshot-copy control 대비 1.0037배였으며 1.10은 입증하지 못했다.
LCG/소수/scan/immutable append는 1.0096/1.0111/0.9454/0.2300배였다.

기존 Rust control을 바꾸지 않고 마지막 builder clone을 move로 바꾼
더 강한 기준도 따로 측정했다. 별도 20개 paired group에서 개선 Forge는
copy/move control 대비 각각 1.0006 (0.9909–1.0063),
0.9986 (0.9814–1.0029)였다. builder를 유지하며 독립 사본을 만드는
계약과 builder를 소비하는 계약의 차이를 공개하며 두 결과 모두 미달이다.

동적 unsigned32 나눗셈과 branch-likelihood 후보는 부호·폭 6개 분포의
추가 실험에서도 충분한 이득을 보이지 않았다. 작은 양수/음수/혼합 입력은
더 느려졌으며 기본 컴파일러에서는 원래 checked int64 연산을 복구했다.
새 동적 경계·trap·단일 평가 회귀는 유지한다. 실패 후보의 패치와 모든
표본을 보존하여 성공한 실험으로 계산하지 않는다.

새 prepared HTTP 비교 V1은 14번째 pair에서 실패하여 전체 비교 주장을
만들지 못했다. 마지막 Rust 측정에서 12,000개 요청 중 2개가 TCP connect
timeout으로 끝났다(각 약 134초). 연결 지연과 오류 행을 보존하고 제거하거나
다시 실행하지 않았다. 원인 진단은 미확정이다. 다음 V2 관측기는 connect
timeout과 단계별 시간, 즉시 저장되는 요청 결과를 제공한다. 추가로 동시
접속량별 오류율·초당 완료 요청·지연시간을 기록해 시험 범위 내 수용량을
보고할 예정이다. 전체 parser/routing/TLS/DB/코루틴 I/O 비교는 아니다.

원시 자료와 진행 보고서는 로컬 `forge-benchmarks/docs/`의
`rust110-progress-2026-10-12.md`,
`compiler-string-optimization-2026-10-12.json`,
`builder-copy-move-controls-2026-10-12.json`,
`arithmetic-division-2026-10-12.json`에 있다. 새 공개 CI 또는 clean-fetch
검증을 수행한 결과로 표현하지 않는다. OMO audit의 API 연결 실패도
보존했으며 native 하위 에이전트 3개로 별도 병렬 작업을 진행했다.

다음 후보인 증명된 native/library loop increment 간소화는 독립 코드
검토를 마쳤지만 아직 빌드·경계 테스트·동일 조건 성능 측정 전이다.
현재 목표는 미완료이며 활성 상태를 유지한다.


## 2026-10-12 prepared HTTP capacity follow-up

The bounded V2 observer passed 14/14 validation gates and 8/8 observer tests. A 4-worker loopback sweep covered concurrency 16–512, 32- and 4096-byte response bodies, and eight repetitions per configuration. All 320 prewarm/measurement samples passed exact response and EOF checks with zero failed request rows. The tested error-free bound is **at least 512 simultaneous client lanes** for both payload sizes; the absolute capacity was not established. Median successful throughput ranged from 2,958–4,240 requests/s (32-byte body) and 3,732–4,210 requests/s (4096-byte body) for Forge; Rust measured 3,720–4,326 and 3,950–4,254 requests/s, respectively. Paired variability is wide, so these measurements do not establish a language performance winner or the 1.10 target. This is a closed-loop local prepared-response, connection-per-request test with observer logging included, not a general user-capacity, keepalive, full-framework, TLS, routing, or database result. [Conditions and table](https://github.com/forge-language/forge-benchmarks/blob/main/docs/http-v2-capacity-2026-10-12.md), [raw report](https://github.com/forge-language/forge-benchmarks/blob/main/docs/http-v2-capacity-2026-10-12.json).


## 2026-10-12 compiler loop proof and sustained HTTP follow-up

A narrowly proven induction-increment codegen candidate was measured against the immediately preceding compiler with identical runtime/stdlib archives and CPU/Rust fixtures. Across 18 position-balanced pairs per CPU/string workload, candidate/baseline throughput medians were 0.9980 (LCG), 1.0043 (primes), 0.9978 (scan), 0.9963 (builder), and 0.9933 (immutable append); the intervals did not establish a reliable improvement, and no Rust/Forge row reached 1.10. The candidate is not reported as a performance win. [Full paired evidence](https://github.com/forge-language/forge-benchmarks/blob/main/docs/compiler-loop-proof-recheck-2026-10-12.md).

A longer prepared-HTTP V2 comparison stopped after seven complete pairs: the Rust prewarm in pair 8 ended with 64 TCP connect timeouts among 991 retained attempts. The shared network namespace's conntrack table was already near its configured maximum before the sample (261,217/262,144) and reached the maximum afterward. Process attribution is unproven; the run is `failed_no_claim`, and successful partial pairs are not aggregated. Earlier V2 capacity results covered 1,024 requests per trial, so they do not erase this longer-run instability. [Failure evidence and limits](https://github.com/forge-language/forge-benchmarks/blob/main/docs/http-v2-primary-2026-10-12.md).

## 2026-10-12 builder metadata store-order experiment

An inline fast-path candidate wrote the appended byte before publishing the new
builder length, testing whether changing the metadata dependency order improved
the pre-reserved builder loop. Sixty paired samples each for scan and builder
found no scan change (candidate/baseline 0.9955, 95% interval 0.9923–1.0056)
and a possible builder regression (0.9642, 0.9146–0.9999). The candidate was
discarded. Both Forge rows remained below Rust in this run (0.9416 and 0.8699).
[Conditions and raw samples](https://github.com/forge-language/forge-benchmarks/blob/main/docs/builder-length-publication-2026-10-12.md).

## 2026-10-12 current CPU suite recheck

Thirty paired samples of the current frozen compiler and SDK against one Rust
control confirmed 1.012/1.011 throughput for LCG/primes, 0.948/0.968 for
scan/builder, and 13.557 for the specifically lowered immutable-append case.
Only that narrow immutable-append workload exceeds 1.10; the four other rows
remain below target. Every checksum matched the independent reference. This is
one host and one run, and does not establish language-wide 110% performance.
[Conditions and raw samples](https://github.com/forge-language/forge-benchmarks/blob/main/docs/cpu-current-2026-10-12.md).

## 2026-10-12 weighted-scan recurrence

The CPU kernel's repeated per-byte `% 13` weight was replaced by an equivalent
bounded weight recurrence in both Forge and Rust. The comparison harness now
allows distinct equivalent source variants only with a source-matched Rust
control per candidate; all implementations must match the independent oracle
for prewarm and timed inputs. The Python oracle now models truncating signed
remainder across zero correctly, and the Forge CLI parser accepts negative
seeds. Three unit tests and 1,188 signed-seed Forge/Rust differential runs
passed for scan, builder and immutable append.

In the final 40 position-balanced pairs over the five CPU workloads, recurrence
Forge reached 1.938x source-matched Rust throughput on scan (95% interval
1.917–1.953) and 16.257x on immutable append (13.422–18.098). Builder's point
estimate was 1.124x with a wide interval (0.991–1.210), so this run does not
establish 1.10 there. LCG and primes stayed near Rust at 1.007x and 1.011x.
The recurrence expresses a more efficient algorithm in both benchmark
implementations; it is not an automatic compiler optimization. Results are
host-specific and do not complete the representative concurrency/HTTP and
cross-machine audit. [Paired conditions and raw samples](https://github.com/forge-language/forge-benchmarks/blob/main/docs/cpu-weight-cycle-2026-10-12.md).

## Builder buffer transfer API

Added `str_builder_finish_take`, which returns the builder's arena buffer as an
immutable NUL-terminated string and resets the builder for reuse. The original
`str_builder_finish` snapshot behavior is unchanged. The compiler now uses the
transfer operation only for its proven private dead-temporary append-loop
lowering; JavaScript and the C ABI expose matching behavior. Native, ABI-only,
and JavaScript tests verify snapshots survive builder reuse. This change has
not yet been isolated in a paired performance run, so it is not counted as a
measured speedup.
