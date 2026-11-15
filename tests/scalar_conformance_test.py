"""Execute scalar conversion and printing through both stage0 backends."""
import argparse
import math
import operator
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
                         '-fsanitize=undefined,float-cast-overflow', '-fno-sanitize-recover=all',
                         '-I', str(Path(config.root) / 'include'), str(generated),
                         *extra, '-L', config.lib_dir, '-lforge_std',
                         '-lforge_runtime', '-pthread', '-lm', '-o', str(binary)],
                        capture_output=True, text=True, timeout=30)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    result = subprocess.run([str(binary)], capture_output=True,
                                            text=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout.encode(), expected.encode())
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
                self.assertEqual(result.stdout.encode(), expected.encode())

    def test_mixed_comparisons_preserve_integer_precision(self) -> None:
        comparisons = (('==', operator.eq), ('!=', operator.ne),
                       ('<', operator.lt), ('<=', operator.le),
                       ('>', operator.gt), ('>=', operator.ge))
        body = ''.join(f'println(i{op}f);println(f{op}i);' for op, _ in comparisons)
        source = ('fn ratio(a:float,b:float):float{return a/b;}'
                  'fn compare(i:int,f:float){' + body + '}native main{')
        expected: list[str] = []
        cases: list[tuple[int, float]] = []
        for integer in (-(1 << 63), -(1 << 63)+1, (1 << 63)-2, (1 << 63)-1,
                        -(1 << 53)-1, -(1 << 53), -(1 << 53)+1,
                        (1 << 53)-1, 1 << 53, (1 << 53)+1):
            rounded = float(integer)
            cases.extend((integer, value) for value in (
                math.nextafter(rounded, -math.inf), rounded,
                math.nextafter(rounded, math.inf)))
        for integer in (-1, 0, 1, -(1 << 63), (1 << 63)-1):
            cases.extend((integer, value) for value in (
                -0.0, 0.0, -math.ulp(0.0), math.ulp(0.0), -0.5, 0.5,
                -1.5, 1.5, -math.inf, math.inf, math.nan))
        for integer, value in cases:
            int_text = '(-9223372036854775807-1)' if integer == -(1 << 63) else str(integer)
            float_text = format(value, '.1f')
            if math.isnan(value):
                float_text = 'ratio(0.0,0.0)'
            elif math.isinf(value):
                float_text = 'ratio(-1.0,0.0)' if value < 0 else 'ratio(1.0,0.0)'
            elif abs(value) == math.ulp(0.0):
                float_text = ('-' if value < 0 else '') + (
                    f'ratio(ratio(1.0,{1 << 1022}.0),4503599627370496.0)')
            source += f'compare({int_text},{float_text});'
            for _, compare in comparisons:
                expected.extend((str(int(compare(integer, value))),
                                 str(int(compare(value, integer)))))
        source += 'println(9007199254740993==9007199254740992.0);'
        source += 'println(9007199254740993>9007199254740992.0);return 0;}'
        self.execute(source, '\n'.join([*expected, '0', '1'])+'\n')

    def test_mixed_comparisons_evaluate_once_and_short_circuit(self) -> None:
        self.execute('''fn integer():int{println("integer");return 9007199254740993;}
fn floating():float{println("float");return 9007199254740992.0;}
fn fail():int{return 1/0;}
native main{
 let a:bool=integer()>9007199254740992.0;println(a);
 let b:bool=9007199254740993>floating();println(b);
 println(false && (fail()>0.5));println(true || (fail()==0.5));
 println(true==1.0);println(false<0.5);return 0;
}''', 'integer\n1\nfloat\n1\n0\n1\n1\n1\n')

    def test_mixed_module_and_suspended_coroutine_comparisons(self) -> None:
        module = 'fn greater(i:int,f:float):bool{return i>f;}'
        self.execute('import sample;native main{println(sample.greater('
                     '9007199254740993,9007199254740992.0));return 0;}', '1\n',
                     modules={'sample.fg': module})
        self.execute('''process main{coroutine task(){
 let i:int=9007199254740993;let f:float=9007199254740992.0;
 yield;println(i==f);println(i>f);}
 spawn task();}''', '0\n1\n', javascript=False)

    def test_mixed_comparisons_preserve_opaque_c_domains(self) -> None:
        self.execute('''import opaque;
native main{
 println(opaque.integer()==opaque.floating());
 println(opaque.integer()>opaque.floating());
 println(opaque.pointer()==opaque.pointer());
 println(opaque.pointer()==opaque.other_pointer());
 println(opaque.pointer()!=opaque.other_pointer());
 println(opaque.unsigned_value()>0.0);println(opaque.unsigned_value()>0);
 println(opaque.wide_float()>0);println(opaque.single_float()>0);return 0;
}''', '0\n1\n1\n0\n1\n1\n1\n1\n1\n', javascript=False, modules={
            'opaque.h': '''#include <stdint.h>
static inline int64_t frlib_opaque_integer(void){return INT64_C(9007199254740993);}
static inline double frlib_opaque_floating(void){return 9007199254740992.0;}
static inline void *frlib_opaque_pointer(void){static int value;return &value;}
static inline void *frlib_opaque_other_pointer(void){static int other;return &other;}
static inline uint64_t frlib_opaque_unsigned_value(void){return UINT64_MAX;}
static inline long double frlib_opaque_wide_float(void){return 0.5L;}
static inline float frlib_opaque_single_float(void){return 0.5f;}
'''})

    def test_nested_mixed_comparisons(self) -> None:
        expression = 'i>f'
        for _ in range(16):
            expression = f'({expression})>0.0'
        self.execute('native main{let i:int=9007199254740993;'
                     'let f:float=9007199254740992.0;'
                     f'println({expression});return 0;}}', '1\n')

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
                     'first\n1.5second\n2.5\n')

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
}''', '1.2345678901234567\n18446744073709552000\n')

    def test_shortest_float_text_and_special_values(self) -> None:
        self.execute('''fn ratio(a:float,b:float):float{return a/b;}
native main{println(0.1);println(ratio(0.0,-1.0));
println(ratio(1.0,0.0));println(ratio(-1.0,0.0));println(ratio(0.0,0.0));
println(ratio(1.0,1000000.0));println(ratio(1.0,10000000.0));return 0;}''',
                     '0.1\n-0\nInfinity\n-Infinity\nNaN\n0.000001\n1e-7\n')


if __name__ == '__main__':
    unittest.main()
