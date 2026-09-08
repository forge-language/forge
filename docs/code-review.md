# Forge adoption and code review

Reviewed on 2026-09-08. Scope: first-use compiler CLI, build instructions,
onboarding, and adjacent risks. This is not a complete compiler, concurrency,
or production security audit. Existing compiler/runtime changes were preserved.

## Findings and changes

| Priority | Finding | Result |
| --- | --- | --- |
| P1 | Unknown flags and missing values silently succeeded; multiple source inputs silently selected the last input. | Fixed in `compiler/main.c`: explicit diagnostics and nonzero exit. |
| P2 | `--help` and `--version` failed; version text disagreed with CMake. | Added stdout help/version and sourced the version from CMake. |
| P2 | First-use instructions were separated from the first runnable program. | Added a compiler/runtime-only quickstart, expected output, troubleshooting, and contributor checks. |
| P2 | No registered CTest CLI regression checks. | Added `cmake/CliSmoke.cmake` and CTest registration. |
| P1 | `-L` was parsed but not forwarded to the native linker, so external-library consumers failed to link. | Fixed in `compiler/driver.c`; installed-prefix regression in `cmake/ExternalSmoke.cmake`. |
| P1, resolved upstream | Temporary-file reservation was released before opening the suffixed path. | Pull brought in `b2ca100` secure suffix handling; retained it when reconciling local changes. The earlier local finding is no longer open. |

## Verification

- Before the fix, `--help` and `--version` exited 1. The misspelled flag
  `--check examples/hello.fg --chek`, a trailing `-o`, and two inputs exited 0.
- The new regression script failed before the fix with
  `help: expected exit 0, got 1`; it passed after rebuilding.
- Full default build, including bundled examples, completed successfully.
- CTest: `100% tests passed, 0 tests failed out of 2` (CLI scenarios and an
  installed external-library consumer). CI now runs these checks and Hello.
- External-library RED: `/usr/bin/ld: cannot find -lforge_greeting_ext` despite
  an existing archive and correct `-L` in the generated build command. GREEN
  after forwarding search directories: `Hello from an external project, Forge`.
- Manual native compilation and execution printed `Hello from Forge!` and `42`.
  Coroutine and match examples also completed. Help works with an unavailable
  `CC`, and version output is `Forge 0.3.0`.
- Linux was exercised. Windows/macOS execution and CMake 3.16 itself were not
  available for verification; contributor commands avoid newer `ctest --test-dir`.
- A clean archive of the staged code also passed a full build and both tests,
  proving the improvement does not depend on unrelated local modifications.
- Existing warnings remain: deprecated OpenCL queue API and unused threading
  variable (plus an unused event-loop helper in the clean upstream-based tree).
  Lean proofs and hosted GitHub Actions were not executed in this session.

## Review ledger

Initial base: `d2f1f417d2b2b7ef8e97477e2e333099c68b649a`.
Requested pull created merge `e08c559e0a9283c45c508883ca532f8f11566b64`,
including 24 upstream commits while preserving the local commit. Conflicts in
stashed local edits were resolved by retaining both sets of APIs and local
hardening, with the newer secure temporary-file implementation taking priority.
The backup stash remains available: `90a4a040c84f6818833b9fdbfdc13d52bf999f75`.

Initial review results below are historical, not coverage of the later merge.
Final SHA-specific review results and cleanup receipts are recorded in the
session notepad at `.omc/ulw-20260908-DU5aUO.md`.

| Lane | Verdict | Source |
| --- | --- | --- |
| Code quality | PASS | `/root/code_review`, final report: CLI parsing and CTest checked; exact version regex and C emission coverage are nonblocking improvements. |
| Security | PASS (scoped edits) | `/root/security_review`, final report: no new invocation/input risk; existing temporary-file race recorded above. |
| Goal | Correction applied | `/root/goal_review` flagged the CMake/CTest minimum mismatch, corrected to run CTest from the build directory. |
| Hands-on QA | Historical checks | Root ran native Hello, coroutines, match, and CLI errors before pull; all passed. |
| Context | PASS | `/root/context_review`: CLI consumers remained compatible; temporary-C feature wording corrected. |

## Adoption priorities beyond this change

1. Broaden automated native runtime regression coverage and a platform build
   matrix before making stronger compatibility claims.
2. Reconcile this monorepo with the split repositories and publish a clear
   supported installation/release path. Distribution was not changed here.
3. Measure whether new users can build and run a first program successfully.
   These changes remove observed friction; increased adoption is not yet measured.
