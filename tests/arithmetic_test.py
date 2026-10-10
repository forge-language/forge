"""Compare int64 arithmetic to a Python oracle, with UBSan and JavaScript."""
import argparse
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
for name in ('forge', 'root', 'lib-dir', 'cc'):
    parser.add_argument('--' + name, required=True)
parser.add_argument('--node', default='node')
config, remaining = parser.parse_known_args()
sys.argv = [sys.argv[0], *remaining]
MIN, MAX = -(1 << 63), (1 << 63) - 1


def literal(value):
    return '(-9223372036854775807 - 1)' if value == MIN else str(value)


def wrap(value):
    return (value + (1 << 63)) % (1 << 64) - (1 << 63)


class ArithmeticTests(unittest.TestCase):
    def execute(self, source, expected='', failure=False, bridge='', js_bridge='', js=True,
                library_bridge=None):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            fg, c, binary = [work / name for name in ('test.fg', 'test.c', 'test')]
            fg.write_text(source)
            args = [config.forge, str(fg), '--emit-c', '-o', str(c)]
            if library_bridge is not None:
                args += ['--lib', '--header', str(work / 'arithmetic.h')]
            result = subprocess.run(args, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            extras = []
            if bridge or library_bridge is not None:
                extra = work / 'bridge.c'
                extra.write_text(bridge + (library_bridge or ''))
                extras.append(str(extra))
            for opt in ('-O0', '-O3'):
                with self.subTest(optimization=opt):
                    result = subprocess.run([
                        config.cc, '-std=gnu11', opt, '-g', '-fsanitize=undefined',
                        '-fno-sanitize-recover=all', '-Werror',
                        '-I', str(Path(config.root) / 'include'), str(c), *extras,
                        '-L', config.lib_dir, '-lforge_std', '-lforge_runtime',
                        '-lpthread', '-lm', '-o', str(binary)
                    ], capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
                    if failure:
                        self.assertEqual(result.returncode, 1, result.stderr)
                        self.assertEqual(result.stderr, 'Forge integer division\n')
                    else:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(result.stdout, expected)
                        self.assertEqual(result.stderr, '')
            if js:
                script = work / 'test.js'
                result = subprocess.run([config.forge, str(fg), '--emit-js', '-o', str(script)],
                                        capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr)
                script.write_text(js_bridge + '\n' + script.read_text())
                result = subprocess.run([config.node, str(script)], capture_output=True, text=True, timeout=20)
                if failure:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('Forge integer division', result.stderr)
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout, expected)

    def test_modular_constants_and_dynamic_oracle(self):
        rng = random.Random(20261009)
        pairs = [(MAX, 1), (MIN, 1), (MIN, -1), (MAX, MAX), (MIN, MIN), (0, MIN)]
        pairs += [(rng.randrange(MIN, MAX + 1), rng.randrange(MIN, MAX + 1)) for _ in range(32)]
        source = 'fn add(a:int,b:int):int{return a+b;} fn sub(a:int,b:int):int{return a-b;} fn mul(a:int,b:int):int{return a*b;} native main {'
        expected = []
        for a, b in pairs:
            for name, op, value in [('add', '+', a+b), ('sub', '-', a-b), ('mul', '*', a*b)]:
                # Identical mathematical result through both folding and dynamic parameters.
                source += f'println({literal(a)} {op} {literal(b)});println({name}({literal(a)},{literal(b)}));'
                expected += [str(wrap(value))] * 2
        source += 'return 0;}'
        self.execute(source, '\n'.join(expected) + '\n')

    def test_signed_division_and_remainder(self):
        source = 'fn quotient(a:int,b:int):int{return a/b;} fn mod(a:int,b:int):int{return a%b;} native main {'
        expected = []
        for a, b in [(MIN, 1), (MAX, -1), (-7, 3), (7, -3), (-7, -3), (0, 7)]:
            quotient = abs(a) // abs(b) * (-1 if (a < 0) != (b < 0) else 1)
            for name, op, value in [('quotient', '/', quotient), ('mod', '%', a - quotient*b)]:
                source += f'println({literal(a)}{op}{literal(b)});println({name}({literal(a)},{literal(b)}));'
                expected += [str(value)] * 2
        self.execute(source + 'return 0;}', '\n'.join(expected) + '\n')

    def test_invalid_division_and_remainder(self):
        for op in ('/', '%'):
            for a, b in [(1, 0), (MIN, -1)]:
                for dynamic in (False, True):
                    with self.subTest(op=op, a=a, b=b, dynamic=dynamic):
                        expr = f'calculate({literal(a)},{literal(b)})' if dynamic else f'{literal(a)}{op}{literal(b)}'
                        source = f'fn calculate(a:int,b:int):int{{return a{op}b;}} native main{{println({expr});return 0;}}'
                        self.execute(source, failure=True)

    def test_float_promotion_both_operands_and_once_evaluation(self):
        source = '''extern fn tick(): int; extern fn ticks(): int; extern fn half(): float; extern fn minus_zero(): float;
fn floating(x:float):float{return x;} native main {
if (floating(1.5)+2 != 3.5) {return 1;} if (2+floating(1.5) != 3.5) {return 2;}
if (floating(1.5)-2 != -0.5) {return 3;} if (2-floating(1.5) != 0.5) {return 4;}
if (floating(1.5)*2 != 3.0) {return 5;} if (2*floating(1.5) != 3.0) {return 6;}
if (floating(1.5)/2 != 0.75) {return 7;} if (2/floating(0.5) != 4.0) {return 8;}
if (half()+1 != 1.5) {return 9;} if (1+half() != 1.5) {return 10;}
if (!(1.0/floating(0.0) > 0.0)) {return 11;}
if (!(1.0/(minus_zero()*2) < 0.0)) {return 12;}
println(tick()+tick()); println(ticks()); println(tick()*tick()); println(ticks());
return 0;}'''
        bridge = '#include <stdint.h>\nstatic int64_t count; int64_t tick(void){count++;return 2;} int64_t ticks(void){return count;} double half(void){return 0.5;} double minus_zero(void){return -0.0;}\n'
        js_bridge = 'let count=0n; globalThis.ForgeNative={tick:()=>{count++;return 2n;},ticks:()=>count,half:()=>0.5,minus_zero:()=>-0.0};'
        self.execute(source, '4\n2\n4\n4\n', bridge=bridge, js_bridge=js_bridge)

    def test_unary_compound_loop_and_min_constant(self):
        self.execute('''const low = -9223372036854775807 - 1;
native main {let x:int=9223372036854775807;x+=1;println(x);println(-x);
let i:int=0;while(i<3){x-=1;i+=1;}println(x);x*=2;println(x);println(low);return 0;}''',
                     f'{MIN}\n{MIN}\n{MAX-2}\n-6\n{MIN}\n')

    def test_bool_promotion_and_short_circuit(self):
        self.execute('fn invalid():int{return 1/0;} native main {let yes:bool=true;let no:bool=false;println(yes+yes);println(5-no);println(yes*7);if(no && invalid()){return 1;}if(yes || invalid()){println(1);}return 0;}', '2\n5\n7\n1\n')

    def test_suspended_coroutine_arithmetic(self):
        self.execute('process main {coroutine work(){let x:int=9223372036854775807;yield;x+=1;println(x);x=x*-1;println(x);yield;println(x/2);}spawn work();}', f'{MIN}\n{MIN}\n{MIN//2}\n', js=False)

    def test_library_boundary(self):
        source = '''library arithmetic {export fn add(a:int,b:int):int{return a+b;}
export fn mixed(a:int,b:float):float{return a+b;} export fn div(a:int,b:int):int{return a/b;}}'''
        bridge = '''#include "arithmetic.h"
#include <stdint.h>
#include <stdio.h>
int main(void){if(frlib_arithmetic_add(INT64_MAX,1)!=INT64_MIN)return 1;
if(frlib_arithmetic_mixed(1,0.5)!=1.5)return 2;if(frlib_arithmetic_div(-7,3)!=-2)return 3;return 0;}'''
        self.execute(source, js=False, library_bridge=bridge)


if __name__ == '__main__':
    unittest.main()
