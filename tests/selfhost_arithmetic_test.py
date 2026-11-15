"""Run emitted stage0/stage2 integer programs at O0/O3 under UBSan."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
for option in ('forge', 'stage0', 'root', 'lib-dir', 'cc'):
    parser.add_argument('--' + option, required=True)
parser.add_argument('--source-root')
config, remaining = parser.parse_known_args()
sys.argv = [sys.argv[0], *remaining]


class SelfhostArithmeticTests(unittest.TestCase):
    def execute(self, source: str, expected: str, *, trap: bool = False) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / 'integer.fg'
            fixture.write_text(source)
            for compiler in (config.stage0, config.forge):
                generated = root / 'integer.c'
                result = subprocess.run([compiler, str(fixture), '--emit-c', '-o', str(generated)],
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                for optimization in ('-O0', '-O3'):
                    with self.subTest(compiler=compiler, optimization=optimization):
                        binary = root / 'integer'
                        result = subprocess.run(
                            [config.cc, '-std=c11', optimization, '-Werror',
                             '-fsanitize=undefined', '-fno-sanitize-recover=all',
                             '-I', str(Path(config.root) / 'include'), str(generated),
                             '-L', config.lib_dir, '-lforge_std', '-lforge_runtime',
                             '-pthread', '-lm', '-o', str(binary)],
                            capture_output=True, text=True, timeout=30)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        result = subprocess.run([str(binary)], capture_output=True,
                                                text=True, timeout=10)
                        self.assertEqual(result.stdout, expected)
                        if trap:
                            self.assertNotEqual(result.returncode, 0)
                            self.assertIn('Forge integer division', result.stderr)
                            self.assertNotIn('runtime error:', result.stderr)
                        else:
                            self.assertEqual(result.returncode, 0, result.stderr)
                            self.assertEqual(result.stderr, '')

    def test_wrapping_precedence_compound_updates_and_short_circuit(self) -> None:
        self.execute('''fn add(a:int,b:int):int{return a+b;}
fn mul(a:int,b:int):int{return a*b;}
fn fail():int{return 1/0;}
native main{
 println(add(9223372036854775807,1));
 println(mul(9223372036854775807,2));
 let x:int=-9223372036854775807-1;println(-x);println(x-1);
 println(-7/3);println(-7%3);println(20-3*4-1);
 x=9223372036854775807;x+=1;println(x);x-=1;println(x);
 x*=2;println(x);println(false && fail());println(true || fail());
 for(let i:int=0;i<3;i+=1){if(i==1){continue;}println(i);}
 return 0;
}''', '-9223372036854775808\n-2\n-9223372036854775808\n9223372036854775807\n'
                     '-2\n-1\n7\n-9223372036854775808\n9223372036854775807\n-2\n0\n1\n0\n2\n')

    def test_integer_traps_preserve_printed_prefix(self) -> None:
        for expression in ('x/y', 'x%y'):
            for left, right in (('1', '0'), ('(-9223372036854775807-1)', '-1')):
                with self.subTest(expression=expression, left=left, right=right):
                    self.execute('fn bad(x:int,y:int):int{return ' + expression + ';}\n'
                                 'native main{println("prefix");println(bad(' + left + ',' + right + '));return 0;}',
                                 'prefix\n', trap=True)


if __name__ == '__main__':
    unittest.main()
