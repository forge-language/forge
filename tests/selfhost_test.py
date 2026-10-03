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

    def test_existing_process_control_and_match_examples(self):
        expected = {'match': 'OK\nlucky seven\n',
                    'control_flow': '2\n0\n1\n2\n0\n1\n2\n1\n2\n3\n4\n6\n7\n'}
        for name, stdout in expected.items():
            with self.subTest(example=name):
                source = Path(config.root) / 'examples' / (name + '.fg')
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
