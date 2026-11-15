# Scoped command resources

Stage0 native output supports automatic cleanup of an explicitly owned command:

```forge
import os;
native main {
    own let command: int = os_command("true");
    if (command == 0) { return 126; }
    return os_command_run(command);
}
```

The factory and release pair are resolved from SDK metadata. `own let` with an
integer type accepts the verified `os_command` factory; an arbitrary integer,
source function or unknown external factory does not establish ownership.
The integer ABI and existing caller-managed command code are unchanged.

The compiler releases live commands in reverse declaration order on block exit,
function fallthrough, early return, `break` and `continue`. A return expression
finishes before cleanup. Nested scopes and shadowed names have separate cleanup
slots. A command declared in a `for` initializer survives body `continue` and is
released when the loop scope ends, including when the body breaks or returns.

Assigning another `os_command(...)` evaluates the factory once, releases the old
command, then stores the new handle. A failed replacement stores zero after
releasing the old command. Cleanup skips zero handles. This does not change the
order of the binding's eventual scope cleanup. `os_command_arg` and
`os_command_run` borrow the owner for the duration of the call; their status
results are ordinary integers. Check factory and argument-operation failures as
appropriate for the program.

Owned handles may be tested against zero or used as conditions. Copying them to
another binding, returning them, sending/spawning them, converting them into
ordinary scalar data, passing them to arbitrary functions or manually freeing
them is rejected. To use explicit `os_command_free`, retain a caller-managed
`let` binding. A resolved source helper may borrow an `int` command parameter when its
only statement calls or returns `os_command_run(parameter)` or
`os_command_arg(parameter, text)`. The parameter may appear only in the SDK
operation's handle slot. For example:

```forge
import os;
fn run(command: int): int { return os_command_run(command); }
native main {
    own let command: int = os_command("true");
    if (command == 0) { return 1; }
    return run(command);
}
```

The helper does not release or retain the command; the caller still releases it
after the return expression. Extern helpers, helpers with additional statements
or another use of the parameter, and chains of forwarding helpers are excluded.
General ownership transfer and interprocedural borrowing remain unsupported.
Resource declarations/replacements in a `for` step are excluded.

Automatic command owners are supported in source/library functions and
`native main`. Process bodies, coroutines, JavaScript, the selfhost compiler,
forced termination and abort/unwind cleanup are excluded. The experimental
`--safe` checker still excludes legacy command APIs; this feature runs through
the ordinary typed checking and native generation pipeline. It does not make
every legacy integer handle a checked resource.

Arena strings, views and builders retain their existing region rules. In
particular, `own let text: string` does not insert a heap free for a literal or
arena allocation. Other native resources need their own reviewed factory/release
contracts before automatic cleanup can be enabled.

`tests/resource_cleanup_test.py` checks native O0/O3 output with the real SDK
factory, run and release operations wrapped only to count live owners and report
ordering. It covers nested shadowing, returns, normal fallthrough, replacements,
null factories, match arms, loop exits, caller-managed compatibility and rejected
escapes, single-call source helpers and rejected helper aliases. The Linux test's exit hook rejects any outstanding command or duplicate
release. No runtime or standard-library ABI change is required.
