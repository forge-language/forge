import pathlib
import subprocess
import sys
import tempfile
import unittest

FORGE = pathlib.Path(sys.argv.pop(1)).resolve()


class ModuleMergeTest(unittest.TestCase):
    def test_many_declarations_and_mixed_forward_functions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            parts = []
            for i in range(600):
                parts += [f'extern fn external_{i}(value: int): int;',
                          f'fn value_{i}(): int {{ return {i}; }}',
                          f'const CONST_{i} = {i};']
            parts += ['struct Item { value: int; }',
                      'enum Choice { First; Second; }',
                      'fn first(): int { return last() + value_0() + value_123() + CONST_123; }',
                      'fn last(): int { return value_599() + CONST_599; }']
            (root / 'large.fg').write_text('\n'.join(parts))
            (root / 'main.fg').write_text('import large; native main { println(large.first()); return 0; }')
            result = subprocess.run([str(FORGE), str(root / 'main.fg'),
                                     '-o', str(root / 'main'), '--forge-root',
                                     str(pathlib.Path(__file__).resolve().parents[1]),
                                     '--lib-dir', str(FORGE.parent.parent / 'lib')],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(root / 'main')], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, '1444\n')


if __name__ == '__main__':
    unittest.main()
