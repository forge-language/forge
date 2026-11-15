# Scalar semantics and backend conformance

Status: **Accepted — policy** (2026-10-19). D1–D4 and the existing
integer/conversion/context rules are Accepted. Implementation completion is not claimed.

Decision owner: Forge human maintainers. Draft author: implementation agent.
Scope: plan task 1; stage0 native C and JavaScript, with explicit selfhost exclusions.
The human user approved D1, D2, D3 and the existing rules on 2026-10-19, then
explicitly authorized the revised D4 option C after review of alternatives.
Both decisions are recorded below under [RFC_PROCESS.md](../../RFC_PROCESS.md).
Implementation of these accepted policies is authorized within their stated scope;
unrelated ABI changes and other RFCs, including ownership, are outside this approval.
Policy acceptance does not establish implementation completion or satisfy the
executable evidence requirements below.

Implementation checkpoint, 2026-10-22: D1 shortest binary64 output, D2 streamed
println arguments and D4 exact mixed comparison are implemented in stage0.
`scalar_conformance_test.py`, `float_format_test.py` and `arithmetic_test.py`
exercise native/JS behavior; `selfhost_arithmetic_test.py` covers the independent
integer subset. Baseline observations below retain their original chronology.
See [backend conformance](../backend-conformance.md) for current support limits.

## Problem and evidence boundary

The semantic checker accepts scalar combinations that independent backend
representations do not always implement identically. Existing conversion fixes
make `half(5)` produce 2.5 at a float boundary, but do not settle float text,
noncanonical bool values, mixed comparisons, or output interleaving.

Source observations below describe the stage0 baseline at `9893ed7`, before
implementation of the newly accepted D1/D2/D4 changes, including the existing
float-conversion, native `%.17g`, and SDK-signature fixes in this worktree. Source inspection is not runtime proof. The evidence ledger is
[scalar-rfc.md](../../.omo/evidence/contracts-20261017/scalar-rfc.md);
runtime observations are separately identified there. No benchmark, fixed point,
or passing subset is evidence of full-language equivalence.

## Current behavior and policy decisions

“Accepted” in the program tables means the compiler accepts a program, not that
a maintainer has accepted a policy. Policy acceptance is recorded separately in
the decision table: D1–D4 and the existing rules are Accepted. The tables still
distinguish current backend behavior from the accepted implementation targets.

| Rule | Observed baseline stage0 behavior and source | Policy (decision status below) | Backend scope |
| --- | --- | --- | --- |
| Scalar representation | Native `c_type` in `compiler/codegen.c`: int → `int64_t`, float → `double`, bool → C `int`. JS `expression`: integer → BigInt, float → Number, boolean literal → Boolean. | Preserve int64 and binary64 value models. Preserve C `int` bool ABI; retain legacy values under accepted D3 below. | Native/JS; selfhost has no float. |
| int/bool → float | `semantic.c:compatible` accepts both. C parameter/local/result types perform conversion; JS `converted_expression` and extern wrappers apply Number. | Preserve conversion at initialization, assignment, argument, return, and declared extern boundaries, evaluating the source once. Loss of precision above 2^53 is allowed binary64 rounding. | Native/JS supported calls; opaque headers retain native C boundary. |
| int ↔ bool | `compatible` accepts either direction. Native C `int` can retain 2 and narrow wide int64 values; JS does not generally coerce declared bool bindings. | Preserve acceptance and ABI for compatibility. Do not call all bool values canonical 0/1. D3 below resolves the portable value domain before checked ABI validation. | Native/JS differ outside canonical values. |
| float → int/bool | Known float values fail `compatible` for int/bool targets. | Preserve rejection; any explicit conversion API needs separate range/NaN policy. | Native/JS semantic check. |
| Numeric arithmetic | `semantic.c:expression` accepts int/bool/float and gives arithmetic float type when either operand is float. Native helpers dispatch on actual C types; JS `arithmetic` promotes Number when either operand is Number. | Preserve promotion and wrapping; record conversions explicitly in later typed lowering. | Native/JS; unknown ABI types need actual declarations. |
| Mixed comparisons | Native C performs its numeric conversions. JS `eq` Number-promotes mixed Number/BigInt; `lt/le/gt/ge` directly compare operands. | Accepted D4 option C below: exact comparison of int64 and stored binary64 values. Arithmetic promotion remains unchanged. | Native/JS; selfhost excludes floats. |
| Integer overflow | `optimize.c:fold_int` and native unsigned helpers wrap add/subtract/multiply to signed int64. JS `wrap` uses `BigInt.asIntN(64, ...)`. | Preserve wrapping, including intermediate results and unary negation/compound assignment lowering. Do not introduce overflow traps or reassociation that changes results. | Stage0 native/JS only. |
| Integer division/remainder | Truncate toward zero; remainder has dividend sign. Zero divisor or INT64_MIN with -1 fails for both `/` and `%`. Optimizer leaves invalid constant operations for runtime. | Preserve nonzero exit and stderr containing `Forge integer division`; JS stack trace and exact status number are not shared text contracts. | Stage0 native/JS only; native FFI internals excluded. |
| Short circuit | Native C operators and JS Boolean wrappers preserve `&&`/`||` short circuit. | Preserve skipped effects/traps and once-only evaluation of evaluated operands. | Native/JS and supported selfhost cases. |
| Float remainder | Known float operand to `%` rejected by `semantic.c:expression`. | Preserve rejection. | Native/JS. |
| Constants | `semantic.c:expression(EXPR_IDENT)` checks locals first and recursively rechecks a constant initializer in each caller context. No permanent initializer-type memoization. Emission has narrower support: native `emit_consts` requires folded literals; JS emits top-level const bindings. | Preserve existing contextual **checking** behavior, including check-only acceptance. Do not invent runtime substitution for nonliteral constants. Resolve each use separately in typed lowering; changing lookup policy needs another RFC. | Stage0 checker; output/backend limits remain explicit. |
| FFI | Explicit extern and mapped SDK signatures participate in checking; unknown binary results remain UNKNOWN. Native output uses actual C declarations; `FORGE_PRINT_AUTO` selects by actual C type. | Keep native ABI and declared float conversions. UNKNOWN means unverified, not void, canonical bool, or safe resource. No name-based ABI guessing. | Native C ABI; only implemented JS mappings/extern facilities. |

## Floats, special values, and text

Numeric model for the accepted baseline: binary64 arithmetic, preserving observable signed zero;
NaN compares unequal to itself and ordered comparisons with NaN are false;
float division by zero yields infinity/NaN under the supported IEEE host
configuration rather than invoking the integer trap. Do not promise NaN payload
bits or NaN sign preservation. Implementation requires explicit host assumptions
and acceptance tests; arbitrary C implementations and fast-math modes are not
covered. `optimize.c:fold_binary` currently declines float division by zero and
nonfinite folded results; `simplify_binary` restricts identity rewrites to known
integers to avoid erasing float promotion or signed-zero behavior.

Native `emit_print_auto_macro` emits `printf("%.17g", value)`. JS
`codegen_emit_js` emits `String(x)` for Numbers. Therefore identical numeric values
do not imply identical bytes. The following spellings are host-format examples,
not a newly adopted portable contract:

| Value/example | Native formatter | JS Number formatter | Accepted D1 target |
| --- | --- | --- | --- |
| 0.1 | Typically `0.10000000000000001` | `0.1` | Accepted D1 decision below. |
| 18446744073709551616.0 | Typically `1.8446744073709552e+19` | `18446744073709552000` | Accepted D1 decision below; same rounded binary64 value. |
| Negative zero, produced by dividing 0.0 by -1.0 | Typically `-0` | `0` | Preserve value sign; use `-0` text under accepted D1. |
| Positive/negative infinity | Typically `inf` / `-inf` | `Infinity` / `-Infinity` | Use `Infinity` / `-Infinity`. |
| NaN from 0.0/0.0 | C library spelling/sign varies | `NaN` | Use `NaN`, with no payload/sign text. |

D1 options: (A) retain backend-specific text and document that parity means numeric
value, (B) choose a common shortest-round-trip formatter, or (C) require identical
17-significant-digit formatting in both backends. **Accepted (2026-10-19): B**, with
ASCII decimal point, no locale variation, shortest decimal round trip, lowercase
`e`, explicit exponent sign, no exponent leading zeroes, fixed notation for
magnitudes from 1e-6 inclusive to 1e21 exclusive, and exponential notation outside
that interval. Print integral finite values without `.0`; print signed zero and
special values as above. The implementation must select and validate a formatter
with deterministic tie handling before claiming byte parity. A is the least
implementation work but leaves divergent output; C retains familiar native text
but adds visible decimal noise and requires a JS formatter. No formatter or
locale behavior is changed by this decision record alone.

## Evaluation and visible effects

Native `emit_expr(EXPR_CALL)` special-cases println as a sequence of
`FORGE_PRINT_AUTO(arg);` statements followed by a newline. JS calls a variadic
helper: its arguments finish evaluating before joining and printing the line.
Both evaluate println arguments once, in source order; their **printing** order
relative to nested effects differs. For the nested example below, native output
is `first\n1.5second\n2.5\n`; JS output is
`first\nsecond\n1.52.5\n`. The native-only test
`test_print_arguments_evaluated_once_in_source_order` intentionally records the
native behavior and cannot establish cross-backend parity.

D2 options: (A) make JS stream each argument immediately, preserving native
behavior, (B) make native evaluate all arguments before printing, matching JS,
or (C) retain explicitly different output contracts. **Accepted (2026-10-19): A**,
including the prefix already printed when a later argument traps. Lowering must
not duplicate evaluation or evaluate a later argument early; a per-argument
emission sequence is needed, not another eager variadic call. B may require
values/borrows to survive evaluation of later arguments and changes existing
native output. Concurrent stdout atomicity is outside this scalar proposal.

Ordinary calls and arithmetic are different: native emits C call arguments and
helper calls without general sequencing temporaries. C does not establish a
left-to-right order for these arguments. JS evaluates call arguments left to
right. A result observed with one C compiler is not a language guarantee.
**Recommendation:** keep general relative operand/argument order unspecified,
preserve once-only evaluation and short circuit, and prohibit clients from
relying on conflicting effects within unsequenced expressions. Implementing a
general ordered-evaluation language change requires a separate decision. Use
separate statements to establish portable effect order. This RFC does not claim
that unspecified order makes conflicting C-side mutations safe.

## Bool validity and ABI decision

D3 options: (A) retain legacy C-int bool values, explicitly limiting portable
cross-backend value claims to representable shared cases, or (B) normalize every
int→bool/FFI bool boundary by nonzero→1, zero→0 while preserving C `int` ABI.
**Accepted (2026-10-19): A for this compatibility baseline.** Keep canonical literals,
comparisons, and logical results at 0/1; record noncanonical inputs as legacy
behavior, not invalid Rust-style bool bit patterns. In particular a C `int`
value 2 is not inherently invalid C, and conversion from an out-of-range int64
must not be described as portable truncation. B provides a cleaner future checked
boundary but changes the observable result of `let b:bool=2; println(b);`.
The maintainer must decide whether checked facades later normalize, reject, or
explicitly preserve noncanonical FFI values; an UNKNOWN signature cannot answer
that question. Scalar checking does not establish callback ABI/lifetime safety.

## D4 alternatives and accepted choice: mixed comparisons

The user requested a better alternative on 2026-10-19, then authorized the
revised recommendation with “D4도 진행” (“Proceed with D4 too”). **Accepted
(2026-10-19): option C**, comparing the exact int64 value with the exact value
represented by the stored binary64 operand, without first rounding the integer
to binary64.
This retains mixed-comparison syntax and avoids collapsing distinct integers near
2^53. Arithmetic promotion remains unchanged. A float literal has already been
rounded to binary64; comparison cannot recover its original decimal intent.

| Option | Example: `9007199254740993 == 9007199254740992.0` | Compatibility and implementation tradeoff |
| --- | --- | --- |
| A: convert both operands to binary64 (original recommendation) | `true`, because the integer rounds down | Simplest alignment with native C promotion and arithmetic; loses integer distinctions and would change current JS ordering near 2^53. |
| B: reject mixed comparisons; require explicit conversion | Compile-time rejection | Makes precision choices explicit, but rejects currently accepted programs and requires a separate explicit conversion API/range policy. |
| C: compare represented values exactly (Accepted 2026-10-19) | `false`; the integer is also greater than the float | Preserves accepted source syntax and integer precision; changes native mixed comparisons and current JS mixed equality, requiring native helpers and numeric-domain-only JS equality handling. |

The revised recommendation was based on Forge's compatibility and precision
requirements; the explicit follow-up authorizes option C. There is established precedent: Python specifies exact-value
comparison across numeric types, while separately promoting mixed arithmetic.
See [Python numeric types](https://docs.python.org/3/library/stdtypes.html#numeric-types-int-float-complex).
ECMAScript compares BigInt/Number mathematical values for ordering and finite
mixed equality; see [IsLessThan](https://tc39.es/ecma262/multipage/abstract-operations.html#sec-islessthan)
and [IsLooselyEqual](https://tc39.es/ecma262/multipage/abstract-operations.html#sec-islooselyequal).
Adopting that numeric behavior would not authorize JavaScript's broader string,
Boolean, null, or object coercions as Forge equality rules.

For option B, Rust primitive `f64` provides `PartialEq<f64>` and `PartialOrd<f64>`;
it does not implicitly compare an `i64` operand with an `f64` operand. See the
[Rust primitive f64 comparison implementations](https://doc.rust-lang.org/std/primitive.f64.html#impl-PartialEq-for-f64).
This precedent is specific to those primitive types; user-defined comparison
trait implementations can support different operand types.

For option C, implementation must cover all six operators (`==`, `!=`, `<`, `<=`,
`>`, `>=`) and both operand orders, preserving once-only evaluation. NaN remains
unordered: `!=` is true; equality and every ordered comparison are false. Signed
zeroes compare equal. Infinities, fractional values, both int64 endpoints, and
positive/negative boundaries around 2^53 require explicit cases. Do not implement
this with an epsilon, an assumption that `long double` is wider than `double`,
an unchecked float-to-int cast, or an integer-to-double conversion before the
comparison. Native helpers need a portable range/classification strategy; JS
must restrict any mixed equality adjustment to the intended numeric domains.

The independent executable comparison study is documented in
[d4-comparison-study.md](../../.omo/evidence/resume-20261019/d4-comparison-study.md).
The study informs implementation planning; it does not implement D4 or establish
a performance benefit. **D4 policy is Accepted.**

Stage0 D4 implementation was verified on 2026-10-20: native O0/O3 with UBSan and
float-cast-overflow instrumentation and Node matched a Python exact-comparison
oracle for 10,621 pairs (127,452 boolean results per backend run). The full
36-suite CTest run, scalar regressions, selfhost fixed point and installed SDK
consumers passed. See [implementation evidence](../../.omo/evidence/resume-20261020/d4-implementation.md).
This completes the scoped comparison change; D1 formatting, D2 streaming and the
planned typed-graph backend migration remain separate work.

## Runnable corpus and accepted/rejected programs

Save each complete source below as the indicated name. From the worktree after
building the SDK, use the following invocation for accepted native/JS sources:

```sh
build/bin/forge case.fg --emit-c -o case.c
cc -std=c11 -O0 -Werror -fsanitize=undefined -fno-sanitize-recover=all -I build/include case.c -L build/lib -lforge_std -lforge_runtime -pthread -lm -o case.native
./case.native
build/bin/forge case.fg --emit-js -o case.js
node case.js
```

Repeat the C command with `-O3`. Capture exit status, stdout, stderr, source and
generated files. For rejected sources use `build/bin/forge case.fg --check`;
require nonzero status and the listed diagnostic. For contextual constants,
check-only is the asserted surface, not successful C/JS program generation.

```forge
// conversion.fg
fn half(x:float):float{return x/2;}
fn five():float{return 5;}
native main{let x:float=true;println(half(5));println(five()/2);
x=7;println(x/2);return 0;}
```

```forge
// wrapping.fg
fn add(x:int):int{return x+1;}
native main{println(add(9223372036854775807));println(-7/3);
println(-7%3);println(false && (1/0));return 0;}
```

```forge
// trap.fg; repeat with x%-1, x/0, and x%0
fn bad(x:int):int{return x/-1;}
native main{println(bad(-9223372036854775807-1));return 0;}
```

```forge
// floats.fg: parameters keep zero division out of literal folding
fn fdiv(a:float,b:float):float{return a/b;}
native main{println(0.1);println(18446744073709551616.0);
println(fdiv(0.0,-1.0));println(fdiv(1.0,0.0));println(fdiv(-1.0,0.0));
let n:float=fdiv(0.0,0.0);println(n);println(n==n);return 0;}
```

```forge
// nested.fg
fn first():float{println("first");return 1.5;}
fn second():float{println("second");return 2.5;}
native main{println(first(),second());return 0;}
```

```forge
// bool.fg
native main{let b:bool=2;println(b);let wide:int=4294967296;
let c:bool=wide;println(c);return 0;}
```

```forge
// mixed.fg
fn compare(i:int,f:float):bool{return i>f;}
native main{println(compare(9007199254740993,9007199254740992.0));return 0;}
```

```forge
// context.fg: check-only acceptance; not a promise of runtime substitution
const BASE=1;
const VALUE=BASE;
fn integer(BASE:int):int{return VALUE;}
fn floating(BASE:float):float{return VALUE;}
native main{return 0;}
```

```forge
// reject-float-int.fg
native main{let value:int=1.5;return 0;}
```

```forge
// reject-float-mod.fg
native main{println(1.5%1);return 0;}
```

| Case | Accepted/rejected surface | Value or failure observable | Backend scope |
| --- | --- | --- | --- |
| conversion | Accepted | `2.5\n2.5\n3.5\n`; status 0 | Stage0 native/JS |
| wrapping | Accepted | `-9223372036854775808\n-2\n-1\n0\n`; status 0, no divide trap in skipped operand | Stage0 native/JS |
| trap and three variations | Accepted compilation, runtime failure | Nonzero status; stderr contains `Forge integer division` | Stage0 native/JS |
| floats | Accepted on supported IEEE host; current `fdiv` example verified 2026-10-19 | Native O0/O3 and JS exit 0; backend-specific float text below, final comparison `0` | Stage0 native/JS; excluded selfhost |
| nested | Accepted | Distinct byte sequences recorded above | Stage0 native/JS; float example excluded selfhost |
| bool | Accepted | First print retains 2; wide conversion is a portability probe, not an expected shared value. **Observed 2026-10-17**: native `2` then `0`; Node `2` then `4294967296` | Stage0 native/JS; same ABI issue relevant to selfhost |
| mixed | Accepted | **Confirmed 2026-10-17**: native prints `0` (false), Node prints `1` (true). See runtime-evidence amendment below | Stage0 native/JS |
| context | Accepted by checker | Check status 0, caller-local BASE determines each initializer type | Stage0 --check; nonliteral native emission unsupported |
| reject-float-int | Rejected | `type mismatch for` diagnostic | Stage0 --check |
| reject-float-mod | Rejected | `non-integer operand for` diagnostic | Stage0 --check |

### Runtime-evidence amendment (2026-10-17)

The corpus version available on 2026-10-17 was executed on that working tree;
its historical byte-exact results are
recorded in [scalar-rfc.md](../../.omo/evidence/contracts-20261017/scalar-rfc.md)
(compiler `642aea4c…` + priorities-1/2 fixes, native O0/O3 UBSan and Node). This
amendment records observations only; D1–D4 were all Proposed at that time.
The policy approvals recorded below occurred on 2026-10-19.

- Confirmed observations: conversion `2.5\n2.5\n3.5\n`; wrapping
  `-9223372036854775808\n-2\n-1\n0\n`; all four integer traps exit 1 with
  `Forge integer division`; nested interleaving exactly as stated; `mixed`
  native `0` / Node `1`; reject cases as listed; `context` accepted by `--check`.
- Historical native float text (the separately renamed `fdiv` variant): `0.10000000000000001`,
  `1.8446744073709552e+19`, `-0`, `inf`, `-inf`, `-nan`, and `n==n` → `0`. Node
  text: `0.1`, `18446744073709552000`, `0`, `Infinity`, `-Infinity`, `NaN`, `0`.
  Native NaN spelling is `-nan` on this host, confirming the spelling/sign caveat.
- **Historical corpus defect:** the original `floats` example used `div`, which
  collided with the C standard `div()` (`conflicting types for 'div'`). The
  historical native float rows came from a separately renamed `fdiv` variant,
  not the original runnable block. The earlier ledger identifies those failing
  and working translations in `raw/scalar/`.

### Runnable float corpus repair (2026-10-19)

The authoritative `floats.fg` block above now uses `fdiv` for its declaration and
all calls. Fresh execution reproduced the original `div` conflict at native
O0/O3, then compiled and ran the corrected block at native O0/O3 with UBSan and
under Node. All corrected runs exited 0 with empty stderr. Exact current bytes:

- Native O0 and O3: `0.10000000000000001\n1.8446744073709552e+19\n-0\ninf\n-inf\n-nan\n0\n`.
- Node: `0.1\n18446744073709552000\n0\nInfinity\n-Infinity\nNaN\n0\n`.

The current host observations match the historical float spellings, including
`n==n` printing `0`; they do not establish a portable NaN spelling or shared
formatter policy. Compiler identity, complete commands, generated sources, and
captured bytes are in [scalar-corpus.md](../../.omo/evidence/resume-20261019/scalar-corpus.md).
That repair changed only the executable example and its evidence description;
it did not grant policy approval. The later explicit user approvals are recorded
below and do not alter those historical runtime observations.

For FFI, use `extern fn read_value():float; native main{println(read_value());return 0;}`
and a bridge `double read_value(void){return 1.5;}`; add `bridge.c` to the C command.
Expected native output is `1.5\n`. An arbitrary C bridge is not executable JS
FFI. The existing scalar suite has this exact boundary scenario and module and
coroutine cases; these are distinct scopes, not permission to generalize to all
binary library APIs.

## Selfhost subset and compatibility

The original baseline described below has been superseded for arithmetic:
on 2026-10-22, task 5 introduced precedence-aware wrapping helpers and guarded
division/remainder for the supported int/bool subset. Compound updates, loop
steps and short circuit are covered by `tests/selfhost_arithmetic_test.py` at
O0/O3 with UBSan; fixed-point verification remains a separate convergence check.

At the RFC baseline, `bootstrap/compiler.fg` was independent and emitted raw C arithmetic;
[bootstrap/AGENTS.md](../../bootstrap/AGENTS.md) limits scalar types to int, bool,
string and void. Floats, arbitrary modules and coroutine lowering are outside
its supported subset. At that baseline overflowing selfhost arithmetic did not
have stage0's wrapping contract. A non-overflow baseline example was
`native main{println(7/3);return 0;}`; it is only a subset case.
Selfhost constants use macros and its checker rejects local constant-name
shadowing, unlike the stage0 contextual-check example. A fixed point establishes
compiler output convergence, not scalar semantic parity. Task 5's differential
cases establish the narrower arithmetic claims recorded above.

Adoption should leave wrapping, copy/value semantics, accepted float boundaries,
and contextual checking intact. D1/D2/D3/D4 are separate decisions so a formatter
choice cannot silently authorize bool normalization or global evaluation order.
Float conversion helpers should have no allocation and evaluate operands once;
shared formatting and streaming output can add code/runtime work. No performance
improvement is claimed; measure formatter size and representative output cost if
adopted. No runtime/stdlib pin or ABI update is included here.

## Maintainer decision record and completion gate

On 2026-10-19 the human user stated: “D4는 더 나은 방안이 없는지 확인해주고 나머지는 허가할게”
(“Check whether there is a better approach for D4; I approve the rest.”).
This initially approved D1–D3 and the existing rules while requesting D4 review.
After the exact-comparison recommendation, the user added “D4도 진행”
(“Proceed with D4 too”), explicitly authorizing revised option C on the same date.

| Decision | Policy choice | Decision recorded | Required evidence before implementation completion |
| --- | --- | --- | --- |
| D1 float text | Shared shortest-round-trip with explicit spellings above | Accepted 2026-10-19; explicit human user authorization | Paired finite/boundary/special-value bytes and formatter tie cases |
| D2 println interleaving | Preserve native streaming on both backends | Accepted 2026-10-19; explicit human user authorization | Nested calls, once-only effects, trap after printed prefix |
| D3 bool validity | Preserve legacy acceptance/C int ABI; limit portability claims | Accepted 2026-10-19; explicit human user authorization | Canonical/noncanonical/wide values and real FFI boundary coverage |
| D4 mixed comparisons | Option C: exact int64/stored-binary64 comparison; reviewed alternatives above | Accepted 2026-10-19; explicit follow-up human user authorization | All six operators and both operand orders; ±2^53 boundaries, int64 endpoints, fractions, signed zero, NaN and infinities |
| Existing int/conversion/context rules | Preserve documented behavior and phase limits | Accepted 2026-10-19; explicit human user authorization | Scalar/arithmetic/JS suites plus check-only context probes |

The accepted rows record the explicit human authorizations above. All scalar
policy decisions are Accepted, including D4 option C. D4's scoped stage0
implementation evidence is recorded above; D1 and D2 implementation completion
is not claimed. D3 retains the documented compatibility baseline. This record
does not approve the separate ownership RFC.
