"""Verify the native compiler's default output and relocatable install layout."""
import argparse
import os
import platform
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--forge', type=Path, required=True)
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--lib-dir', type=Path, required=True)
parser.add_argument('--cc', required=True)
config, remaining = parser.parse_known_args()
sys.argv = [sys.argv[0], *remaining]


class DriverTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='forge driver ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'hello world.fg'
        self.source.write_text('native main { println("native"); return 0; }')

    def compile(self, *extra, env=None):
        return subprocess.run([str(config.forge.resolve()), str(self.source), '--cc', config.cc,
                               '--forge-root', str(config.root.resolve()),
                               '--lib-dir', str(config.lib_dir.resolve()), *extra],
                              cwd=self.root, env=env, capture_output=True, text=True, timeout=30)

    def test_default_basename_in_working_directory(self):
        result = self.compile()
        self.assertEqual(result.returncode, 0, result.stderr)
        binary = self.root / ('hello world.exe' if os.name == 'nt' else 'hello world')
        self.assertEqual(subprocess.check_output([str(binary)], text=True), 'native\n')
        self.assertIn('native main', self.source.read_text())

    def test_explicit_root_overrides_environment(self):
        result = self.compile(env={**os.environ, 'FORGE_ROOT': str(self.root / 'missing')})
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_native_thread_module_resolves_runtime_archive(self):
        self.source.write_text('import thread; native main { println(thread_cpu_count()); return 0; }')
        # The stdlib thread object calls back into runtime. Also rebuild an
        # existing output to ensure its identity differs from the input file.
        for _ in range(2):
            result = self.compile()
            self.assertEqual(result.returncode, 0, result.stderr)
        binary = self.root / ('hello world.exe' if os.name == 'nt' else 'hello world')
        self.assertGreater(int(subprocess.check_output([str(binary)], text=True)), 0)

    @unittest.skipUnless(sys.platform.startswith('linux') and platform.machine().lower() in
                         ('x86_64', 'amd64'), 'Forge native ELF prototype targets x86_64 Linux')
    def test_forge_native_target_emits_without_a_c_compiler(self):
        cases = [
            ('native main { let answer: int = 6 * 7; return answer; }', 42),
            ('native main { return (100 - 16) / 2; }', 42),
            ('native main { return 89 % 47; }', 42),
            ('native main { let n: int = 8; return n == 8; }', 1),
            ('native main { let n: int = 40; if (n < 42) { return 42; } else { return 1; } }', 42),
            ('native main { let mut n: int = 0; while (n < 42) { n = n + 1; } return n; }', 42),
        ]
        for index, (source, expected) in enumerate(cases):
            with self.subTest(source=source):
                self.source.write_text(source)
                binary = self.root / f'native-answer-{index}'
                result = self.compile('--target', 'x86_64-unknown-linux-gnu', '--cc',
                                      str(self.root / 'missing-cc'), '-o', str(binary))
                self.assertEqual(result.returncode, 0, result.stderr)
                run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
                self.assertEqual(run.returncode, expected)

    def test_input_alias_cannot_be_overwritten(self):
        original = self.source.read_bytes()
        alias = self.root / 'alias.c'
        os.link(self.source, alias)
        for path in [self.source, alias]:
            result = self.compile('--emit-c', '-o', str(path))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('overwrite input', result.stderr)
            self.assertEqual(self.source.read_bytes(), original)

    def test_installed_layout_resolves_from_path_without_env(self):
        prefix = self.root / 'installed'
        (prefix / 'bin').mkdir(parents=True)
        (prefix / 'lib').mkdir()
        shutil.copytree(config.root / 'include', prefix / 'include')
        name = 'forge.exe' if os.name == 'nt' else 'forge'
        shutil.copy2(config.forge, prefix / 'bin' / name)
        for archive in ['libforge_runtime.a', 'libforge_std.a']:
            shutil.copy2(config.lib_dir / archive, prefix / 'lib' / archive)
        metadata = config.lib_dir / 'forge.link'
        if metadata.exists():
            shutil.copy2(metadata, prefix / 'lib' / metadata.name)
        env = {**os.environ, 'PATH': str(prefix / 'bin') + os.pathsep + os.environ['PATH']}
        env.pop('FORGE_ROOT', None)
        result = subprocess.run([name, str(self.source), '--cc', config.cc], cwd=self.root,
                                env=env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        binary = self.root / ('hello world.exe' if os.name == 'nt' else 'hello world')
        self.assertEqual(subprocess.check_output([str(binary)], text=True), 'native\n')


if __name__ == '__main__':
    unittest.main()
