"""Exercise the public compiler CLI and execute the generated native programs."""
import argparse
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--forge', required=True)
parser.add_argument('--root', required=True)
parser.add_argument('--lib-dir', required=True)
parser.add_argument('--cc', required=True)
config, remaining = parser.parse_known_args()
sys.argv = [sys.argv[0], *remaining]


class CompilerRegressionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.source = Path(self.temp.name) / 'test.fg'

    def invoke(self, *args):
        return subprocess.run([config.forge, *map(str, args)], text=True,
                              capture_output=True, timeout=20)

    def compile_program(self, source):
        self.source.write_text(source)
        binary = Path(self.temp.name) / 'test'
        result = self.invoke(self.source, '-o', binary, '--cc', config.cc,
                             '--forge-root', config.root, '--lib-dir', config.lib_dir)
        self.assertEqual(result.returncode, 0, result.stderr)
        return binary

    def run_program(self, source, expected):
        binary = self.compile_program(source)
        result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, expected)

    def test_imported_extern_and_forward_module_calls(self):
        (Path(self.temp.name) / 'bridge.fg').write_text('extern fn fr_os_getenv(name: string): string;\nfn later(): string { return earlier(); }\nfn earlier(): string { return fr_os_getenv("FORGE_MODULE_TEST"); }\nfn number(): int { return 42; }\n')
        binary = self.compile_program('import bridge;\nnative main { println(bridge.later()); println(bridge.number()); bridge.number(); return 0; }')
        import os
        env = dict(os.environ, FORGE_MODULE_TEST='module ffi')
        result = subprocess.run([str(binary)], text=True, capture_output=True, env=env, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'module ffi\n42\n')

    def test_string_escapes_and_multiline_literals(self):
        self.run_program(r'''native main {
            println("{\"status\":\"ok\"}");
            println("a\nb\tc\\d");
            println("first
second");
            return 0;
        }''', '{"status":"ok"}\na\nb\tc\\d\nfirst\nsecond\n')

    def test_multiply_zero_preserves_value_and_side_effects(self):
        self.run_program('''
fn zero(x: int): int { return x * 0; }
fn effect(): int { println(91); return 7; }
native main {
    println(zero(7));
    println(0 * effect());
    println(effect() * 0);
    return 0;
}
''', '0\n91\n0\n91\n0\n')

    def test_coroutine_multiple_and_nested_yields(self):
        self.run_program('''process main {
    coroutine task() {
        let x: int = 0;
        println(1); yield;
        if (x == 0) { println(2); yield; println(3); }
        while (x < 2) { x = x + 1; yield; println(x + 3); }
        yield; println(6);
    }
    spawn task();
}
''', '1\n2\n3\n4\n5\n6\n')

    def test_coroutine_initializers_run_once(self):
        for pause in ['', 'yield;']:
            with self.subTest(pause=pause):
                self.run_program('''
fn effect(): int { println(91); return 7; }
process main {
    coroutine task() { let x: int = effect(); ''' + pause + ''' println(x); }
    spawn task();
}
''', '91\n7\n')

    def test_await_suspends_until_socket_is_readable(self):
        with socket.socket() as server:
            server.bind(('127.0.0.1', 0))
            server.listen(1)
            server.settimeout(8)
            port = server.getsockname()[1]
            binary = self.compile_program('''
import tcp;
process main {
    coroutine wait(port: int) {
        let client: int = tcp_connect("127.0.0.1", port);
        await client;
        println("ready");
        tcp_close(client);
    }
    spawn wait(''' + str(port) + ''');
}
''')
            child = subprocess.Popen([str(binary)], stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True)
            try:
                connection, _ = server.accept()
                with connection:
                    with self.assertRaises(subprocess.TimeoutExpired):
                        child.communicate(timeout=0.2)
                    connection.sendall(b'wake')
                    stdout, stderr = child.communicate(timeout=8)
                self.assertEqual(child.returncode, 0, stderr)
                self.assertEqual(stdout, 'ready\n')
            finally:
                if child.poll() is None:
                    child.kill()
                child.communicate()

    def test_float_literals_keep_precision_and_floating_type(self):
        self.run_program('''
fn two(): int { return 2; }
fn precise(): float { return 1.2345678901234567; }
native main {
    if (5.0 / two() != 2.5) { return 1; }
    if (precise() < 1.234567890123456) { return 2; }
    if (precise() > 1.234567890123457) { return 3; }
    return 0;
}
''', '')

    def test_wide_integer_literals_and_minimum(self):
        self.run_program('''
fn two(): int { return 2; }
native main {
    println(2147483647 * two());
    println(-9223372036854775807 - 1);
    println(-7 / 3);
    println(-7 % 3);
    return 0;
}
''', '4294967294\n-9223372036854775808\n-2\n-1\n')

    def test_unsafe_constant_arithmetic_does_not_crash_compiler(self):
        for expression in ['9223372036854775807 + 1', '-9223372036854775807 - 2',
                           '9223372036854775807 * 2',
                           '(-9223372036854775807 - 1) / -1',
                           '(-9223372036854775807 - 1) % -1', '1 / 0', '1 % 0']:
            with self.subTest(expression=expression):
                self.source.write_text('native main { let x: int = ' + expression + '; return 0; }')
                result = self.invoke(self.source, '--check')
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_out_of_range_literal_is_rejected(self):
        for literal in ['9223372036854775808', '9' * 100, '1' + '0' * 310 + '.0']:
            with self.subTest(literal=literal):
                self.source.write_text('native main { let x: int = ' + literal + '; }')
                result = self.invoke(self.source, '--check')
                self.assertEqual(result.returncode, 1)
                self.assertIn('numeric literal out of range', result.stderr)

    def test_long_valid_literal_is_not_truncated(self):
        self.run_program('native main { println(' + '0' * 100 + '7); return 0; }', '7\n')

    def test_cli_rejects_unknown_and_incomplete_options(self):
        self.source.write_text('native main { return 0; }')
        for option in ['--unknown', '-o', '--cc', '--header', '--forge-root', '--lib-dir', '-I', '-l']:
            with self.subTest(option=option):
                result = self.invoke(self.source, option)
                self.assertEqual(result.returncode, 1)
                self.assertIn('option', result.stderr)
        result = self.invoke(self.source, '--check', self.source)
        self.assertEqual(result.returncode, 1)
        self.assertIn('multiple input', result.stderr)
        self.assertEqual(self.invoke('--help').returncode, 0)

    def test_module_search_after_many_dependencies(self):
        directories = []
        for number in range(65):
            directory = self.source.parent / f"dependency_{number}"
            directory.mkdir()
            directories.extend(['-I', str(directory)])
        (self.source.parent / 'dependency_64' / 'tail.fg').write_text('fn value(): int { return 42; }')
        self.source.write_text('import tail; native main { println(tail.value()); return 0; }')
        binary = self.source.parent / 'many_dependencies'
        result = self.invoke(self.source, '-o', binary, *directories)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(subprocess.check_output([str(binary)], text=True), '42\n')

    def test_cli_bounds_repeated_include_and_library_options(self):
        self.source.write_text('native main { return 0; }')
        for option, value, limit in [('-I', config.root, 256), ('-l', 'test', 32)]:
            with self.subTest(option=option):
                accepted = self.invoke(self.source, '--check', *([option, value] * limit))
                self.assertEqual(accepted.returncode, 0, accepted.stderr)
                rejected = self.invoke(self.source, '--check', *([option, value] * (limit + 1)))
                self.assertEqual(rejected.returncode, 1)
                self.assertIn(f'maximum {limit}', rejected.stderr)


if __name__ == '__main__':
    unittest.main()
