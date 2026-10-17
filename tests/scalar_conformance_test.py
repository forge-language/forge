"""Execute scalar conversion and printing through both stage0 backends."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
for option in ('forge', 'root', 'lib-dir', 'cc'):
    parser.add_argument('--' + option, required=True)
parser.add_argument('--node', default='node')
config, remaining = parser.parse_known_args()
sys.argv = [sys.argv[0], *remaining]


class ScalarConformanceTests(unittest.TestCase):
    def execute(self, source: str, expected: str, *, javascript: bool = True,
                modules: dict[str, str] | None = None, bridge: str = '') -> None:
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            fg = work / 'main.fg'
            fg.write_text(source)
            for name, text in (modules or {}).items():
                (work / name).write_text(text)
            generated = work / 'main.c'
            emitted = subprocess.run(
                [config.forge, str(fg), '--emit-c', '-o', str(generated)],
                capture_output=True, text=True, timeout=20)
            self.assertEqual(emitted.returncode, 0, emitted.stderr)
            extra: list[str] = []
            if bridge:
                path = work / 'bridge.c'
                path.write_text(bridge)
                extra.append(str(path))
            for optimization in ('-O0', '-O3'):
                with self.subTest(backend='native', optimization=optimization):
                    binary = work / 'program'
                    compiled = subprocess.run(
                        [config.cc, '-std=c11', optimization, '-Werror',
                         '-fsanitize=undefined', '-fno-sanitize-recover=all',
                         '-I', str(Path(config.root) / 'include'), str(generated),
                         *extra, '-L', config.lib_dir, '-lforge_std',
                         '-lforge_runtime', '-pthread', '-lm', '-o', str(binary)],
                        capture_output=True, text=True, timeout=30)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    result = subprocess.run([str(binary)], capture_output=True,
                                            text=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout, expected)
                    self.assertEqual(result.stderr, '')
            if javascript:
                script = work / 'main.js'
                emitted = subprocess.run(
                    [config.forge, str(fg), '--emit-js', '-o', str(script)],
                    capture_output=True, text=True, timeout=20)
                self.assertEqual(emitted.returncode, 0, emitted.stderr)
                result = subprocess.run([config.node, str(script)], capture_output=True,
                                        text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, expected)

    def test_float_printing_and_conversion_boundaries(self) -> None:
        self.execute('''import math;
fn half(x:float):float{return x/2;}
fn five():float{return 5;}
native main{
 println(1.5);println(-2.5);println(1.5+2);
 let x:float=5;println(x/2);x=7;println(x/2);
 println(half(5));println(five()/2);println(abs_f(-5));
 println("mixed:",1.5,":",true,":",9);
 return 0;
}''', '1.5\n-2.5\n3.5\n2.5\n3.5\n2.5\n2.5\n5\nmixed:1.5:1:9\n')

    def test_print_arguments_evaluated_once_in_source_order(self) -> None:
        self.execute('''fn first():float{println("first");return 1.5;}
fn second():float{println("second");return 2.5;}
native main{println(first(),second());return 0;}''',
                     'first\n1.5second\n2.5\n', javascript=False)

    def test_float_module_result_printing(self) -> None:
        self.execute('''import sample;
native main{println(sample.half(5));return 0;}''', '2.5\n',
                     modules={'sample.fg': 'fn half(x:float):float{return x/2;}'})

    def test_coroutine_float_survives_suspension(self) -> None:
        self.execute('''process main{
 coroutine task(){let x:float=1.5;yield;println(x);}
 spawn task();
}''', '1.5\n', javascript=False)

    def test_opaque_float_results_use_actual_c_type(self) -> None:
        self.execute('''extern fn read_value():float;
native main{println(read_value());return 0;}''', '1.5\n', javascript=False,
                     bridge='double read_value(void){return 1.5;}\n')

    def test_native_float_print_preserves_precision_and_large_magnitudes(self) -> None:
        self.execute('''native main{
 println(1.2345678901234567);println(18446744073709551616.0);return 0;
}''', '1.2345678901234567\n1.8446744073709552e+19\n', javascript=False)


if __name__ == '__main__':
    unittest.main()
