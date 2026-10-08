# Forge evaluation and corrective work — 2026-10-08

This assessment separates demonstrated behavior from intended language properties.
It covers the inspected compiler interface, standard-library packaging, project
manager, editor integration, learning material and CI. Repository count, a working
website and a passing example are not measurements of ecosystem maturity. No
numerical maturity score is assigned.

The baseline findings below describe the workspace before this corrective work.
A later patch does not retroactively make those original limitations nonexistent.
Verified fixes must be recorded separately, with the command/test that checked them.

## Assessment baseline

### Editor support is functional, but does not yet provide semantic development tooling

Both servers advertise completion, hover and document symbols. The TypeScript
completion handler concatenates keyword, type, standard-library and document
symbol lists; symbol locations are recovered from the first matching name in the
text, and hover uses a static word/documentation lookup. Neither inspected server
advertises definition, references, rename or formatting support. A protocol
connection and a completion list therefore demonstrate working editor integration,
not project-wide symbol resolution or type-aware refactoring.

Evidence: `language-server/src/server.ts` initialization at lines 85–91,
completion at 119–123, symbol-location mapping at 135–145, and hover at 164–186;
`language-server/native/main.fg` capabilities at 389–396 and request dispatch at
743–785. These are concrete implementation limits, not a claim that LSP support
is absent.

The compiler interface originally described `--check` as “Parse only”. Both
servers use that operation for diagnostics. Consequently, a clean editor buffer
could not establish semantic validity, ownership correctness or memory safety.
The TypeScript server also launches the compiler synchronously, with a ten-second
limit, for every content change: a slow check can block that server's request
processing. A future asynchronous/incremental implementation must preserve
version ordering and reject stale diagnostics, not merely move work to a thread.

Evidence: `forge/compiler/main.c` usage at 52;
`language-server/src/forge.ts` compiler invocation at 27–42 and check at 59;
`language-server/src/server.ts` content-change validation at 101–110;
`language-server/native/main.fg` diagnostics at 255–274.

A read-only baseline probe of the existing installed SDK executable at
`/home/helloworld0822/coding/forge-sdk/bin/forge` (SHA-256
`f18d88c11239331c60a08129e432b40d0edc44f51ad6679b454ce8768826e8ef`)
ran `--check` on three programs:

```forge
native main { println(missing_name); }
native main { let value: int = "wrong"; println(value); }
fn add(a: int, b: int): int { return a + b; }
native main { println(add(1)); }
```

The first two snippets were separate programs, and the final two lines formed
one program. All three returned status 0 with empty stderr. This is observed
false-negative behavior from that installed baseline compiler, not a claim about
a subsequently patched compiler. No generated program was executed.

### Packaging has real integrity controls and a deliberately limited resolver

The project manager writes exact versions and commits into `forge.lock`, rejects
cycles and conflicting versions, verifies downloads/checkouts, and requires
explicit trust before native package builds. These are useful production-oriented
foundations. Its resolver accepts exact `x.y.z` versions rather than version
ranges, permits one version of each named module, and bounds graphs at 64 modules
and 32 levels. It is not a general dependency solver with alternative-version
selection or compatibility-range reasoning. The manifest's license field is
validated as text; that alone is not a license-compatibility analysis.

Evidence: `forge-platform/cli/src/manager.fg` download verification at 27–53,
trust at 117–118, resolution at 139–149 and lock publication at 151–157;
`forge-platform/backend/src/manifest.fg` version grammar at 9–18, dependencies at
44–46 and license validation at 43. For existing projects, reproducibility and
correct failure behavior matter more than adding package-manager commands.

### Distribution and compatibility coverage are narrower than the component APIs

The public installer explicitly supports Linux x86_64 with glibc 2.35 or newer.
Compiler, runtime and standard-library code contain additional platform branches,
but those branches are not evidence of released, tested toolchains on all of
those platforms. Core and platform CI inspected here run on `ubuntu-latest`.
Checksums prevent accidental substitution relative to the published checksum;
the installer documentation correctly states that same-host checksums are not
independent signed attestations.

Evidence: `forge-platform/scripts/install.sh` platform check at 43;
`forge-platform/README.md` installation integrity statement at 57–66;
`forge/.github/workflows/ci.yml` at 11–20 and
`forge-platform/.github/workflows/check.yml` at 4–6. Stable compiler/runtime ABI,
a supported-version compatibility policy and multi-platform release verification
are prerequisites for broader operational adoption; CMake package version files
alone do not demonstrate them.

### Standard-library capabilities need precise contracts and broader verification

The standard library is a consumable CMake package with a pinned runtime dependency
and installed headers/libraries. Optional OpenSSL, OpenCL and io_uring support
changes runtime capabilities. One inspected message claimed missing OpenSSL
“falls back to plain HTTP”, while the actual no-OpenSSL TLS entry points return
`-1` from listen and do nothing in serve. The implementation does not establish
an automatic TLS-to-plaintext downgrade; the message was inaccurate. Applications
need documented unavailable-feature/error behavior and a capability check, not
assumptions inferred from the existence of an API name.

Evidence: `forge-stdlib/CMakeLists.txt` options at 30–32, optional dependency paths
at 70–116 and installed package setup at 132–147;
`forge-stdlib/src/http_tls.c` no-OpenSSL branch at 290–302. Direct stdlib CTest
registration covered string, OS and filesystem tests at CMake lines 148–159.
Other integration tests exist in the compiler/platform, but this unit-test list
is not comprehensive coverage of HTTP/TLS, failure recovery, GPU or platform
compatibility.

### Debugging and onboarding are useful starting points, not a mature ecosystem

The inspected compiler exposes emitted C and intermediate-file retention.
That enables manual investigation through host tools, but no Forge source-level
debugger adapter is contributed by the inspected VS Code extension, and no
Forge debug/source-map mode appears in the compiler's published CLI options.
Generated C debugging can help maintainers; it is not equivalent to reliably
setting breakpoints, inspecting Forge variables and reading Forge stack traces.

Evidence: `forge/compiler/main.c` options at 38–54;
`forge/compiler/driver.c` optimization construction at 74–83;
`vscode-extension/package.json` contributed language/editor configuration.

The new course has eight lessons and separately checked challenge solutions.
Native and emitted-JavaScript execution checked all 16 programs, producing 32
matching outputs. This is valuable onboarding evidence for that supported subset.
It does not validate every compiler feature, every standard-library operation,
production applications or safety guarantees. The course explicitly avoids
browser-inapplicable native APIs and acknowledges incomplete type/ownership
checking. Newly added guides, editors and tests should be judged as new tools;
they do not supply a history of independent users, compatibility migrations or
incident recovery.

Evidence: `forge-learning/course.json`, `forge-learning/solutions.json`,
`forge-learning/tests/test_learning.py` and its README at 59–98.

## Comparison with established languages

| Reference | Established property | Forge evidence and practical implication |
| --- | --- | --- |
| Rust | Compile-time reference/borrowing rules prevent dangling references and conflicting mutable borrows in safe code. | Ownership syntax and message transfer express a similar goal, but the inspected compiler has no comparable alias/lifetime analysis. Forge cannot claim Rust-level safety. |
| Go | A specified memory model defines synchronization, and signed integer overflow has defined behavior. | Lightweight concurrency is implemented, but scheduler examples and isolated deadlock fixes do not supply a complete memory model or arithmetic policy. |
| C toolchains | Native compilation and UBSan can diagnose selected runtime errors, including signed overflow. | Emitting C enables native integration and sanitizer testing. It also means native arithmetic still needs an explicit overflow policy; safe constant folding alone does not make execution safe. |

References: [Rust references and borrowing](https://doc.rust-lang.org/book/ch04-02-references-and-borrowing.html),
[Go memory model](https://go.dev/ref/mem),
[Go integer overflow](https://go.dev/ref/spec#Integer_overflow),
[Clang UBSan](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html).
These are comparisons of documented properties, not measurements of relative speed.
No broad performance ranking follows from the existing benchmarks.

The engineering judgment is that Forge is an experimental native language with
useful runtime/tooling foundations. It is suitable for controlled experiments
and compiler development. The inspected evidence does not justify choosing it
as a general replacement for Go or Rust in systems that require their existing
safety and operational guarantees. The philosophy can be preserved by improving
semantic checks and runtime contracts before expanding syntax or APIs.

## Demonstrated strengths

- Immutable compiler/runtime/stdlib revisions and installed CMake consumers make
  component builds reproducible and expose broken installation assumptions.
  Core CI actually builds and runs external consumers after installation.
- Package source inspection, exact commits, archive hashes, bounded dependency
  graphs and explicit native-build trust are substantive controls.
- Real Sublime portable-profile tests exercised separate syntax/helper archives,
  110 syntax assertions, indentation/comments and real native/TypeScript LSP
  initialization and completion. These test results support those specific claims.
- Learning examples and challenge answers run in both native and JavaScript
  backends, with actual compile-failure, timeout and wrong-output regressions.
- CI exercises installer/project-manager behavior in addition to source builds;
  honest documentation already distinguishes experimental properties and
  checksum integrity from stronger guarantees.

## Corrective-work record

The resumed session completed and verified the following local corrective work:

- **Stage0 semantic pass:** `compiler/semantic.c` runs after module loading and
  before optimization, symbol output, `--check`, native compilation and JavaScript
  emission. It rejects unknown lexical values/functions, visible function/coroutine
  arity and type errors, incompatible initialization/assignment/returns,
  uninitialized locals, invalid loop control and suspension outside coroutines.
  It checks thread callback signatures before C casts. Typed declarations from
  source modules and extern functions participate; binary/stdlib API signatures
  remain opaque. Invalid expressions in dead branches or multiplied by zero are
  checked before optimization can erase them. Rejected programs preserve existing
  output files. `tests/compiler_test.py` covers 17 invalid fixtures in three modes
  (51 checks), imported signatures, accepted initialization and lexical shadowing.
- **Runtime integration:** the existing published runtime correction
  `39ab3daa90f15852cbbf4dd97f5d4c1502bd392d` fixes nested indexed-pool waits and
  consumes owned payloads rejected by full mailboxes. The compiler dependency pin
  now selects that commit. The standalone stdlib dependency pin is also updated;
  the compiler selects stdlib revision
  `261ac791443b0f857cd8bc0b3e72c174f9526527`. `scheduler_regressions` exercises
  single-/multi-worker nested waits,
  coroutine and external-thread batches, full mailboxes and other scheduler paths.
- **Lean 4:** `Forge.BoundedMailbox` proves capacity preservation, FIFO admission,
  rejection without queue mutation and the rejection disposal token.
  `Forge.CheckedInt64` proves accepted-result bounds and zero/minimum-integer
  division/remainder guards. Evaluation and optimization use truncation toward
  zero, including negative remainder; all existing proofs build with Lean 4.14.0.
  `scripts/check-correspondence.py` checks 27 Lean/emitted-C cases: 17 safe
  expressions execute under UBSan; 10 rejected expressions are inspected only,
  never compiled or executed. The report is reproducible with the command below.
- **Optional TLS messaging:** the local stdlib configure message now describes
  unavailable TLS (`listen` returns `-1`) rather than claiming a plaintext fallback.

Validation commands (all successful in the resumed workspace):

```sh
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
cmake --build build --target forge-selfhost-verify -j4
# In ../forge-proofs:
lake build
python3 scripts/check-correspondence.py \
  --forge ../forge/build/bin/forge --include ../forge/build/include \
  --report /tmp/forge-correspondence-20261008.json
```

CTest passed all 14 registered suites, including compiler, JavaScript, self-host,
module merge, driver, component integration, runtime and stdlib regressions.
A separate Release build fetched the pinned GitHub component revisions without
local source overrides and built all examples and stage2 successfully; its 14
suites and self-host fixed point also passed. Installing that build into a fresh
SDK prefix, building `examples/external-project` against it and running the Forge,
runtime and stdlib consumers all succeeded.

The semantic pass is deliberately incomplete: it does not implement full
ownership/alias/lifetime checking, standard-library signatures or proof that
non-void functions return on every path. Initialization joins are conservative
and can reject valid programs whose initialization depends on loop execution,
exhaustive `match` or an early-return branch. The separate stage2 `forge-fg`
compiler does not share this stage0 pass. Diagnostics lack source spans. Native
compilation can defer opaque API signatures to C, while JavaScript emission has
no equivalent host signature check.
The Lean results prove explicit mathematical models, not refinement of the C
implementation, heap disposal, race freedom or scheduler liveness. Runtime
integer overflow remains a separate unresolved language/backend policy.
Compiler, stdlib and proof changes are tracked in their respective repositories.
These tests establish the listed behavior rather than broader safety guarantees.
The runtime correction itself was already published before resume.

The most useful next production gates are semantic failure detection before
execution, runtime bounds/recovery tests, precise optional-feature behavior,
reproducible supported-platform releases, semantic editor/navigation support and
source-level debugging. Adding more repositories or highlighting more keywords
would not substitute for those gates.

## 한국어 요약

Forge는 실제 실행·패키지 검증·에디터 연결·학습 자료를 갖춘 실험적인 프로젝트입니다.
하지만 작동하는 예제와 많은 저장소가 안정적인 언어 생태계를 증명하지는 않습니다.
현재 점검에서 확인한 주요 제약은 의미 분석에 기반한 오류 검출과 에디터 기능,
정확한 버전만 처리하는 의존성 해석, Linux 중심 배포·검증 범위, 선택 기능의 오류
계약 및 Forge 소스 수준 디버깅입니다. 새로 추가한 기능의 테스트 결과는 인정하되,
오랜 실사용·호환성 유지·장애 복구 경험과 구분해야 합니다. 아래 개선 기록은 실제
검증이 끝난 변경만 완료로 표시해야 합니다.
