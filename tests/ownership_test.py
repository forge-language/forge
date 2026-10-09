"""Compile emitted C with sanitizers and exercise real owned mailbox payloads."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
for name in ('forge', 'root', 'lib-dir', 'cc'):
    parser.add_argument('--' + name, required=True)
config, remaining = parser.parse_known_args()
sys.argv = [sys.argv[0], *remaining]

BRIDGE = r'''
#include "forge_runtime.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
int64_t receiver(void) {
    return (int64_t)(intptr_t)fr_process_create("ownership-test");
}
int64_t verify_receiver(int64_t handle, const char *expected) {
    fr_process_t *process = (fr_process_t *)(intptr_t)handle;
    fr_msg_t message = {0};
    int result = fr_try_recv(process, &message) && message.owns_payload &&
        message.payload && message.payload_size == strlen(expected) + 1 &&
        strcmp(message.payload, expected) == 0;
    if (message.owns_payload) free(message.payload);
    fr_process_destroy(process);
    return result ? 0 : 1;
}
'''


class OwnershipExecutionTests(unittest.TestCase):
    def run_source(self, source, expected=''):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            fg, emitted, bridge, binary = [work / name for name in
                                          ('test.fg', 'test.c', 'bridge.c', 'test')]
            fg.write_text(source)
            result = subprocess.run([config.forge, str(fg), '--emit-c', '-o', str(emitted)],
                                    text=True, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            bridge.write_text(BRIDGE)
            # Instrument the generated boundary and bridge; runtime has its own
            # complete sanitizer suite. Intercepted free catches the old crash.
            link_flags = ['-fno-pie', '-no-pie'] if sys.platform.startswith('linux') else []
            result = subprocess.run([
                config.cc, '-O1', '-g', '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', *link_flags,
                '-Werror=incompatible-pointer-types', '-I', str(Path(config.root) / 'include'),
                str(emitted), str(bridge), '-L', config.lib_dir,
                '-lforge_std', '-lforge_runtime', '-lpthread', '-lm', '-o', str(binary)
            ], text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
                       UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
            result = subprocess.run([str(binary)], env=env, text=True,
                                    capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, expected)

    def test_rejected_literal_and_arena_payloads(self):
        self.run_source('''import strings;
native main {
    let i: int = 0;
    while (i < 1000) {
        own let literal: string = "literal";
        send 0, Drop, move literal;
        own let arena: string = str_concat("arena", " payload");
        send 0, Drop, move arena;
        i = i + 1;
    }
    return 0;
}''')

    def test_receiver_owns_independent_payload(self):
        self.run_source('''import strings;
extern fn receiver(): int;
extern fn verify_receiver(handle: int, expected: string): int;
native main {
    let destination: int = receiver();
    own let literal: string = "literal";
    send destination, Text, move literal;
    if (verify_receiver(destination, "literal") != 0) { return 1; }
    let second: int = receiver();
    own let arena: string = str_concat("arena", " payload");
    let alias: string = arena;
    send second, Text, move arena;
    if (verify_receiver(second, "arena payload") != 0) { return 2; }
    println(alias);
    return 0;
}''', 'arena payload\n')

    def test_suspended_coroutine_transfer(self):
        self.run_source('''process main {
    coroutine sender() {
        own let payload: string = "suspended";
        yield;
        send 0, Drop, move payload;
        println("sent");
    }
    spawn sender();
}''', 'sent\n')


if __name__ == '__main__':
    unittest.main()
