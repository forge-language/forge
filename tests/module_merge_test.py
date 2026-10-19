import json
import pathlib
import shutil
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


class NestedQuotedImportTest(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name)
        self.source = self.root / 'main.fg'
        self.source.write_text('import "parent.fg"; native main { '
                               'println(parent.wrapper()); println(leaf.value()); return 0; }')
        (self.root / 'parent.fg').write_text('import "leaf.fg"; '
                                            'fn wrapper(): int { return leaf.value() + 1; }')
        (self.root / 'leaf.fg').write_text('fn value(): int { return 41; }')

    def test_qualified_calls_pass_check_after_parent_cleanup(self) -> None:
        result = subprocess.run([str(FORGE), str(self.source), '--check'],
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_symbol_containers_survive_parent_cleanup(self) -> None:
        result = subprocess.run([str(FORGE), str(self.source), '--symbols-json'],
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        symbols = json.loads(result.stdout)
        self.assertIn({'kind': 'function', 'name': 'value', 'container': 'leaf'}, symbols)
        self.assertIn({'kind': 'function', 'name': 'wrapper', 'container': 'parent'}, symbols)

    def test_direct_imports_share_one_module(self) -> None:
        for imports in ('import leaf;', 'import "leaf.fg";',
                        'import leaf; import "leaf.fg"; import leaf;'):
            with self.subTest(imports=imports):
                self.source.write_text(imports + ' import "parent.fg"; native main { '
                                       'println(parent.wrapper()); println(leaf.value()); return 0; }')
                result = subprocess.run([str(FORGE), str(self.source), '--symbols-json'],
                                        capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr)
                symbols = json.loads(result.stdout)
                self.assertEqual(symbols.count({'kind': 'function', 'name': 'value',
                                                'container': 'leaf'}), 1)
                self.assertEqual(symbols.count({'kind': 'function', 'name': 'wrapper',
                                                'container': 'parent'}), 1)

    def test_native_calls_survive_parent_cleanup(self) -> None:
        output = self.root / 'main'
        result = subprocess.run([str(FORGE), str(self.source), '-o', str(output),
                                 '--forge-root', str(FORGE.parent.parent),
                                 '--lib-dir', str(FORGE.parent.parent / 'lib')],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run([str(output)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, '42\n41\n')

    @unittest.skipUnless(shutil.which('node'), 'Node is required for JavaScript execution')
    def test_javascript_calls_survive_parent_cleanup(self) -> None:
        output = self.root / 'main.js'
        result = subprocess.run([str(FORGE), str(self.source), '--emit-js', '-o', str(output)],
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run(['node', str(output)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, '42\n41\n')


if __name__ == '__main__':
    unittest.main()
