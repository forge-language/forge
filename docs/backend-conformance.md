# Backend conformance

This matrix describes the current compiler source. It is not a claim about older
installed SDKs or a release announcement. The native/JS backends consume the same
post-optimization typed occurrence graph; the Forge compiler is independent.

| Behavior | Stage0 native | Stage0 JavaScript | Selfhost `forge-fg` | Evidence |
| --- | --- | --- | --- | --- |
| int64 wrapping add/subtract/multiply/negate | Supported | BigInt wrapping | Supported subset | `arithmetic_test.py`, `selfhost_arithmetic_test.py` |
| Division/remainder traps, truncation toward zero | Supported | Supported | Supported subset | Same arithmetic suites |
| int/bool to float at declared boundaries | Supported | Number conversion | Floats unsupported | `scalar_conformance_test.py` |
| Exact mixed int/float comparisons | Supported | Supported | Floats unsupported | Scalar suite, including range edges, NaN and single evaluation |
| Shortest binary64 output and special spellings | Ryu adapter | Number plus signed-zero case | Floats unsupported | `float_format_test.py`, scalar suite |
| Once-only, ordered println argument evaluation and output | Supported | Node stdout or browser `ForgeWrite`; console fallback buffers lines | Supported subset | Scalar suite, `selfhost_test.py` |
| NUL-terminated string operations, nullable string output | Supported | Supported for valid UTF-8 | Native string SDK | `string_conformance_test.py` |
| UTF-8 byte lengths and indexing | Supported | Supported; slices must preserve UTF-8 boundaries | Native string SDK | String suite, `js_test.py` |
| String views, builder snapshots and finish-take | Native SDK | Supported valid UTF-8 subset | Selected string builtins | `ownership_test.py`, `js_test.py`, `selfhost_test.py` |
| Names, scalar SDK signatures and conversion metadata | Checked | Shared checks | Independent subset checker | `semantic_contract_test.py`, `typed_ast_test.c`, `selfhost_test.py` |
| Send-move binding consumption and branch/loop joins | Checked, copying payload | Shared precheck; processes cannot be emitted | Unsupported | `compiler_test.py`, `diagnostics_test.py`, `ownership_test.py` |
| Scoped owned command cleanup | Supported in synchronous functions and native main | Unsupported | Unsupported | `resource_cleanup_test.py` |
| Source modules | Supported | Supported | Selected SDK imports only | `module_merge_test.py`, `js_test.py`, `selfhost_test.py` |
| Processes and suspended coroutines | Supported | Unsupported | Sequential process main only | Compiler and selfhost suites |
| General move, full borrowing and field-level ownership | Unsupported | Unsupported | Unsupported | Rejection cases in compiler suite |
| Arbitrary external ABI/lifetime guarantees | Not established | Requires explicit native bridge | Not established | Opaque resource manifest entries |

Ordinary `--check` includes semantic checks, optimization, typed construction and
graph verification. A graph verifier proves consistency of this representation,
not lifetime correctness of arbitrary native resources. The current SDK manifest
describes 41 mapped calls and explicitly marks the remaining 127 as opaque.
It does not infer allocator, callback or concurrency guarantees from a scalar ABI.
Argument-position masks distinguish borrowing, mutation, ownership consumption
and release. The command factory records allocation, nullability and its paired
deallocator; its release operation consumes the handle. These descriptors do not
make legacy aliases safe or enforce general unused-result diagnostics. Unknown
SDK and external binary obligations stay explicitly opaque. Catalog coverage is
checked against the installed SDK headers by `semantic_contract_test.py`;
`resource_contracts_test.c` also checks effect masks against each signature's
argument count. The cleanup runner injects real SDK allocation failure and
checks that replacement releases the old owner while zero owners are skipped.
The additive `--safe` checker is experimental and incomplete; its further
hardening and security audit work is deferred from the current implementation
scope. Existing source is retained, and no full-language safety claim follows.

`own let` authorizes binding consumption by send-move. Native lowering creates a
mailbox-owned copy; aliases to arena strings retain their original storage.
Reassigning the binding initializes it again. These rules do not introduce general
move expressions, exclusive ownership or implicit disposal of opaque resources.
Explicit `own` command bindings use the reviewed SDK command factory/release pair;
the legacy aliasing boundary remains opaque to the experimental safety checker.
See [scoped command lifetimes](resource-lifetimes.md) for cleanup ordering and exclusions.
See [ownership](examples/ownership.md) and the accepted [RFC](rfcs/ownership-lifetimes.md).

Selfhost tests compare supported program behavior at O0/O3 separately from the
stage3/stage4 fixed point. Convergence does not establish equivalence outside its
subset. Native FFI and SDK implementation arithmetic retain their own contracts.

For local components, configure/build/test with the root project guide's source
overrides. Run `ctest --test-dir build --output-on-failure` and separately
`cmake --build build --target forge-selfhost-verify -j2`. Test the installed SDK
with all three consumers in `examples/external-project/`. Security audit and fuzz
results are separate evidence and are not prerequisites claimed by this matrix.
