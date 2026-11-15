"""Exercise additive checked resource contracts through the compiler CLI."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
for option in ('forge', 'root', 'lib-dir', 'cc'):
    parser.add_argument('--' + option, required=True)
config, remaining = parser.parse_known_args()
sys.argv = [sys.argv[0], *remaining]


class SafetyTests(unittest.TestCase):
    def check(self, source: str, message: str | None = None) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path, output = Path(directory) / 'safe.fg', Path(directory) / 'safe.c'
            path.write_text(source)
            output.write_text('preserve existing artifact')
            legacy = subprocess.run([config.forge, str(path), '--check'],
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(legacy.returncode, 0, legacy.stderr)
            result = subprocess.run([config.forge, str(path), '--safe', '--check',
                                     '--diagnostics-json', '-o', str(output)],
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(output.read_text(), 'preserve existing artifact')
            if message is None:
                self.assertEqual(result.returncode, 0, result.stderr)
            else:
                self.assertEqual(result.returncode, 1, result.stderr)
                diagnostic = json.loads(result.stderr)
                self.assertEqual(diagnostic['phase'], 'safety')
                self.assertIn(message, diagnostic['message'])

    def test_forged_wrong_kind_and_raw_ffi_boundaries(self) -> None:
        cases = (
            ('import strings;native main{println(str_view_len(123));}', 'wrong-kind'),
            ('import strings;native main{let b:int=str_builder();if(b==0){return 1;}println(str_view_len(b));}', 'wrong-kind'),
            ('import strings;native main{let v:int=str_view("x");println(v+1);}', 'scalar arithmetic'),
            ('extern fn address():ptr;native main{println(address()[0]);}', 'raw pointer'),
            ('extern fn foreign():int;native main{println(foreign());}', 'extern'),
            ('import tcp;native main{tcp_close(123);}', 'Legacy ABI'),
        )
        for source, message in cases:
            with self.subTest(source=source):
                self.check(source, message)

    def test_callback_abi_is_rejected_in_both_modes(self) -> None:
        for declaration in ('fn work(x:float):int{return 0;}',
                            'fn work(x:int):string{return "wrong";}'):
            with tempfile.TemporaryDirectory() as directory:
                source = Path(directory) / 'callback.fg'
                source.write_text(declaration + 'native main{let _:int=thread_spawn(work,1);}')
                for flags in ([], ['--safe']):
                    result = subprocess.run([config.forge, str(source), '--check', *flags],
                                            capture_output=True, text=True, timeout=20)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn('callback', result.stderr)
        self.check('fn work(x:int):int{return x;}native main{let _:int=thread_spawn(work,1);return 0;}')

    def test_arena_reset_aliases_and_function_summaries(self) -> None:
        cases = (
            ('let s:string=str_concat("a","b");str_reset_arena();println(s);', 'invalidation'),
            ('let v:int=str_view("a");let alias:int=v;str_reset_arena();println(str_view_len(alias));', 'invalidation'),
            ('let b:int=str_builder();if(b==0){return 1;}let s:string=str_builder_finish(b);str_reset_arena();println(s);', 'invalidation'),
            ('let v:int=str_view("a");if(1){str_reset_arena();}println(str_view_len(v));', 'invalidation'),
            ('let s:string=str_concat("a","b");reset();println(s);', 'invalidation'),
            ('let v:int=identity(str_view("a"));str_reset_arena();println(str_view_len(v));', 'invalidation'),
            ('let v:int=str_view("a");while(1){println(str_view_len(v));str_reset_arena();}', 'invalidation'),
            ('let v:int=str_view("a");while(1){str_reset_arena();break;}println(str_view_len(v));', 'invalidation'),
        )
        prefix = 'import strings;fn reset(){str_reset_arena();}fn identity(x:int):int{return x;}'
        for body, message in cases:
            with self.subTest(body=body):
                self.check(prefix + 'native main{' + body + 'return 0;}', message)

    def test_failure_results_are_tested_returned_or_explicitly_discarded(self) -> None:
        for body in ('str_builder();', 'let b:int=str_builder();',
                     'let b:int=str_builder();if(b==0){return 1;}str_builder_append(b,"x");'):
            with self.subTest(body=body):
                self.check('import strings;native main{' + body + 'return 0;}', 'failure')
        self.check('import strings;native main{let _:int=str_builder();return 0;}')
        self.check('import strings;native main{let b:int=str_builder();if(b==0){return 1;}'
                   'if(str_builder_append(b,"x")==0){return 2;}println(str_builder_finish(b));return 0;}')

    def test_suspension_keeps_local_arena_but_spawn_rejects_foreign_loan(self) -> None:
        self.check('import strings;process main{coroutine c(){let v:int=str_view("x");'
                   'yield;println(str_view_len(v));}spawn c();}')
        self.check('import strings;process main{coroutine child(s:string){println(s);}'
                   'let s:string=str_concat("x","y");spawn child(s);}', 'another arena or worker')
        self.check('import strings;process main{coroutine child(s:string){println(s);}spawn child("static");}')

    def test_last_use_static_return_and_independent_builder_snapshot(self) -> None:
        self.check('import strings;fn identity(s:string):string{return s;}native main{'
                   'let s:string=identity("static");str_reset_arena();println(s);return 0;}')
        self.check('import strings;native main{let v:int=str_view("x");println(str_view_len(v));'
                   'str_reset_arena();println(str_view_len(0));return 0;}')
        self.check('import strings;native main{let b:int=str_builder();if(b==0){return 1;}'
                   'if(str_builder_append(b,"a")==0){return 2;}let first:string=str_builder_finish(b);'
                   'if(str_builder_append(b,"b")==0){return 3;}println(first);println(str_builder_finish_take(b));return 0;}')

    def test_checked_string_bounds_and_snapshots_execute_at_o0_o3_and_node(self) -> None:
        source = '''import strings;
fn identity(s:string):string{return s;}
native main{
 let b:int=str_builder();if(b==0){return 1;}
 if(str_builder_append(b,"a")==0){return 2;}
 let first:string=str_builder_finish(b);
 if(str_builder_append(b,"b")==0){return 3;}
 println(first);println(str_builder_finish_take(b));
 let s:string=identity("static");str_reset_arena();println(s);
 println("x"[-1]);println("x"[1]);println("x"[9223372036854775807]);
 println(str_view_at(0,0));return 0;
}'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture, generated = root / 'safe.fg', root / 'safe.c'
            fixture.write_text(source)
            emitted = subprocess.run([config.forge, str(fixture), '--safe', '--emit-c', '-o', str(generated)],
                                     capture_output=True, text=True, timeout=20)
            self.assertEqual(emitted.returncode, 0, emitted.stderr)
            expected = 'a\nab\nstatic\n-1\n-1\n-1\n-1\n'
            for optimization in ('-O0', '-O3'):
                binary = root / 'safe'
                compiled = subprocess.run(
                    [config.cc, '-std=c11', optimization, '-fsanitize=undefined', '-fno-sanitize-recover=all',
                     '-I', str(Path(config.root) / 'include'), str(generated),
                     '-L', config.lib_dir, '-lforge_std', '-lforge_runtime', '-pthread', '-lm', '-o', str(binary)],
                    capture_output=True, text=True, timeout=30)
                self.assertEqual(compiled.returncode, 0, compiled.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, expected)
                self.assertEqual(result.stderr, '')
            script = root / 'safe.js'
            emitted = subprocess.run([config.forge, str(fixture), '--safe', '--emit-js', '-o', str(script)],
                                     capture_output=True, text=True, timeout=20)
            self.assertEqual(emitted.returncode, 0, emitted.stderr)
            result = subprocess.run(['node', str(script)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, expected)


if __name__ == '__main__':
    unittest.main()
