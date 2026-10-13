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

    def test_excessive_statement_nesting_has_controlled_diagnostic(self):
        # A bounded input exceeds the parser's guard by only a small margin;
        # exercise both block and else-if recursion without a giant payload.
        for source in ('native main {' + '{' * 600 + '}' * 600 + '}',
                       'native main {' + 'if(0){}else ' * 600 + '{} }'):
            with self.subTest(source_prefix=source[:40]):
                self.source.write_text(source)
                result = self.invoke(self.source, '--check')
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn('statement nesting too deep', result.stderr)

    def compile_program(self, source, cc=config.cc):
        self.source.write_text(source)
        binary = Path(self.temp.name) / 'test'
        result = self.invoke(self.source, '-o', binary, '--cc', cc,
                             '--forge-root', config.root, '--lib-dir', config.lib_dir)
        self.assertEqual(result.returncode, 0, result.stderr)
        return binary

    def test_append_loop_lowering_builds_with_abi_only_string_header(self):
        wrapper = Path(self.temp.name) / 'cc-no-inline'
        wrapper.write_text(
            '#!/usr/bin/env python3\n'
            'import os, sys\n'
            f'cc = {config.cc!r}\n'
            'os.execv(cc, [cc, "-DFORGE_STRING_NO_INLINE", *sys.argv[1:]])\n')
        wrapper.chmod(0o755)
        binary = self.compile_program('''import strings;
            native main {
                let text: string = "";
                let i: int = 0;
                while (i < 3) { text = str_append(text, 65 + i); i = i + 1; }
                println(text);
            }''', str(wrapper))
        result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'ABC\n')

    def test_view_index_bounds_elided_only_for_guarded_zero_based_loop(self):
        optimized = '''import strings;
fn weighted(view:int):int {
    let n:int=str_view_len(view); let i:int=0; let sum:int=0;
    while(i<n){sum=sum+str_view_at(view,i);i=i+1;}
    return sum;
}
native main {let view:int=str_view("ABC");println(weighted(view));return 0;}'''
        self.source.write_text(optimized)
        generated_c = Path(self.temp.name) / 'optimized.c'
        result = self.invoke(self.source, '--emit-c', '-o', generated_c)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('fr_detail_str_view_at_unchecked(view,i)', generated_c.read_text())
        self.run_program(optimized, '198\n')

        non_tail_increment = optimized.replace(
            'while(i<n){sum=sum+str_view_at(view,i);i=i+1;}',
            'while(i<n){sum=sum+str_view_at(view,i);i=i+1;sum=sum+1;}')
        self.source.write_text(non_tail_increment)
        generated_c = Path(self.temp.name) / 'non-tail-increment.c'
        result = self.invoke(self.source, '--emit-c', '-o', generated_c)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('fr_detail_str_view_at_unchecked(view,i)', generated_c.read_text())
        self.run_program(non_tail_increment, '201\n')

        access_after_increment = optimized.replace(
            'while(i<n){sum=sum+str_view_at(view,i);i=i+1;}',
            'while(i<n){i=i+1;sum=sum+str_view_at(view,i);}')
        self.source.write_text(access_after_increment)
        generated_c = Path(self.temp.name) / 'access-after-increment.c'
        result = self.invoke(self.source, '--emit-c', '-o', generated_c)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('fr_str_view_at(view,i)', generated_c.read_text())
        self.assertNotIn('fr_detail_str_view_at_unchecked(view,i)', generated_c.read_text())

        negative_start = '''import strings;
fn weighted(view:int):int {
    let n:int=str_view_len(view); let i:int=-1; let sum:int=0;
    while(i<n){sum=sum+str_view_at(view,i);i=i+1;}
    return sum;
}
native main {let view:int=str_view("ABC");println(weighted(view));return 0;}'''
        self.source.write_text(negative_start)
        generated_c = Path(self.temp.name) / 'checked.c'
        result = self.invoke(self.source, '--emit-c', '-o', generated_c)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('fr_str_view_at(view,i)', generated_c.read_text())
        self.assertNotIn('fr_detail_str_view_at_unchecked(view,i)', generated_c.read_text())
        self.run_program(negative_start, '197\n')

        negative_before_bound = optimized.replace(
            'let n:int=str_view_len(view); let i:int=0; let sum:int=0;',
            'let i:int=0; i=-1; let n:int=str_view_len(view); let sum:int=0;')
        self.source.write_text(negative_before_bound)
        generated_c = Path(self.temp.name) / 'negative-before-bound.c'
        result = self.invoke(self.source, '--emit-c', '-o', generated_c)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('fr_str_view_at(view,i)', generated_c.read_text())
        self.assertNotIn('fr_detail_str_view_at_unchecked(view,i)', generated_c.read_text())
        self.run_program(negative_before_bound, '197\n')

        invalidated_before_loop = optimized.replace(
            'let n:int=str_view_len(view); let i:int=0; let sum:int=0;',
            'let n:int=str_view_len(view); str_reset_arena(); let i:int=0; let sum:int=0;')
        self.source.write_text(invalidated_before_loop)
        generated_c = Path(self.temp.name) / 'invalidated.c'
        result = self.invoke(self.source, '--emit-c', '-o', generated_c)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('fr_str_view_at(view,i)', generated_c.read_text())
        self.assertNotIn('fr_detail_str_view_at_unchecked(view,i)', generated_c.read_text())

        invalidated_in_loop = optimized.replace(
            'while(i<n){sum=sum+str_view_at(view,i);i=i+1;}',
            'while(i<n){str_reset_arena();sum=sum+str_view_at(view,i);i=i+1;}')
        self.source.write_text(invalidated_in_loop)
        generated_c = Path(self.temp.name) / 'invalidated-body.c'
        result = self.invoke(self.source, '--emit-c', '-o', generated_c)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('fr_str_view_at(view,i)', generated_c.read_text())
        self.assertNotIn('fr_detail_str_view_at_unchecked(view,i)', generated_c.read_text())

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

    def test_native_service_string_results_print_as_strings(self):
        self.run_program('''import proc; import lsprpc; import docstore; import json;
        native main {
            doc_set("key", "stored");
            println(doc_get("key"));
            println(json_get_path_str("{\\"value\\":\\"decoded\\"}", "value"));
            println(json_get_path("{\\"value\\":42}", "value"));
            println(json_array_at("[\\"item\\"]", 0));
            println(proc_stdout(-1)); println(proc_stderr(-1));
            println(proc_output()); println(lsp_message());
        }''', 'stored\ndecoded\n42\n"item"\n\n\n\n\n')

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

    def test_builder_finish_take_preserves_snapshot_and_allows_reuse(self):
        self.run_program('''import strings; native main {
            let b: int = str_builder_with_capacity(3);
            str_builder_append(b, "one");
            let first: string = str_builder_finish_take(b);
            str_builder_char(b, 65);
            println(first);
            println(str_builder_finish(b));
            println(str_builder_finish_take(b));
            return 0;
        }''', 'one\nA\nA\n')

    def test_cached_length_immutable_view_append(self):
        self.run_program('''import strings; native main {
            let old: int = str_view("A");
            let high: int = str_view_append_byte(old, 255);
            let nul: int = str_view_append_byte(high, 0);
            let next: int = str_view_append_byte(nul, 66);
            println(str_view_len(old)); println(str_view_len(high));
            println(str_view_at(high, 1)); println(str_view_len(nul));
            println(str_view_len(next)); println(str_view_at(next, 2));
            println(str_view_append_byte(0, 65)); return 0;
        }''', '1\n2\n255\n2\n3\n66\n0\n')

    def view_append_c(self, source):
        self.source.write_text(source)
        output = Path(self.temp.name) / 'view.c'
        result = self.invoke(self.source, '--emit-c', '-o', output)
        self.assertEqual(result.returncode, 0, result.stderr)
        return output.read_text()

    def test_reserved_builder_loop_requires_private_pure_construction(self):
        source = '''import strings;
fn make(n:int,seed:int):string {
 let b:int=str_builder_with_capacity(n);if(b==0){return "";}let i:int=0;
 while(i<n){if(str_builder_char(b,65+(i+seed)%26)==0){return "";}i=i+1;}
 return str_builder_finish_take(b);
}
native main {println(make(3,0));return 0;}'''
        generated = self.view_append_c(source)
        self.assertIn('__forge_reserved_builder_', generated)
        self.assertIn('fr_str_builder_char(', generated)  # checked fallback
        self.run_program(source, 'ABC\n')
        cases = {
            'escaped builder': source.replace('let i:int=0;', 'let alias:int=b;let i:int=0;'),
            'observer': source.replace('let i:int=0;', 'println(b);let i:int=0;'),
            'nonzero index': source.replace('let i:int=0;', 'let i:int=1;'),
            'dynamic divisor': source.replace('(i+seed)%26', '(i+seed)%n'),
            'NUL': source.replace('65+(i+seed)%26', 'i%26'),
            'high byte': source.replace('65+(i+seed)%26', '255+(i+seed)%26'),
            'extra loop call': source.replace('i=i+1;', 'println(i);i=i+1;'),
            'extra control flow': source.replace('i=i+1;', 'if(i==1){break;}i=i+1;'),
            'different bound': source.replace('while(i<n)', 'while(i<3)'),
            'capacity mutation': source.replace('let i:int=0;', 'n=n+1;let i:int=0;'),
            'builder in byte': source.replace('(i+seed)%26', '(i+b)%26'),
            'nonunit increment': source.replace('i=i+1;', 'i=i+2;'),
        }
        for label, candidate in cases.items():
            with self.subTest(label=label):
                self.assertNotIn('__forge_reserved_builder_', self.view_append_c(candidate))
        collision = source.replace('let b:int=', 'let __forge_reserved_builder_1:int=seed;let b:int=')
        self.assertNotIn('fr_detail_str_builder *__forge_reserved_builder_1 =', self.view_append_c(collision))

    def test_mersenne_loop_version_rejects_unproven_recurrences(self):
        source = '''fn run(seed:int,n:int):int {
 let v:int=seed;let i:int=0;
 while(i<n){v=(v*48271)%2147483647;i=i+1;}return v;
}
native main {println(run(1,3));return 0;}'''
        self.assertIn('__forge_mersenne_product_', self.view_append_c(source))
        cases = {
            'observer': source.replace('i=i+1;', 'println(v);i=i+1;'),
            'changing bound': source.replace('while(i<n)', 'while(i<v)'),
            'different value': source.replace('(v*48271)', '(seed*48271)'),
            'overflowing closed range': source.replace('2147483647', '9223372036854775807'),
            'non-Mersenne': source.replace('2147483647', '2147483646'),
            'nonunit step': source.replace('i=i+1;', 'i=i+2;'),
            'dynamic coefficient': source.replace('(v*48271)', '(v*seed)'),
            'call in condition': source.replace('fn run', 'fn tick(n:int):int{println(n);return n;}fn run').replace('i<n', 'i<tick(n)'),
        }
        for label, candidate in cases.items():
            with self.subTest(label=label):
                self.assertNotIn('__forge_mersenne_product_', self.view_append_c(candidate))
        collision=source.replace('let v:int=', 'let __forge_mersenne_product_1:int=seed;let v:int=')
        self.assertNotIn('uint64_t __forge_mersenne_product_1 =', self.view_append_c(collision))
        # The original loop predicate is evaluated before the recurrence guard.
        empty = source.replace('run(1,3)', 'run(0,0)')
        generated = self.view_append_c(empty)
        self.assertIn('if ((\n(i < n)) && v >= 0', generated)
        self.run_program(empty, '0\n')

    def test_private_view_append_loop_lowering(self):
        # Runtime dimensions and signed wrapping byte arithmetic mirror the
        # construction shape, without timing or modifying benchmark fixtures.
        source = '''import strings;
fn make(n:int,seed:int):string {
    let view:int=str_view("");let i:int=0;
    while(i<n){view=str_view_append_byte(view,65+(i+seed)%26);i=1+i;}
    return str_view_sub(view,0,str_view_len(view));
}
native main {
    println(make(-1,0));println(make(0,0));println(make(3,0));
    println(make(3,-2));println(make(3,9223372036854775807));
    let view:int=str_view("");let i:int=0;
    while(i<200){view=str_view_append_byte(view,65);i=i+1;}
    println(str_view_len(view));println(str_view_at(view,199));
    let saved:int=view;str_builder_char(str_builder(),66);
    println(str_view_len(saved));return 0;
}'''
        generated = self.view_append_c(source)
        self.assertIn('__forge_view_builder_', generated)
        self.assertNotIn('fr_str_view_append_byte(', generated)
        self.assertIn('fr_str_builder_finish_take(', generated)
        # Signed remainder: 2**63-1 % 26 = 7, then wrapping gives -8, -7.
        self.run_program(source, '\n\nABC\n?@A\nH9:\n200\n65\n200\n')
        collision = '''import strings;native main {
            let __forge_view_builder_1:int=3;let view:int=str_view("");
            let i:int=0;while(i<__forge_view_builder_1){
                view=str_view_append_byte(view,65);i=i+1;}
            println(str_view_sub(view,0,3));return 0;}'''
        self.assertNotIn('int64_t __forge_view_builder_1 = 0;', self.view_append_c(collision))
        self.run_program(collision, 'AAA\n')

    def test_private_view_append_rejects_unproven_construction(self):
        base = '''import strings;extern fn opaque(value:int):int;
native main {let view:int=str_view("");let i:int=0;
while(i<3){view=str_view_append_byte(view,65);i=i+1;}
println(str_view_len(view));return 0;}'''
        cases = {
            'nonempty': base.replace('str_view("")', 'str_view("A")'),
            'null': base.replace('str_view("")', '0'),
            'nul': base.replace('view,65)', 'view,0)'),
            'high byte': base.replace('view,65)', 'view,255)'),
            'narrowed nul': base.replace('view,65)', 'view,256)'),
            'unknown byte': base.replace('view,65)', 'view,i)'),
            'handle byte': base.replace('view,65)', 'view,65+view%26)'),
            'handle bound': base.replace('i<3', 'i<view'),
            'alias': base.replace('let i:int=0;', 'let alias:int=view;let i:int=0;'),
            'escape before loop': base.replace('let i:int=0;', 'opaque(view);let i:int=0;'),
            'escape in loop': base.replace('i=i+1;', 'opaque(view);i=i+1;'),
            'observed prefix': base.replace('i=i+1;', 'println(str_view_len(view));i=i+1;'),
            'reset before loop': base.replace('let i:int=0;', 'str_reset_arena();let i:int=0;'),
            'reset in loop': base.replace('i=i+1;', 'str_reset_arena();i=i+1;'),
            'effectful byte': base.replace('view,65)', 'view,65+opaque(i)%26)'),
            'nonzero index': base.replace('let i:int=0;', 'let i:int=-1;'),
            'computed bound': base.replace('i<3', 'i<i+1'),
            'continue': base.replace('i=i+1;', 'i=i+1;continue;'),
            'shadowed view': base.replace('i=i+1;', '{let view:int=0;}i=i+1;'),
            'outer view binding': base.replace('native main {',
                'native main {let view:int=0;{').replace('return 0;}', 'return 0;}}'),
            'outer index binding': base.replace('native main {',
                'native main {let i:int=2;{').replace('return 0;}', 'return 0;}}'),
            'float bound': base.replace('native main {',
                'native main {let n:float=3.0;').replace('i<3', 'i<n'),
            'deep byte': base.replace('view,65)', 'view,65+(' + 'i+' * 70 + '0)%26)'),
            'process': base.replace('native main', 'process main').replace('return 0;', ''),
            'coroutine': base.replace('native main {', 'process main {coroutine work(){')
                .replace('return 0;}', '}spawn work();}'),
            'suspension': base.replace('native main {', 'process main {coroutine work(){')
                .replace('i=i+1;', 'yield;i=i+1;').replace('return 0;}', '}spawn work();}'),
        }
        for label, source in cases.items():
            with self.subTest(case=label):
                generated = self.view_append_c(source)
                self.assertNotIn('__forge_view_builder_', generated)
                self.assertIn('fr_str_view_append_byte(', generated)
        self.run_program(cases['nul'].replace('extern fn opaque(value:int):int;', ''), '0\n')
        self.run_program(cases['high byte'].replace('extern fn opaque(value:int):int;', ''), '3\n')
        self.run_program(cases['nonzero index'].replace('extern fn opaque(value:int):int;', ''), '4\n')

    def test_private_view_append_failure_and_trap_contract_with_external_abi(self):
        header = Path(self.temp.name) / 'faults.h'
        header.write_text('''#define FORGE_STRING_NO_INLINE
#include "forge/string.h"
static int64_t fault_mode, view_calls, builder_calls, byte_calls, finish_calls;
void test_mode(int64_t mode) {
    fault_mode=mode;view_calls=builder_calls=byte_calls=finish_calls=0;
}
static int64_t test_view(const char *s) {
    ++view_calls;
    if ((fault_mode==1 && view_calls==1) || (fault_mode==5 && view_calls==2)) return 0;
    return fr_str_view(s);
}
static int64_t test_builder(void) {
    ++builder_calls;
    return fault_mode==2 ? 0 : fr_str_builder();
}
static int64_t test_byte(int64_t b,int64_t ch) {
    ++byte_calls;
    if ((fault_mode==3 && byte_calls==3) || (fault_mode==6 && byte_calls==64)) return 0;
    return fr_str_builder_char(b,ch);
}
static char *test_finish(int64_t b) {
    ++finish_calls;
    return fault_mode==4 ? 0 : fr_str_builder_finish_take(b);
}
int64_t test_counts(void) {return builder_calls*10000+byte_calls*100+finish_calls*10+view_calls;}
#define fr_str_view(s) test_view(s)
#define fr_str_builder() test_builder()
#define fr_str_builder_char(b,ch) test_byte((b),(ch))
#define fr_str_builder_finish_take(b) test_finish(b)
''')
        wrapper = Path(self.temp.name) / 'fault-cc'
        wrapper.write_text('#!/usr/bin/env python3\nimport os,sys\n'
                           f'cc={config.cc!r}\n'
                           f'os.execv(cc,[cc,"-include",{str(header)!r},*sys.argv[1:]])\n')
        wrapper.chmod(0o755)
        source = '''import strings;
extern fn test_mode(mode:int):void;extern fn test_counts():int;
fn run(mode:int,n:int):void {
    test_mode(mode);let view:int=str_view("");let i:int=0;
    while(i<n){view=str_view_append_byte(view,65+i%26);i=i+1;}
    println(str_view_len(view));println(i);println(test_counts());
}
native main {run(0,8);run(1,8);run(2,8);run(3,8);run(4,8);run(5,8);
    run(0,0);run(1,0);run(0,200);run(6,200);return 0;}'''
        self.assertIn('__forge_view_builder_', self.view_append_c(source))
        binary = self.compile_program(source, str(wrapper))
        result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout,
                         '8\n8\n10812\n0\n8\n1\n0\n8\n10001\n'
                         '0\n8\n10301\n0\n8\n10811\n0\n8\n10812\n'
                         '0\n0\n1\n0\n0\n1\n200\n200\n30012\n0\n200\n16401\n')
        # Null and failed-growth paths must still evaluate subsequent pure
        # arguments, so a later division by zero remains an observable trap.
        expressions = ('65+(1/(3-i))%26',
                       '65+((-9223372036854775807-1)/(i-3))%26',
                       '65+((-9223372036854775807-1)%(i-3))%26')
        for expression, mode in ((expr, mode) for expr in expressions for mode in (0, 1, 2, 3)):
            trap = source[:source.index('native main')] + f'''native main {{
                test_mode({mode});let view:int=str_view("");let i:int=0;
                while(i<4){{view=str_view_append_byte(view,{expression});i=i+1;}}
                println(str_view_len(view));return 0;}}'''
            with self.subTest(expression=expression, mode=mode):
                self.assertIn('__forge_view_builder_', self.view_append_c(trap))
                binary = self.compile_program(trap, str(wrapper))
                result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Forge integer', result.stderr)

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
