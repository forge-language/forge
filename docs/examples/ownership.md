# ownership

An initialized string binding inside a coroutine using `own let`. Stage0 checks
use after a `send` move, including conservative branch and loop joins.

## Source

`examples/ownership.fg`

## Features

- `own let` — a string binding eligible for checked mailbox transfer
- `yield` with owned state preserved across suspension

## Code

```forge
process main {
    coroutine demo() {
        own let msg: string = "owned by coroutine";
        yield;
        println(msg);
    }

    spawn demo();
}
```

## Run

```bash
cmake --build build --target ownership
./build/bin/ownership
```

## Expected output

```
owned by coroutine
```

## Related

The supported transfer syntax is `send target, Tag, move msg;`. Native lowering
copies the string into a heap payload, clears the binding and transfers the copy
to the mailbox. A rejected send also releases that copy. String literals and
arena strings are not themselves freed, so borrowed aliases remain readable.
This does not provide zero-copy transfer, exclusive ownership or full lifetime
checking. General `move` expressions are rejected by stage0. The separate
`forge-fg` compiler does not implement this ownership check.
