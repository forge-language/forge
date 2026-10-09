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

    def test_semantic_errors_are_rejected_before_every_backend(self):
        cases = [
            ('native main { println(missing_name); }', 'unknown value'),
            ('native main { let value: int = "wrong"; }', 'type mismatch'),
            ('fn add(a: int, b: int): int { return a + b; } native main { add(1); }', 'argument count'),
            ('fn f(a: int): int { return a; } native main { f("wrong"); }', 'type mismatch'),
            ('fn f(): int { return "wrong"; } native main { return 0; }', 'type mismatch'),
            ('fn f(): void { return 1; } native main { return 0; }', 'type mismatch'),
            ('native main { let x: int; println(x); }', 'uninitialized'),
            ('native main { let x: int; if (1) { x = 3; } println(x); }', 'uninitialized'),
            ('native main { let x: int; while (0) { x = 3; } println(x); }', 'uninitialized'),
            ('native main { { let x: int = 1; } println(x); }', 'unknown value'),
            ('native main { if (0) { missing_function(); } }', 'unknown function'),
            ('native main { let x: int = missing_function() * 0; }', 'unknown function'),
            ('native main { break; }', 'outside loop'),
            ('native main { yield; }', 'coroutine context'),
            ('process main { coroutine worker(n: int) {} spawn worker(); }', 'argument count'),
            ('native main { let x: int = x; }', 'unknown value'),
            ('import thread; fn wrong(n: string): int { return 0; } native main { thread_spawn(wrong, 1); }', 'callback parameters'),
        ]
        for source, diagnostic in cases:
            for mode in ['--check', '--emit-c', '--emit-js']:
                with self.subTest(source=source, mode=mode):
                    self.source.write_text(source)
                    output = self.source.parent / 'rejected.out'
                    output.write_text('preserve existing output')
                    result = self.invoke(self.source, mode, '-o', output)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn(diagnostic, result.stderr)
                    self.assertEqual(output.read_text(), 'preserve existing output')

    def test_scalar_function_references_and_fallthrough_are_rejected(self):
        cases = [
            ('fn f(): int { return 1; } native main { let n: int = f; }', 'type mismatch'),
            ('fn f(): int { return 1; } native main { println(f); }', 'function reference'),
            ('fn f(): int { return 1; } native main { println(f * 2); }', 'non-numeric'),
            ('fn f(): int { return 1; } native main { if (f) {} }', 'non-numeric'),
            ('fn f(): int { return 1; } fn consume(n: int): int { return n; } native main { consume(f); }', 'type mismatch'),
            ('extern fn opaque(cb: int): int; fn f(): int { return 1; } fn wrapper(cb: int): int { println(cb * 2); return opaque(cb); } native main { wrapper(f); }', 'type mismatch'),
            ('extern fn opaque(cb: int, n: int): int; fn f(): int { return 1; } fn wrapper(cb: int): int { return opaque(cb, cb * 2); } native main { wrapper(f); }', 'type mismatch'),
            ('fn f(): int { return 1; } fn g(): int { return f; } native main {}', 'type mismatch'),
            ('fn choose(flag: int): int { if (flag) { return 7; } } native main {}', 'reachable end'),
            ('fn f(n: int): int { match n { 1 => { return 7; } } } native main {}', 'reachable end'),
            ('fn f(): int { while (1) { break; } } native main {}', 'reachable end'),
            ('fn f(n: int): int { while (n) { return 1; } } native main {}', 'reachable end'),
        ]
        for source, diagnostic in cases:
            for mode in ['--check', '--emit-c', '--emit-js']:
                with self.subTest(source=source, mode=mode):
                    self.source.write_text(source)
                    output = self.source.parent / 'preserved.out'
                    output.write_text('existing output')
                    result = self.invoke(self.source, mode, '-o', output)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn(diagnostic, result.stderr)
                    self.assertEqual(output.read_text(), 'existing output')

    def test_reachable_returns_match_and_initialization(self):
        self.run_program('''fn choose(flag: int): int {
            if (flag) { return 7; } else { return 9; }
        }
        fn selected(n: int): int {
            match n { 1 => { return 11; } _ => { return 13; } }
        }
        fn initialized(flag: int): int {
            let value: int;
            if (flag) { return 17; } else { value = 19; }
            return value;
        }
        fn matched(n: int): int {
            let value: int;
            match n { 1 => { value = 23; } _ => { value = 29; } }
            return value;
        }
        native main { println(choose(0)); println(selected(1));
            println(initialized(0)); println(matched(0)); }
        ''', '9\n11\n19\n29\n')
        for source in ['fn forever(): int { while (1) { continue; } } native main {}',
                       'fn forever(): int { while (1) { while (1) { break; } } } native main {}',
                       'fn loop(): int { for (let i: int = 0; 1; i = i + 1) { return i; } } native main {}']:
            for mode in ['--check', '--emit-c', '--emit-js']:
                self.source.write_text(source)
                result = self.invoke(self.source, mode, '-o', self.source.parent / 'accepted.out')
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_contextual_function_callbacks_remain_supported(self):
        self.source.write_text('import thread; fn worker(n: int): int { return n; } native main { thread_spawn(worker, 1); }')
        result = self.invoke(self.source, '--check')
        self.assertEqual(result.returncode, 0, result.stderr)
        (self.source.parent / 'helper.fg').write_text('extern fn js_start(callback: int): int; fn start(callback: int): int { return js_start(callback); } fn callback(value: int): int { return value; }')
        self.source.write_text('import helper; native main { helper.start(helper.callback); }')
        result = self.invoke(self.source, '--emit-js', '-o', self.source.parent / 'callback.js')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_owned_transfer_state_and_unsupported_moves_are_rejected(self):
        cases = [
            ('native main { let n: int = 7; let result: int = move n; }', 'unsupported ownership'),
            ('native main { own let n: int = 7; }', 'initialized strings'),
            ('native main { own let s: string; }', 'initialized strings'),
            ('native main { let s: string = "borrowed"; send 0, Drop, move s; }', 'named owned string'),
            ('native main { send 0, Drop, move "literal"; }', 'named owned string'),
            ('native main { send 0, Drop, "literal"; }', 'integer value'),
            ('native main { send "invalid", Drop, 1; }', 'process handle'),
            ('native main { own let s: string = "literal"; send s, Drop, move s; }', 'process handle'),
            ('fn target(): int { return 0; } native main { send target, Drop, 1; }', 'process handle'),
            ('native main { send 1.5, Drop, 1; }', 'process handle'),
            ('native main { send true, Drop, 1; }', 'process handle'),
            ('native main { own let s: string = "literal"; send 0, Drop, move s; println(s); }', 'moved owned value'),
            ('native main { own let s: string = "literal"; if (1) { send 0, Drop, move s; } println(s); }', 'moved owned value'),
            ('native main { own let s: string = "literal"; match 1 { 1 => { send 0, Drop, move s; } _ => {} } println(s); }', 'moved owned value'),
            ('native main { own let s: string = "literal"; while (1) { send 0, Drop, move s; } }', 'moved owned value'),
            ('native main { own let s: string = "literal"; while (1) { if (1) { send 0, Drop, move s; break; } } println(s); }', 'moved owned value'),
            ('native main { own let s: string = "literal"; while (1) { match 1 { 1 => { send 0, Drop, move s; break; } _ => { break; } } } println(s); }', 'moved owned value'),
            ('native main { own let s: string = "literal"; while (1) { if (1) { send 0, Drop, move s; continue; } } }', 'moved owned value'),
            ('native main { own let s: string = "literal"; for (let i: int = 0; i < 2; i = i + 1) { send 0, Drop, move s; } }', 'moved owned value'),
        ]
        for source, diagnostic in cases:
            for mode in ['--check', '--emit-c', '--emit-js']:
                with self.subTest(source=source, mode=mode):
                    self.source.write_text(source)
                    output = self.source.parent / 'preserved.out'
                    output.write_text('existing output')
                    result = self.invoke(self.source, mode, '-o', output)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn(diagnostic, result.stderr)
                    self.assertEqual(output.read_text(), 'existing output')
        # Each iteration constructs a fresh owned handle. A consumed outer handle
        # is also reusable after an explicit reinitialization at the loop head.
        for source in ['native main { let destination: int = 0; send destination, Drop, 1; }',
                       'extern fn destination(): ptr; native main { send destination(), Drop, 1; }',
                       'native main { while (1) { own let s: string = "fresh"; send 0, Drop, move s; break; } }',
                       'native main { own let s: string = "first"; while (1) { s = "fresh"; send 0, Drop, move s; } }']:
            self.source.write_text(source)
            result = self.invoke(self.source, '--check')
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_unsupported_javascript_preserves_existing_output(self):
        cases = [
            'native main { own let value: string = "literal"; }',
            'native main { own let value: string = "literal"; send 0, Drop, move value; }',
            'native main { send 0, Drop, 7; }',
            'process main { coroutine worker() { yield; } spawn worker(); }',
        ]
        for source in cases:
            with self.subTest(source=source):
                self.source.write_text(source)
                checked = self.invoke(self.source, '--check')
                self.assertEqual(checked.returncode, 0, checked.stderr)
                output = self.source.parent / 'existing.js'
                output.write_bytes(b'const priorOutput = "keep me";\n')
                result = self.invoke(self.source, '--emit-js', '-o', output)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn('JavaScript backend', result.stderr)
                self.assertEqual(output.read_bytes(), b'const priorOutput = "keep me";\n')

    def test_semantic_initialization_and_shadowing(self):
        self.run_program('''native main {
            let x: int;
            if (1) { x = 7; } else { x = 9; }
            { let x: string = "inner"; println(x); }
            println(x);
            let y: int;
            y = 3;
            println(y);
            return 0;
        }''', 'inner\n7\n3\n')

    def test_imported_function_signatures_are_checked(self):
        (self.source.parent / 'helper.fg').write_text('fn value(n: int): string { return "ok"; }')
        self.source.write_text('import helper; native main { helper.value("wrong"); }')
        result = self.invoke(self.source, '--check')
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn('type mismatch', result.stderr)
        self.source.write_text('import helper; native main { helper.missing(); }')
        result = self.invoke(self.source, '--check')
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn('unknown function', result.stderr)

    def test_string_views_and_builder_snapshots(self):
        self.run_program('import strings;native main{let v: int=str_view("한글");println(str_view_len(v));println(str_view_at(v,0));println(str_view_at(v,9));let b: int=str_builder();str_builder_append(b,"one");let first: string=str_builder_finish(b);str_builder_char(b,65);println(first);println(str_builder_finish(b));println(str_builder_char(b,0));return 0;}', '6\n237\n-1\none\noneA\n0\n')

    def test_local_type_and_constant_resolution_follow_lexical_scopes(self):
        self.run_program('''const LABEL = "constant";
fn show(value: string): void {
    if (1) { let value: int = 11; println(value); }
    println(value);
}
native main {
    show("parameter");
    { let LABEL: int = 12; println(LABEL); }
    println(LABEL);
    if (0) { let LABEL: int = 13; println(LABEL); }
    else if (1) { println(LABEL); }
    for (let LABEL: int = 0; LABEL < 1; LABEL = LABEL + 1) {
        let LABEL: string = "body";
        println(LABEL);
    }
    println(LABEL);
    return 0;
}
''', '11\nparameter\n12\nconstant\nconstant\nbody\nconstant\n')

    def test_native_os_path_calls_print_strings_and_clean_temp_files(self):
        binary = self.compile_program('''import os; import fs;
native main {
    println(os_executable_path());
    let executable: string = os_executable_path();
    println(executable);
    println(os_temp_file());
    let temporary: string = os_temp_file();
    println(temporary);
    println(fs_exists(temporary));
    println(fs_remove(temporary));
    println(fs_exists(temporary));
    return 0;
}''')
        result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10)
        lines = result.stdout.splitlines()
        if len(lines) >= 3:
            # The direct temporary-file call hands its path to the caller.
            direct_temp = Path(lines[2])
            for path_text in lines[2:4]:
                path = Path(path_text)
                if path.is_absolute() and path.name.startswith(('forge-', 'frg')):
                    self.addCleanup(path.unlink, missing_ok=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(lines), 7, result.stdout)
        self.assertEqual(lines[:2], [str(binary.resolve()), str(binary.resolve())])
        self.assertTrue(direct_temp.is_file(), result.stdout)
        self.assertEqual(direct_temp.stat().st_size, 0)
        self.assertNotEqual(lines[2], lines[3])
        self.assertFalse(Path(lines[3]).exists())
        self.assertEqual(lines[4:], ['1', '1', '0'])

    def test_string_view_ranges_and_matches(self):
        self.run_program('''import strings;
native main {
    let v: int = str_view("한글😀");
    println(str_view_sub(v, 3, 3));
    println(str_view_sub(v, 6, 9223372036854775807));
    println(str_view_sub(v, 9223372036854775807, 1));
    println(str_view_matches(v, 3, "글"));
    println(str_view_matches(v, 6, "😀!"));
    println(str_view_matches(v, 10, ""));
    println(str_view_matches(v, 11, ""));
    println(str_view_matches(v, -1, ""));
    let b: int = str_builder();
    str_builder_append_view(b, v, 3, 3);
    let snapshot: string = str_builder_finish(b);
    let saved: int = str_view(snapshot);
    str_builder_append_view(b, v, 6, 1);
    str_builder_append_view(b, v, 7, 9223372036854775807);
    println(str_builder_append_view(b, v, -1, 1));
    println(str_builder_append_view(b, v, 0, -1));
    println(str_builder_append_view(b, 0, 0, 1));
    println(str_builder_finish(b));
    let i: int = 0;
    while (i < 1000) { str_builder_append_view(b, saved, 0, 3); i = i + 1; }
    println(str_len(str_builder_finish(b)));
    println(snapshot);
    return 0;
}''', '글\n😀\n\n1\n0\n1\n0\n0\n0\n0\n0\n글😀\n3007\n글\n')

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
        result = self.invoke(self.source, '-o', binary, *directories,
                             '--cc', config.cc, '--forge-root', config.root,
                             '--lib-dir', config.lib_dir)
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
