# Scalar semantics and backend conformance

Status: **Draft / Proposed** (2026-10-17). No maintainer acceptance is recorded.

Decision owner: Forge human maintainers. Draft author: implementation agent.
Scope: plan task 1; stage0 native C and JavaScript, with explicit selfhost exclusions.
This draft does not authorize production changes, ABI changes, or later plan tasks.
Under [RFC_PROCESS.md](../../RFC_PROCESS.md), a human maintainer records the
Accepted / Rejected / Deferred decision. A request to proceed with investigation
is not that decision. The task 1 acceptance gate remains open.

## Problem and evidence boundary

The semantic checker accepts scalar combinations that independent backend
representations do not always implement identically. Existing conversion fixes
make `half(5)` produce 2.5 at a float boundary, but do not settle float text,
noncanonical bool values, mixed comparisons, or output interleaving.

Source observations below refer to the current stage0 implementation, including
the existing float-conversion, native `%.17g`, and SDK-signature fixes copied into
this worktree. Source inspection is not runtime proof. The evidence ledger is
[scalar-rfc.md](../../.omo/evidence/contracts-20261017/scalar-rfc.md);
runtime observations are separately identified there. No benchmark, fixed point,
or passing subset is evidence of full-language equivalence.

## Current behavior and proposed policy

“Accepted” in the program tables means the compiler accepts a program, not that
a maintainer has accepted this RFC. Every recommendation below remains Proposed.

| Rule | Current stage0 behavior and source | Proposed policy | Backend scope |
| --- | --- | --- | --- |
| Scalar representation | Native `c_type` in `compiler/codegen.c`: int → `int64_t`, float → `double`, bool → C `int`. JS `expression`: integer → BigInt, float → Number, boolean literal → Boolean. | Preserve int64 and binary64 value models. Preserve C `int` bool ABI; separately decide value normalization below. | Native/JS; selfhost has no float. |
| int/bool → float | `semantic.c:compatible` accepts both. C parameter/local/result types perform conversion; JS `converted_expression` and extern wrappers apply Number. | Preserve conversion at initialization, assignment, argument, return, and declared extern boundaries, evaluating the source once. Loss of precision above 2^53 is allowed binary64 rounding. | Native/JS supported calls; opaque headers retain native C boundary. |
| int ↔ bool | `compatible` accepts either direction. Native C `int` can retain 2 and narrow wide int64 values; JS does not generally coerce declared bool bindings. | Preserve acceptance and ABI for compatibility. Do not call all bool values canonical 0/1. D3 below resolves the portable value domain before checked ABI validation. | Native/JS differ outside canonical values. |
| float → int/bool | Known float values fail `compatible` for int/bool targets. | Preserve rejection; any explicit conversion API needs separate range/NaN policy. | Native/JS semantic check. |
| Numeric arithmetic | `semantic.c:expression` accepts int/bool/float and gives arithmetic float type when either operand is float. Native helpers dispatch on actual C types; JS `arithmetic` promotes Number when either operand is Number. | Preserve promotion and wrapping; record conversions explicitly in later typed lowering. | Native/JS; unknown ABI types need actual declarations. |
| Mixed comparisons | Native C performs its numeric conversions. JS `eq` Number-promotes mixed Number/BigInt; `lt/le/gt/ge` directly compare operands. | D4 recommends binary64 conversion for all mixed float comparisons, consistently with arithmetic. This is a proposed JS behavior change near 2^53. | Native/JS; selfhost excludes floats. |
| Integer overflow | `optimize.c:fold_int` and native unsigned helpers wrap add/subtract/multiply to signed int64. JS `wrap` uses `BigInt.asIntN(64, ...)`. | Preserve wrapping, including intermediate results and unary negation/compound assignment lowering. Do not introduce overflow traps or reassociation that changes results. | Stage0 native/JS only. |
| Integer division/remainder | Truncate toward zero; remainder has dividend sign. Zero divisor or INT64_MIN with -1 fails for both `/` and `%`. Optimizer leaves invalid constant operations for runtime. | Preserve nonzero exit and stderr containing `Forge integer division`; JS stack trace and exact status number are not shared text contracts. | Stage0 native/JS only; native FFI internals excluded. |
| Short circuit | Native C operators and JS Boolean wrappers preserve `&&`/`||` short circuit. | Preserve skipped effects/traps and once-only evaluation of evaluated operands. | Native/JS and supported selfhost cases. |
| Float remainder | Known float operand to `%` rejected by `semantic.c:expression`. | Preserve rejection. | Native/JS. |
| Constants | `semantic.c:expression(EXPR_IDENT)` checks locals first and recursively rechecks a constant initializer in each caller context. No permanent initializer-type memoization. Emission has narrower support: native `emit_consts` requires folded literals; JS emits top-level const bindings. | Preserve existing contextual **checking** behavior, including check-only acceptance. Do not invent runtime substitution for nonliteral constants. Resolve each use separately in typed lowering; changing lookup policy needs another RFC. | Stage0 checker; output/backend limits remain explicit. |
| FFI | Explicit extern and mapped SDK signatures participate in checking; unknown binary results remain UNKNOWN. Native output uses actual C declarations; `FORGE_PRINT_AUTO` selects by actual C type. | Keep native ABI and declared float conversions. UNKNOWN means unverified, not void, canonical bool, or safe resource. No name-based ABI guessing. | Native C ABI; only implemented JS mappings/extern facilities. |

## Floats, special values, and text

Proposed numeric model: binary64 arithmetic, preserving observable signed zero;
NaN compares unequal to itself and ordered comparisons with NaN are false;
float division by zero yields infinity/NaN under the supported IEEE host
configuration rather than invoking the integer trap. Do not promise NaN payload
bits or NaN sign preservation. This proposal requires explicit host assumptions
and acceptance tests; arbitrary C implementations and fast-math modes are not
covered. `optimize.c:fold_binary` currently declines float division by zero and
nonfinite folded results; `simplify_binary` restricts identity rewrites to known
integers to avoid erasing float promotion or signed-zero behavior.

Native `emit_print_auto_macro` emits `printf("%.17g", value)`. JS
`codegen_emit_js` emits `String(x)` for Numbers. Therefore identical numeric values
do not imply identical bytes. The following spellings are host-format examples,
not a newly adopted portable contract:

| Value/example | Native formatter | JS Number formatter | Proposed shared policy |
| --- | --- | --- | --- |
| 0.1 | Typically `0.10000000000000001` | `0.1` | D1 decision below. |
| 18446744073709551616.0 | Typically `1.8446744073709552e+19` | `18446744073709552000` | D1 decision below; same rounded binary64 value. |
| Negative zero, produced by dividing 0.0 by -1.0 | Typically `-0` | `0` | Preserve value sign; recommend `-0` text under D1. |
| Positive/negative infinity | Typically `inf` / `-inf` | `Infinity` / `-Infinity` | Recommend `Infinity` / `-Infinity`. |
| NaN from 0.0/0.0 | C library spelling/sign varies | `NaN` | Recommend `NaN`, with no payload/sign text. |

D1 options: (A) retain backend-specific text and document that parity means numeric
value, (B) choose a common shortest-round-trip formatter, or (C) require identical
17-significant-digit formatting in both backends. **Recommendation: B**, with
ASCII decimal point, no locale variation, shortest decimal round trip, lowercase
`e`, explicit exponent sign, no exponent leading zeroes, fixed notation for
magnitudes from 1e-6 inclusive to 1e21 exclusive, and exponential notation outside
that interval. Print integral finite values without `.0`; print signed zero and
special values as above. The implementation must select and validate a formatter
with deterministic tie handling before claiming byte parity. A is the least
implementation work but leaves divergent output; C retains familiar native text
but adds visible decimal noise and requires a JS formatter. No formatter or
locale behavior is changed by this draft.

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
or (C) retain explicitly different output contracts. **Recommendation: A**,
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
**Recommendation: A for this compatibility baseline.** Keep canonical literals,
comparisons, and logical results at 0/1; record noncanonical inputs as legacy
behavior, not invalid Rust-style bool bit patterns. In particular a C `int`
value 2 is not inherently invalid C, and conversion from an out-of-range int64
must not be described as portable truncation. B provides a cleaner future checked
boundary but changes the observable result of `let b:bool=2; println(b);`.
The maintainer must decide whether checked facades later normalize, reject, or
explicitly preserve noncanonical FFI values; an UNKNOWN signature cannot answer
that question. Scalar checking does not establish callback ABI/lifetime safety.

D4 recommendation: use binary64 conversion on both operands of every mixed
float comparison. Example: `9007199254740993 > 9007199254740992.0` is false after
conversion, whereas JS direct BigInt/Number relational comparison is true.
The alternative is exact mixed mathematical comparison on both backends, which
would require new native comparison helpers and differs from the existing
promotion model. This is a concrete compatibility choice awaiting a decision.

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
fn div(a:float,b:float):float{return a/b;}
native main{println(0.1);println(18446744073709551616.0);
println(div(0.0,-1.0));println(div(1.0,0.0));println(div(-1.0,0.0));
let n:float=div(0.0,0.0);println(n);println(n==n);return 0;}
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
| floats | **Corpus as written fails native compile** (see amendment); renamed variant Accepted on supported IEEE host | Same numeric classes; formatter differences described above, final comparison `0` | Stage0 native/JS; excluded selfhost |
| nested | Accepted | Distinct byte sequences recorded above | Stage0 native/JS; float example excluded selfhost |
| bool | Accepted | First print retains 2; wide conversion is a portability probe, not an expected shared value. **Observed 2026-10-17**: native `2` then `0`; Node `2` then `4294967296` | Stage0 native/JS; same ABI issue relevant to selfhost |
| mixed | Accepted | **Confirmed 2026-10-17**: native prints `0` (false), Node prints `1` (true). See runtime-evidence amendment below | Stage0 native/JS |
| context | Accepted by checker | Check status 0, caller-local BASE determines each initializer type | Stage0 --check; nonliteral native emission unsupported |
| reject-float-int | Rejected | `type mismatch for` diagnostic | Stage0 --check |
| reject-float-mod | Rejected | `non-integer operand for` diagnostic | Stage0 --check |

### Runtime-evidence amendment (2026-10-17)

The corpus above was executed on the current working tree; byte-exact results are
recorded in [scalar-rfc.md](../../.omo/evidence/contracts-20261017/scalar-rfc.md)
(compiler `642aea4c…` + priorities-1/2 fixes, native O0/O3 UBSan and Node). This
amendment records observations only; every D1–D4 decision stays Proposed.

- Confirmed observations: conversion `2.5\n2.5\n3.5\n`; wrapping
  `-9223372036854775808\n-2\n-1\n0\n`; all four integer traps exit 1 with
  `Forge integer division`; nested interleaving exactly as stated; `mixed`
  native `0` / Node `1`; reject cases as listed; `context` accepted by `--check`.
- Native float text now observed directly (renamed variant): `0.10000000000000001`,
  `1.8446744073709552e+19`, `-0`, `inf`, `-inf`, `-nan`, and `n==n` → `0`. Node
  text: `0.1`, `18446744073709552000`, `0`, `Infinity`, `-Infinity`, `NaN`, `0`.
  Native NaN spelling is `-nan` on this host, confirming the spelling/sign caveat.
- **Corpus defect:** the `floats` case as written does **not** compile natively —
  a user function named `div` collides with the C standard `div()`
  (`conflicting types for 'div'`). The native float rows were obtained from a
  renamed variant (`fdiv`). The `floats` row must be corrected before task 10
  cites it; the failing and working translations are both in `raw/scalar/`.

For FFI, use `extern fn read_value():float; native main{println(read_value());return 0;}`
and a bridge `double read_value(void){return 1.5;}`; add `bridge.c` to the C command.
Expected native output is `1.5\n`. An arbitrary C bridge is not executable JS
FFI. The existing scalar suite has this exact boundary scenario and module and
coroutine cases; these are distinct scopes, not permission to generalize to all
binary library APIs.

## Selfhost subset and compatibility

`bootstrap/compiler.fg` is independent. Its `compile_expr` emits raw C arithmetic;
[bootstrap/AGENTS.md](../../bootstrap/AGENTS.md) limits scalar types to int, bool,
string and void. Floats, arbitrary modules and coroutine lowering are outside
its supported subset. Do not run overflowing selfhost arithmetic as if stage0's
wrapping contract applied. A safe non-overflow example is
`native main{println(7/3);return 0;}`; it is only a subset case.
Selfhost constants use macros and its checker rejects local constant-name
shadowing, unlike the stage0 contextual-check example. A fixed point establishes
compiler output convergence, not scalar semantic parity. Plan task 5 must add
explicit supported-subset differential cases before extending this RFC's claims.

Adoption should leave wrapping, copy/value semantics, accepted float boundaries,
and contextual checking intact. D1/D2/D3/D4 are separate decisions so a formatter
choice cannot silently authorize bool normalization or global evaluation order.
Float conversion helpers should have no allocation and evaluate operands once;
shared formatting and streaming output can add code/runtime work. No performance
improvement is claimed; measure formatter size and representative output cost if
adopted. No runtime/stdlib pin or ABI update is included here.

## Maintainer decision record and completion gate

| Decision | Recommendation | Decision recorded | Required evidence before implementation completion |
| --- | --- | --- | --- |
| D1 float text | Shared shortest-round-trip with explicit spellings above | Proposed; maintainer action required | Paired finite/boundary/special-value bytes and formatter tie cases |
| D2 println interleaving | Preserve native streaming on both backends | Proposed; maintainer action required | Nested calls, once-only effects, trap after printed prefix |
| D3 bool validity | Preserve legacy acceptance/C int ABI; limit portability claims | Proposed; maintainer action required | Canonical/noncanonical/wide values and real FFI boundary coverage |
| D4 mixed comparisons | Binary64 promotion for all mixed comparisons | Proposed; maintainer action required | Positive/negative cases around 2^53, NaN and infinities |
| Existing int/conversion/context rules | Preserve documented behavior and phase limits | Proposed ratification; maintainer action required | Scalar/arithmetic/JS suites plus check-only context probes |

No Accepted/Rejected policy decision is filled on the maintainer's behalf.
The draft deliverable can be reviewed now; plan task 1 itself is not complete
until the maintainer decision and its required executable evidence are recorded.
