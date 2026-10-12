import pathlib
import subprocess
import sys
import tempfile
import unittest

FORGE = pathlib.Path(sys.argv.pop(1)).resolve()


class ModuleMergeTest(unittest.TestCase):
    def test_failed_imported_read_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / 'unreadable.fg').mkdir()
            source = root / 'main.fg'
            source.write_text('import "unreadable.fg"; native main {}')
            result = subprocess.run([str(FORGE), str(source), '--check'],
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn('cannot read module', result.stderr)
            self.assertNotIn('out of memory', result.stderr)

    def test_deep_import_chain_is_rejected_before_stack_exhaustion(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for index in range(66):
                (root / f'm{index}.fg').write_text(f'import m{index+1};' if index < 65 else '')
            source = root / 'main.fg'
            source.write_text('import m0; native main {}')
            result = subprocess.run([str(FORGE), str(source), '--check'],
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn('module import nesting too deep', result.stderr)

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
                                     str(FORGE.parent.parent),
                                     '--lib-dir', str(FORGE.parent.parent / 'lib')],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(root / 'main')], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, '1444\n')


if __name__ == '__main__':
    unittest.main()
