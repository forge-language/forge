"""Execute the Forge-written stage2 CLI and the native binaries it produces."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--forge', required=True)
parser.add_argument('--stage0', default=None)
parser.add_argument('--source-root', default=None)
parser.add_argument('--root', required=True)
parser.add_argument('--lib-dir', required=True)
parser.add_argument('--cc', required=True)
config, remaining = parser.parse_known_args()
sys.argv = [sys.argv[0], *remaining]


class SelfHostedCompilerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='forge fg ; $ ')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.source = self.directory / 'hello world;$(touch INJECTED).fg'
        self.source.write_text('native main { println("Forge native 성공"); return 0; }\n')

    def invoke(self, *args, env=None):
        return subprocess.run([config.forge, *map(str, args)], cwd=self.directory,
                              text=True, capture_output=True, timeout=30, env=env)

    def native_flags(self):
        return ['--forge-root', config.root, '--lib-dir', config.lib_dir,
                '--cc', config.cc]

    def run_binary(self, path, expected='Forge native 성공\n'):
        result = subprocess.run([str(path)], cwd=self.directory,
                                text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, expected)

    def compare_native_compilers(self, source, expected):
        self.source.write_text(source)
        compilers = [config.forge]
        if config.stage0:
            compilers.append(config.stage0)
        for index, compiler in enumerate(compilers):
            with self.subTest(compiler=compiler):
                output = self.directory / f'comparison-{index}'
                result = subprocess.run(
                    [compiler, str(self.source), '-o', str(output), *self.native_flags()],
                    cwd=self.directory, text=True, capture_output=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.run_binary(output, expected)

    def test_default_native_output(self):
        result = self.invoke(self.source, *self.native_flags())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.run_binary(self.directory / self.source.stem)
        self.assertFalse((self.directory / 'INJECTED').exists())

    def test_explicit_output_and_compiler_argv(self):
        output = self.directory / 'native output;$(touch INJECTED)'
        wrapper = self.directory / 'cc wrapper;$(touch INJECTED)'
        record = self.directory / 'argv.json'
        wrapper.write_text('#!' + sys.executable + '\n'
                           'import json,subprocess,sys\n'
                           'open(' + repr(str(record)) + ',"w").write(json.dumps(sys.argv[1:]))\n'
                           'sys.exit(subprocess.call([' + repr(config.cc) + ']+sys.argv[1:]))\n')
        wrapper.chmod(0o755)
        result = self.invoke(self.source, '-o', output, '--forge-root', config.root,
                             '--lib-dir', config.lib_dir, '--cc', wrapper)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.run_binary(output)
        args = json.loads(record.read_text())
        self.assertIn(str(output), args)
        self.assertEqual(args[args.index('-I') + 1], str(Path(config.root) / 'include'))
        self.assertEqual(args[args.index('-L') + 1], config.lib_dir)
        self.assertLess(args.index('-lforge_runtime'), args.index('-lforge_std'))
        self.assertIn('-pthread', args)
        self.assertIn('-lm', args)
        self.assertFalse(Path(args[args.index('c') + 1]).exists())
        self.assertFalse((self.directory / 'INJECTED').exists())

    def test_c_emission_modes(self):
        for flags, name in [(['--emit-c'], self.source.stem + '.c'),
                            (['-o', 'legacy.c'], 'legacy.c'),
                            (['--emit-c', '-o', 'explicit.txt'], 'explicit.txt')]:
            with self.subTest(flags=flags):
                result = self.invoke(self.source, *flags)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('int main(', (self.directory / name).read_text())
        self.assertFalse((self.directory / self.source.stem).exists())

    def test_cli_errors(self):
        for args in [[], ['missing.fg'], [self.source, '--unknown'],
                     [self.source, '--emit-js'], [self.source, self.source],
                     [self.source, '-o'], [self.source, '--cc'],
                     [self.source, '--forge-root'], [self.source, '--lib-dir'],
                     [self.source, '-o', '--emit-c']]:
            with self.subTest(args=args):
                result = self.invoke(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('forge-fg:', result.stderr)

    def test_no_source_overwrite(self):
        original = self.source.read_bytes()
        for output in [self.source, Path('.') / self.source.name]:
            result = self.invoke(self.source, '--emit-c', '-o', output)
            self.assertNotEqual(result.returncode, 0, result.stderr)
            self.assertEqual(self.source.read_bytes(), original)
        alias = self.directory / 'alias.c'
        alias.symlink_to(self.source)
        result = self.invoke(self.source, '-o', alias)
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.source.read_bytes(), original)
        no_extension = self.directory / 'source'
        no_extension.write_text('native main { return 0; }\n')
        result = self.invoke('./source', *self.native_flags())
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(no_extension.read_text(), 'native main { return 0; }\n')

    def test_write_and_compiler_failures(self):
        for output in ['absent/output.c', self.directory]:
            result = self.invoke(self.source, '--emit-c', '-o', output)
            self.assertNotEqual(result.returncode, 0, result.stderr)
        if Path('/dev/full').exists():
            result = self.invoke(self.source, '--emit-c', '-o', '/dev/full')
            self.assertNotEqual(result.returncode, 0, result.stderr)
        result = self.invoke(self.source, '-o', 'absent/native', *self.native_flags())
        self.assertNotEqual(result.returncode, 0)
        result = self.invoke(self.source, '--forge-root', config.root,
                             '--lib-dir', config.lib_dir, '--cc', '/missing/compiler')
        self.assertNotEqual(result.returncode, 0)
        wrapper = self.directory / 'failing cc'
        record = self.directory / 'failed-argv.json'
        wrapper.write_text('#!' + sys.executable + '\n'
                           'import json,sys\n'
                           'open(' + repr(str(record)) + ',"w").write(json.dumps(sys.argv[1:]))\n'
                           'sys.exit(13)\n')
        wrapper.chmod(0o755)
        result = self.invoke(self.source, '--forge-root', config.root,
                             '--lib-dir', config.lib_dir, '--cc', wrapper)
        self.assertEqual(result.returncode, 13, result.stderr)
        args = json.loads(record.read_text())
        self.assertFalse(Path(args[args.index('c') + 1]).exists())

    def test_keep_temp_and_environment_defaults(self):
        env = dict(os.environ, FORGE_ROOT=config.root, CC=config.cc)
        result = self.invoke(self.source, '--lib-dir', config.lib_dir,
                             '--keep-temp', '-o', 'kept', env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        marker = 'forge-fg: temporary C: '
        line = next(line for line in result.stderr.splitlines() if line.startswith(marker))
        path = Path(line[len(marker):])
        self.addCleanup(path.unlink, missing_ok=True)
        self.assertTrue(path.is_file())
        self.assertIn('int main(', path.read_text())
        self.run_binary(self.directory / 'kept')

    def test_installed_layout_inference(self):
        prefix = self.directory / 'install prefix ; $'
        (prefix / 'bin').mkdir(parents=True)
        (prefix / 'include').symlink_to(Path(config.root) / 'include', target_is_directory=True)
        (prefix / 'lib').symlink_to(Path(config.lib_dir), target_is_directory=True)
        executable = prefix / 'bin' / 'forge-fg'
        shutil.copy2(config.forge, executable)
        env = dict(os.environ)
        env.pop('FORGE_ROOT', None)
        env['CC'] = config.cc
        result = subprocess.run([str(executable), str(self.source), '-o', 'inferred'],
                                cwd=self.directory, text=True, capture_output=True,
                                timeout=30, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.run_binary(self.directory / 'inferred')

    def test_language_subset_and_structure_errors(self):
        for source in ['native main { println("x");', 'native main { return (0; }',
                       'native main { println("unterminated); }',
                       'import tcp; native main { return 0; }',
                       'process main { coroutine work() { yield; } spawn work(); }',
                       'fn f(): float { return 1.5; }\nnative main { return 0; }',
                       'native main { let x: custom = 1; return 0; }',
                       'native main { let x: int; return 0; }',
                       'let x: int = 1;', '']:
            with self.subTest(source=source):
                self.source.write_text(source)
                result = self.invoke(self.source, '--emit-c', '-o', 'bad.c')
                self.assertNotEqual(result.returncode, 0, result.stderr)
                self.assertFalse((self.directory / 'bad.c').exists())

    def test_functions_control_flow_and_escaped_literals(self):
        self.source.write_text('''fn sum(n: int): int {
    let total: int = 0;
    let i: int = 0;
    while (i < n) { total = total + i; i = i + 1; }
    return total;
}
native main {
    // A comment with unmatched delimiters: } ) native main {
    println(sum(5));
    println("a\\\"b");
    return 0;
}
''')
        result = self.invoke(self.source, '-o', 'flow', *self.native_flags())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.run_binary(self.directory / 'flow', '10\na"b\n')

    def test_semantic_errors_preserve_outputs_before_emission_or_native_compilation(self):
        cases = [
            ('native main { println(missing); }', 'unknown value'),
            ('native main { missing(); }', 'unknown function'),
            ('native main { let x: int = "wrong"; }', 'initializer type mismatch'),
            ('native main { let x: string = 1; }', 'initializer type mismatch'),
            ('native main { let x: int = x; }', 'unknown value'),
            ('native main { let x: int = 0; x = "wrong"; }', 'assignment type mismatch'),
            ('native main { x = 1; }', 'assignment to unknown value'),
            ('native main { { let x: int = 1; } println(x); }', 'unknown value'),
            ('native main { if (1) { let x: int = 1; } else { println(x); } }', 'unknown value'),
            ('native main { for(let i: int = 0; i < 1; i = i + 1) {} println(i); }', 'unknown value'),
            ('native main { for(let i: int = 0; i < 1; i = x) { let x: int = 1; } }', 'unknown value'),
            ('native main { match 1 { 1 => { let x: int = 0; } _ => println(x); } }', 'unknown value'),
            ('native main { return "wrong"; }', 'return type mismatch'),
            ('process main { return 1; }', 'return type mismatch'),
            ('native main { break; }', 'outside loop'),
            ('native main { if (0) { continue; } }', 'outside loop'),
            ('native main { println("a" + 1); }', 'non-numeric operand'),
            ('native main { println("a" == "a"); }', 'non-numeric operand'),
            ('native main { println(!"a"); }', 'non-numeric unary operand'),
            ('native main { if ("a") {} }', 'non-numeric condition'),
            ('native main { while ("a") {} }', 'non-numeric condition'),
            ('native main { match "a" { _ => {} } }', 'integer value'),
            ('native main { if (0) { missing(); } }', 'unknown function'),
            ('native main { let x: int = missing() * 0; }', 'unknown function'),
            ('native main { return 0; println(missing); }', 'unknown value'),
            ('fn f(x: int): int { return x; }\nnative main { f(); }', 'argument count'),
            ('fn f(): int { return 1; }\nnative main { f(1); }', 'argument count'),
            ('fn f(x: int): int { return x; }\nnative main { f("a"); }', 'argument type mismatch'),
            ('fn f(): int { return "a"; }\nnative main {}', 'return type mismatch'),
            ('fn f(): int { return; }\nnative main {}', 'return type mismatch'),
            ('fn f(): void { return 1; }\nnative main {}', 'return type mismatch'),
            ('fn f(): void {}\nnative main { let x: int = f(); }', 'initializer type mismatch'),
            ('fn f(): void {}\nnative main { println(f()); }', 'scalar value'),
            ('fn f(): int { return 1; }\nnative main { let x: int = f; }', 'initializer type mismatch'),
            ('fn f(): int { return 1; }\nnative main { println(f + 1); }', 'non-numeric operand'),
            ('const VALUE = missing;\nnative main {}', 'unknown value'),
            ('const VALUE = local;\nnative main { let local: int = 1; println(VALUE); }', 'unknown value'),
            ('fn f(): int { println("return"); }\nnative main {}', 'reachable end'),
            ('fn f(flag: int): int { if (flag) { return 1; } }\nnative main {}', 'reachable end'),
            ('fn f(): int { while (false) { return 1; } }\nnative main {}', 'reachable end'),
            ('fn f(): int { while (true) { break; } }\nnative main {}', 'reachable end'),
            ('fn f(flag: int): int { while (flag) { return 1; } }\nnative main {}', 'reachable end'),
            ('fn f(n: int): int { match n { 1 => return 1; } }\nnative main {}', 'reachable end'),
            ('fn f(n: int): int { match n { 1 => return 1; _ => {} } }\nnative main {}', 'reachable end'),
            ('fn f(): int { while (true) { match 0 { _ => break; } } }\nnative main {}', 'reachable end'),
        ]
        wrapper = self.directory / 'unexpected compiler'
        record = self.directory / 'compiler-called'
        wrapper.write_text('#!' + sys.executable + '\n'
                           'from pathlib import Path\n'
                           'Path(' + repr(str(record)) + ').touch()\n')
        wrapper.chmod(0o755)
        for source, diagnostic in cases:
            self.source.write_text(source + '\n')
            if config.stage0:
                stage0 = subprocess.run([config.stage0, str(self.source), '--check'],
                                        text=True, capture_output=True, timeout=10)
                self.assertEqual(stage0.returncode, 1, (source, stage0.stderr))
                self.assertIn('forge: semantic:', stage0.stderr)
            for flags in [['--emit-c'], [], ['legacy']]:
                with self.subTest(source=source, flags=flags):
                    output = self.directory / ('rejected.c' if flags == ['legacy'] else 'rejected.out')
                    output.write_bytes(b'preserve existing output\x00\xff')
                    mode = [] if flags == ['legacy'] else flags
                    result = self.invoke(self.source, *mode, '-o', output,
                                         *self.native_flags(), '--cc', wrapper)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn('forge-fg: semantic:', result.stderr)
                    self.assertIn(diagnostic, result.stderr)
                    self.assertEqual(output.read_bytes(), b'preserve existing output\x00\xff')
                    self.assertFalse(record.exists())
            output.unlink()
            result = self.invoke(self.source, '--emit-c', '-o', output)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertFalse(output.exists())

    def test_subset_semantics_reject_malformed_declarations_expressions_and_builtins(self):
        cases = [
            'native main { let x: void = 1; }',
            'fn f(x: void): int { return 1; }\nnative main {}',
            'fn f(x: int, x: int): int { return x; }\nnative main {}',
            'fn f(x: int,): int { return x; }\nnative main {}',
            'fn f(x int): int { return x; }\nnative main {}',
            'fn f(): int { return 1; }\nfn f(): int { return 2; }\nnative main {}',
            'native main { let x: int = 1; let x: int = 2; }',
            'native main { let true: bool = false; }',
            'native main { let str_len: int = 0; }',
            'const X = 1;\nconst X = 2;\nnative main {}',
            'const X = Y;\nconst Y = X;\nnative main {}',
            'fn f(): void {}\nconst X = f();\nnative main {}',
            'const X = 1;\nnative main { let X: int = 2; }',
            'fn f(X: int): int { return X; }\nconst X = 1;\nnative main {}',
            'native main { let x: int = ; }',
            'native main { println(1 + ); }',
            'native main { println(1 2); }',
            'native main { return 0 }',
            'native main { break 1; }',
            'native main { println(str_len("a")); }',
            'import strings; native main { str_len(); }',
            'import strings; native main { str_len(1); }',
            'import strings; native main { str_concat("a", 1); }',
            'import strings; native main { str_view_sub(1, "a", 2); }',
            'import fs; native main { fs_write("a"); }',
            'import os; native main { os_command_arg("a", "b"); }',
            'import io; native main { read_fd(1, "a"); }',
            'native main { print_int("a"); }',
            'native main { print_str(1); }',
            'native main { println(print_int(1)); }',
            'native main { let x: string = "a"; x += "b"; }',
        ]
        for source in cases:
            with self.subTest(source=source):
                self.source.write_text(source + '\n')
                output = self.directory / 'malformed.c'
                result = self.invoke(self.source, '--emit-c', '-o', output)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn('forge-fg: semantic:', result.stderr)
                self.assertFalse(output.exists())

    def test_return_paths_forward_constants_and_nested_loop_exits(self):
        self.compare_native_compilers('''const VALUE = 3;
fn choose(flag: bool): string {
    if (flag) { return "yes"; } else { return "no"; }
}
fn select(n: int): int {
    match n { 0 => return 4; _ => if (n) { return 5; } else { return 6; } }
}
fn literal(): int { if ((true // literal with a comment
    )) { return 7; } }
fn endless(): int { while (true) { while (true) { break; } continue; } }
fn loop_return(): int { while (1) { return 8; } }
fn for_return(): int { for (let i: int = 0; true; i = i + 1) { return 10; } }
fn for_endless(): int { for (let i: int = 0; true; i = i + 1) { continue; } }
fn block(): bool { { return 9; } }
fn finish(): void { return; }
native main {
    let local: string = "local";
    let x: int = true;
    let y: bool = 2;
    x += y; x *= 2;
    println(VALUE, local, choose(y), select(0), select(2), literal(), loop_return(), block(), x);
    println(for_return());
    finish();
    return 0;
}
''', '3localyes457896\n10\n')

    def test_constant_aliases_resolve_against_global_scope(self):
        # Stage0's C backend currently requires constants to fold to literals;
        # forge-fg emits macros and supports aliases, including forward aliases.
        self.source.write_text('const ALIAS = VALUE;\nconst VALUE = 3;\n'
                               'native main { println(ALIAS); return 0; }\n')
        output = self.directory / 'aliases'
        result = self.invoke(self.source, '-o', output, *self.native_flags())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.run_binary(output, '3\n')

    def test_semantic_nesting_limits_reject_without_creating_output(self):
        sources = [
            'native main { println(' + '!' * 260 + 'true); }',
            'native main {' + '{' * 150 + 'println(1);' + '}' * 150 + '}',
        ]
        for source in sources:
            with self.subTest(source=source):
                self.source.write_text(source)
                output = self.directory / 'deep.c'
                result = self.invoke(self.source, '--emit-c', '-o', output)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn('nesting limit', result.stderr)
                self.assertFalse(output.exists())

    def test_expression_comments_preserve_native_semantics(self):
        self.compare_native_compilers('''fn sum(n: int): int {
    return n // ignored: } ) ; unsupported bool
        + 2;
}
native main {
    let value: int = sum(1 // ) ; }
        );
    println(value // generated closing punctuation must survive
        );
    println("string" // literal expressions also need comment removal
        );
    if ((value // ) } ;
        == 3)) { println("literal // ; ) } \\\"quote\\\""); }
    for (let i: int = 0 // ; ) }
        ; i < 2 // ; ) }
        ; i = i + 1 // ; ) }
        ) { println(i); }
    return 0 // ; }
        ;
}
''', '3\nstring\nliteral // ; ) } "quote"\n0\n1\n')

    def test_println_uses_expression_types_and_preserves_argument_order(self):
        self.compare_native_compilers('''import strings;
const GREETING = "constant";
fn text(): string { return "function"; }
fn number(): int { println(99); return 7; }
fn choose(value: string, unused: int): string { return value; }
fn show(value: string): void { println(value); }
native main {
    let value: string = "variable";
    println(value);
    println((text()));
    println(GREETING);
    show("parameter");
    if (1) { let value: int = 42; println(value); }
    println(value);
    println("prefix:", number(), "tail");
    println (choose("a,b", 1), // , ) ignored
        str_concat("c", "d"), (1 + (2 * 3)));
    println( // comments-only empty argument list
        );
    return 0;
}
''', 'variable\nfunction\nconstant\nparameter\n42\nvariable\nprefix:99\n7tail\na,bcd7\n\n')

    def test_process_println_string_values_and_mixed_arguments(self):
        self.compare_native_compilers('''fn message(): string { return "process"; }
process main {
    let value: string = message();
    println(value, ":", 3);
}
''', 'process:3\n')

    def test_println_rejects_empty_comma_arguments_before_emission(self):
        for arguments in [', 1', '1,', '1, ,2', '1, // comment\n',
                          '1, // comment\n , 2']:
            with self.subTest(arguments=arguments):
                self.source.write_text(f'native main {{ println({arguments}); return 0; }}\n')
                output = self.directory / 'invalid-print.c'
                result = self.invoke(self.source, '--emit-c', '-o', output)
                self.assertNotEqual(result.returncode, 0, result.stderr)
                self.assertIn('println argument', result.stderr)
                self.assertFalse(output.exists())

    def test_println_resolves_shadowed_values_after_nested_scopes(self):
        self.compare_native_compilers('''native main {
    let value: string = "outer";
    { let value: int = 1; println(value); }
    println(value);
    while (0) { let value: int = 2; println(value); }
    println(value);
    for (let value: int = 0; value < 1; value = value + 1) {
        println(value);
    }
    println(value);
    match 1 {
        1 => { let value: int = 3; println(value); }
        _ => { println(value); }
    }
    println(value);
    if (0) { let value: int = 4; println(value); }
    else { println(value); }
    println(value);
    return 0;
}
''', '1\nouter\nouter\n0\nouter\n3\nouter\nouter\nouter\n')

    def test_boolean_functions_literals_and_short_circuit(self):
        self.compare_native_compilers('''const ON = true;
const OFF = false;
fn identity(value: bool): bool { return value; }
fn even(n: int): bool {
    if (n == 0) { return true; }
    return odd(n - 1);
}
fn odd(n: int): bool {
    if (n == 0) { return false; }
    return even(n - 1);
}
fn effect(): bool { println("effect"); return true; }
fn true_value(): int { return 8; }
fn false_value(): int { return 9; }
native main {
    let ready: bool = identity(ON);
    println(ready, OFF, true, false);
    println(!ready, ready == true, false != ready);
    println(false && effect());
    println(true || effect());
    println(true && effect());
    println(false || effect());
    println(even(6), odd(6), even(7), odd(7));
    if (ready) { println("ready"); }
    else { println("wrong"); }
    while (ready) { ready = false; }
    println(ready);
    for (let running: bool = true; running; running = false) { println(5); }
    println(true_value(), false_value(), "true,false");
    return 0;
}
''', '1010\n011\n0\n1\neffect\n1\neffect\n1\n1001\nready\n0\n5\n89true,false\n')

    def test_boolean_constants_and_parenthesized_constant_expressions(self):
        self.compare_native_compilers('''const BASE = 1 + 2;
const NEGATIVE = -2;
const ENABLED = true;
const LABEL = "true;false";
native main {
    println(BASE * 3, 10 / NEGATIVE);
    println(ENABLED && !false, LABEL);
    return 0;
}
''', '9-5\n1true;false\n')

    def test_process_boolean_parameter_and_binding(self):
        self.compare_native_compilers('''fn invert(value: bool): bool { return !value; }
process main {
    let value: bool = invert(false);
    if (value) { println(value); }
}
''', '1\n')

    def test_existing_process_control_and_match_examples(self):
        expected = {'match': 'OK\nlucky seven\n',
                    'control_flow': '2\n0\n1\n2\n0\n1\n2\n1\n2\n3\n4\n6\n7\n'}
        for name, stdout in expected.items():
            with self.subTest(example=name):
                source = Path(config.source_root or config.root) / 'examples' / (name + '.fg')
                result = self.invoke(source, '-o', name, *self.native_flags())
                self.assertEqual(result.returncode, 0, result.stderr)
                self.run_binary(self.directory / name, stdout)

    def test_inline_prints_preserve_following_statements(self):
        self.source.write_text('native main { println(1); println(2); return 7; }\n')
        result = self.invoke(self.source, '-o', 'inline', *self.native_flags())
        self.assertEqual(result.returncode, 0, result.stderr)
        native = subprocess.run([str(self.directory / 'inline')], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(native.returncode, 7)
        self.assertEqual(native.stdout, '1\n2\n')

    def test_match_arm_keeps_statements_after_println(self):
        self.source.write_text('native main {\n match 1 {\n 1 => { println(1); return 7; }\n _ => { return 2; }\n }\n return 0;\n}\n')
        result = self.invoke(self.source, '-o', 'match-arm', *self.native_flags())
        self.assertEqual(result.returncode, 0, result.stderr)
        native = subprocess.run([str(self.directory / 'match-arm')], capture_output=True,
                                text=True, timeout=2)
        self.assertEqual(native.returncode, 7, native.stderr)
        self.assertEqual(native.stdout, '1\n')

    def test_multiline_inline_and_nested_match_arms(self):
        self.compare_native_compilers('''fn select(): int { println("select"); return 1; }
native main {
    match(select()) // comment with a fake opening brace {
    {
        1 // comment with a fake => }
        =>
        {
            println("first; => }");
            println("second");
            match(2) { 2 => println("nested"); _ => { println("wrong"); } }
        }
        _ => { println("wrong"); }
    }
    match 2 { 1 => println("wrong"); 2 => if(false) { println("wrong"); }
        else if(true) // comment with a fake brace {
        { println("branch"); } else { println("wrong"); }
        _ => println("wrong"); }
    match 9 { 0009 => { println("decimal"); } _ => { println("wrong"); } }
    match 9223372036854775807 {
        9223372036854775807 => println("maximum");
        _ => println("wrong");
    }
    match 0 { _ => {} }
    println("after");
    return 0;
}
''', 'select\nfirst; => }\nsecond\nnested\nbranch\ndecimal\nmaximum\nafter\n')

    def test_match_rejects_malformed_patterns_and_unreachable_arms(self):
        for arms in ['_ => {} 1 => {}', '_ => {} _ => {}', 'name => {}',
                     '-1 => {}', '9223372036854775808 => {}', '1 {}',
                     '1 =>', '1 => println(1)', '1 => if(1) println(1);',
                     '1 => println(1) 2 => println(2);']:
            with self.subTest(arms=arms):
                self.source.write_text(f'native main {{ match 1 {{ {arms} }} return 0; }}\n')
                output = self.directory / 'invalid-match.c'
                result = self.invoke(self.source, '--emit-c', '-o', output)
                self.assertNotEqual(result.returncode, 0, result.stderr)
                self.assertIn('match', result.stderr)
                self.assertFalse(output.exists())

    def test_match_arms_accept_loop_statements_and_keep_loop_control(self):
        self.compare_native_compilers('''native main {
    match 1 {
        1 => for(let i: int = 0; i < 2; i = i + 1) { println(i); }
        _ => {}
    }
    let n: int = 0;
    match 0 {
        0 => while(n < 2) { n = n + 1; println(n); }
        _ => {}
    }
    for(let i: int = 0; i < 4; i = i + 1) {
        match i {
            0 => continue // ignored ;
                ;
            1 => println("one");
            _ => break // ignored ;
                ;
        }
        println("tail");
    }
    return 0;
}
''', '0\n1\n1\n2\none\ntail\n')

    def test_match_statement_scan_rejects_excessive_else_nesting(self):
        arms = 'if(0) {} else ' * 300 + '{}'
        self.source.write_text(f'native main {{ match 1 {{ 1 => {arms} }} return 0; }}\n')
        output = self.directory / 'deep-match.c'
        result = self.invoke(self.source, '--emit-c', '-o', output)
        self.assertGreater(result.returncode, 0, result.stderr)
        self.assertIn('match arm requires a complete statement', result.stderr)
        self.assertFalse(output.exists())

    def test_for_continue_executes_iteration_step(self):
        self.source.write_text('native main {\n let total: int = 0;\n for (let i: int = 0; i < 4; i = i + 1) {\n  if (i == 1) { continue; }\n  total = total + i;\n }\n println(total);\n return 0;\n}\n')
        result = self.invoke(self.source, '-o', 'for-continue', *self.native_flags())
        self.assertEqual(result.returncode, 0, result.stderr)
        native = subprocess.run([str(self.directory / 'for-continue')], capture_output=True,
                                text=True, timeout=2)
        self.assertEqual(native.returncode, 0, native.stderr)
        self.assertEqual(native.stdout, '5\n')

    def test_keyword_prefixes_remain_identifiers(self):
        self.source.write_text('native main {\n let returning: int = 0;\n let breaking: int = 0;\n let continuing: int = 0;\n returning = 7; breaking = 8; continuing = 9;\n println(returning); println(breaking); println(continuing);\n return 0;\n}\n')
        result = self.invoke(self.source, '-o', 'identifiers', *self.native_flags())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.run_binary(self.directory / 'identifiers', '7\n8\n9\n')

    def test_builtin_prefixes_preserve_user_functions_and_short_names(self):
        names = ['os_total', 'str_total', 'fs_total', 'print_total',
                 'eprint_total', 'read_total', 'write_total', 'flush_total',
                 'stdin_total', 'stdout_total', 'stderr_total', 'q', 'os']
        declarations = '\n'.join(
            f'fn {name}(): int {{ return {index + 1}; }}'
            for index, name in enumerate(names))
        calls = '\n'.join(f'    println({name}());' for name in names)
        self.source.write_text(declarations + '\nnative main {\n' + calls +
                               '\n    return 0;\n}\n')
        result = self.invoke(self.source, '-o', 'prefixes', *self.native_flags())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.run_binary(self.directory / 'prefixes',
                        ''.join(f'{index + 1}\n' for index in range(len(names))))

    def test_string_constant_semicolons_and_escaped_quotes(self):
        self.source.write_text('const text = "a;b\\"c";\nnative main { print_str(text); println(); return 0; }\n')
        result = self.invoke(self.source, '-o', 'constant', *self.native_flags())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.run_binary(self.directory / 'constant', 'a;b"c\n')

    def test_string_from_int_handles_signed_boundaries(self):
        self.source.write_text('import strings;\nnative main {\n println(str_from_int(42));\n println(str_from_int(-9223372036854775807 - 1));\n println(str_from_int(9223372036854775807));\n return 0;\n}\n')
        result = self.invoke(self.source, '-o', 'conversion', *self.native_flags())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.run_binary(self.directory / 'conversion',
                        '42\n-9223372036854775808\n9223372036854775807\n')

    def test_native_filesystem_module_resolves_platform_archive(self):
        self.source.write_text('import fs; native main { println(fs_is_file("missing-forge-fixture")); return 0; }\n')
        result = self.invoke(self.source, '-o', 'filesystem', *self.native_flags())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.run_binary(self.directory / 'filesystem', '0\n')

    def test_native_bounded_string_apis(self):
        self.source.write_text('''import strings;
native main {
    let view: int = str_view("한글😀");
    println(str_view_len(view));
    println(str_view_matches(view, 3, "글"));
    println(str_view_sub(view, 3, 3));
    let builder: int = str_builder();
    str_builder_append_view(builder, view, 6, 4);
    println(str_builder_finish(builder));
    println(str_len("bytes"));
    return 0;
}
''')
        result = self.invoke(self.source, '-o', 'strings', *self.native_flags())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.run_binary(self.directory / 'strings', '10\n1\n글\n😀\n5\n')

    def test_help_version(self):
        for flag in ['--help', '--version']:
            result = self.invoke(flag)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('forge-fg', result.stdout)


if __name__ == '__main__':
    unittest.main()
