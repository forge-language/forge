# Integer arithmetic

Stage0's native and JavaScript backends use signed 64-bit integers. Addition,
subtraction and multiplication wrap modulo 2^64, interpreted as a signed result.
The optimizer applies the same rule to constants. Unary negation and compound
assignments lower through these operations.

```forge
native main {
    let largest: int = 9223372036854775807;
    println(largest + 1); // -9223372036854775808
    return 0;
}
```

Integer division truncates toward zero. Remainder has the dividend's sign.
Division or remainder by zero, and either operation on `INT64_MIN` with `-1`,
fail at runtime. Native execution writes `Forge integer division` to stderr and
exits with a failure status; JavaScript throws an error carrying that message.
These cases also fail when their operands are constants; the optimizer does not
evaluate an invalid operation inside the compiler.

The native backend uses unsigned arithmetic and bit copies for wrapping, so it
does not depend on C signed overflow. C11 type dispatch keeps floating-point
operations floating-point when either operand is floating-point. Each operand
is evaluated once; this does not add a new operand evaluation order guarantee.
Logical `&&` and `||` retain short-circuit evaluation. Floating-point remainder
is not supported.

Mixed integer/float comparisons compare the exact integer against the value
represented by the binary64 operand, without rounding the integer first. All
six operators and either operand order follow this rule. For example,
`9007199254740993 == 9007199254740992.0` is false and the corresponding `>` is
true. Arithmetic still promotes mixed operands to binary64; comparison cannot
recover precision already lost by arithmetic or by parsing a float literal.
NaN is unordered (`!=` is true and all other comparisons are false), signed
zeroes compare equal, and infinities retain their usual ordering. Native
helpers guard the int64 range before any float-to-integer conversion and
evaluate each operand once. Opaque C types outside the Forge integer/bool and
binary64 domain retain their C comparison behavior.

The integer wrapping and guarded division contract also applies to the supported
int/bool subset of the independent self-hosted `forge-fg` frontend. It lowers
arithmetic, unary negation and compound updates through its own helpers;
`tests/selfhost_arithmetic_test.py` compares stage0 and stage2 at O0/O3 with UBSan,
including precedence, short circuit, loop steps and division failures.
Selfhost does not support float values or mixed integer/float comparison.
Native FFI and standard-library internal arithmetic have their own
contracts. Lean's mathematical integer optimizer and checked-int64 model are
separate models; neither is a proof of refinement of this wrapping implementation.

`tests/arithmetic_test.py` compares constant and parameterized native/JS results
against a Python oracle, checks native execution with UBSan at O0 and O3, and
covers invalid division, floating promotion, boolean operations, suspended
coroutines and exported library functions.

## Floating-point boundaries and output

Stage0 accepts integer and boolean values at declared `float` boundaries. Both
backends convert these values for local initialization/assignment, function
parameters and function results. JavaScript also converts float extern arguments
and results to Number, so an integer literal passed to a float parameter does not
silently select BigInt division. Conversion evaluates its operand once.

Native `println` dispatches on the actual generated C type, including types from
external library headers. Binary64 values use a compiler-owned shortest-round-trip
formatter based on vendored Ryu, with an ASCII decimal point and no locale
dependence. Both backends use fixed notation for magnitudes from 1e-6 inclusive
to 1e21 exclusive, lowercase `e` otherwise, an explicit exponent sign and no
leading exponent zeroes. Integral values omit `.0`; signed zero, infinities and
NaN print as `-0`, `Infinity`, `-Infinity` and `NaN`. JavaScript uses Number's
shortest formatting with an explicit negative-zero case. No narrowing to int64
occurs. `tests/float_format_test.py` compares deterministic binary64 bit patterns
against Node, including exponent boundaries and subnormals.

`println` evaluates and writes each argument once in source order, then writes
a newline. Nested output interleaves at the argument that produces it. JavaScript
uses stdout under Node, or a browser `ForgeWrite` callback when provided; its
console fallback buffers complete lines.

`tests/scalar_conformance_test.py` compares representative scalar programs across
native O0/O3 with UBSan and JavaScript, and checks native float output across
module calls, extern results and coroutine suspension.
