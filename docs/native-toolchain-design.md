# GCC-independent Forge native toolchain

## Goal

Build Forge programs into native executables without GCC or Clang. The delivered
compiler must be implemented in Forge, and it must generate and link native code
for the supported platform and CPU targets. A prebuilt copy of the current C
compiler does not meet this goal: it emits C and delegates both machine-code
generation and linking to a C toolchain.

The requested scope includes Linux, macOS and Windows, plus 32-bit ARM.
`compiler/target.c` parses architecture, OS, ABI, object format, libc and pointer
width from target triples. This is target identification, not backend support.
The exact ARM32 OS/ABI tuple remains to be selected; “ARM32” alone does not
define object relocations, calling convention, system-call interface or C ABI.

## Current pipeline and blockers

The production compiler parses Forge into `Program` AST nodes, emits C from
`compiler/codegen.c`, invokes `CC` to produce an object, then invokes the same
C driver to link that object with `libforge_runtime`, `libforge_std`, platform
libraries and the system math/thread libraries. `compiler/driver.c` owns this
pipeline and `compiler/main.c` exposes the `--cc` option.

The bootstrap compiler in `bootstrap/compiler.fg` is a separate, deliberately
small frontend. It handles a restricted subset and emits C; `build_native`
invokes `cc` to produce the executable. Its stage2 fixed-point check proves
stable C output for that subset, not native code generation or full-language
self-hosting.

The runtime and much of the standard library are C. A source inventory in this
checkout counts about 5.8k lines across 21 compiler C/header files, 1.3k lines
in the single Forge bootstrap source, and another 5.8k lines across 28 runtime
and standard-library C/header files. Native adapters and system APIs also
depend on operating-system libraries. Removing GCC/Clang from the Forge
source-to-executable path therefore requires all of the following:

- A Forge-authored frontend that reaches the required language and module parity.
- A typed, target-independent intermediate representation (IR).
- Native instruction selection and object emission for each supported CPU ABI.
- Object symbol/relocation handling and executable linking for each OS format.
- A runtime and standard-library implementation that the new compiler can emit
  without routing through a C compiler.
- A staged bootstrap and SDK packaging process that does not require users to
  install a C compiler.

Shipping `as`, `ld`, `link.exe`, or a system linker could remove GCC/Clang while
still retaining external native tools. That is a valid intermediate milestone,
but it does not satisfy the stronger goal of Forge-owned object linking. Each
milestone must say which tools remain required.

## Implementation sequence

1. **Specify the platform matrix and ABI contracts.** List each OS, CPU, object
   format, calling convention, stack rules, relocation set, executable format,
   system libraries and minimum OS version. Include Windows and 32-bit ARM as
   explicit entries, with independent host and output target fields.
2. **Introduce a typed IR.** Lower the existing AST into explicit values,
   control-flow blocks, calls, memory operations, imports and source locations.
   Keep target-independent validation and optimization separate from C output.
   Initially retain the C backend as a reference path.
3. **Build native backends incrementally.** Implement instruction selection and
   object writing for one target tuple at a time. Define a shared object model
   for sections, symbols, relocations and debug/source metadata. Unsupported
   operations must fail with a source-located diagnostic rather than emit
   partial or silently incorrect output.
4. **Add a linker and runtime path.** Resolve Forge object files, archives,
   imports and relocations, then create executables for each supported format.
   Port the runtime and required standard-library functions from C to Forge or
   backend-supported low-level modules. Keep OS-specific primitives behind
   narrow, documented interfaces.
5. **Self-host the frontend.** Grow the Forge implementation to parse, validate
   and lower the language features that the IR/backend supports. Keep a C
   stage0 only as a temporary bootstrap. Compile the Forge compiler with stage0,
   then compile it with itself and compare behavior and compiler outputs across
   repeated stages.
6. **Package and remove host compiler requirements.** Build distributable
   compiler/SDK artifacts per host and target tuple. CI must build and execute
   native fixtures without GCC/Clang installed, and cross-compile fixtures for
   each advertised target. Remove `CC` from the default Forge native path only
   after the Forge backend and linker cover that path; retain C emission as an
   optional interoperability feature if desired.

## Completion criteria

- The compiler frontend, IR lowering, target backends and linker are authored
  in Forge; any unavoidable low-level bootstrap seed is documented and no longer
  needed to rebuild later compiler stages.
- A clean machine without GCC or Clang can install the SDK and build supported
  Forge programs for every advertised target tuple.
- The shipped runtime and standard library do not require a C compiler during
  user builds.
- Executable format, ABI, imports, relocations, errors and runtime behavior are
  covered for every target tuple; unsupported features fail explicitly.
- Rebuilding the compiler through the documented bootstrap sequence succeeds
  from a clean checkout and does not use the old C backend as the final native
  code generator or linker.

## First implementation boundary

The `--emit-ir` path lowers ordinary function bodies, local slots, calls with
visible signatures, structured control flow, `match`, short-circuit boolean
operations, and `native main` into target-neutral basic blocks. Literal globals
and struct/enum declarations are represented; process declarations and
supervisors remain unsupported. Calls without visible signatures retain an
unknown result type.

`compiler/native.c` consumes a single-entry `native main` and writes a Linux
x86_64 ELF executable directly. It supports integer constants, arithmetic,
comparisons, integer locals, and basic branches and loops, with no C compiler,
assembler, linker or Forge runtime in that output path. Driver regressions run
these programs with `--cc` set to a missing executable. Function calls,
general phi lowering across conditional edges, object files, runtime linking,
self-hosting and the remaining requested targets are still outstanding.
