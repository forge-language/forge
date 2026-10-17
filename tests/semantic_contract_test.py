"""Exercise SDK signatures and lexical declaration checks through the CLI."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--forge', required=True)
parser.add_argument('--root')
parser.add_argument('--lib-dir')
parser.add_argument('--cc', default='cc')
config, remaining = parser.parse_known_args()
sys.argv = [sys.argv[0], *remaining]


class SemanticContractTests(unittest.TestCase):
    def check(self, source: str, message: str | None = None, *,
              target: str | None = None, binary: bool = False) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'contract.fg'
            output = Path(directory) / 'output.c'
            path.write_text(source)
            output.write_text('preserve existing output')
            extra = []
            if binary:
                (Path(directory) / 'libforge_opaque.a').write_bytes(b'!<arch>\n')
                extra = ['--lib-dir', directory]
            result = subprocess.run(
                [config.forge, str(path), '--check', '--diagnostics-json', '-o', str(output), *extra],
                capture_output=True, text=True, timeout=20)
            self.assertEqual(output.read_text(), 'preserve existing output')
            if message is None:
                self.assertEqual(result.returncode, 0, result.stderr)
            else:
                self.assertNotEqual(result.returncode, 0, source)
                diagnostic = json.loads(result.stderr)
                self.assertEqual(diagnostic['phase'], 'semantic')
                self.assertEqual(diagnostic['file'], str(path))
                self.assertIn(message, diagnostic['message'])
                self.assertGreater(diagnostic['range']['end']['character'],
                                   diagnostic['range']['start']['character'])
                if target is not None:
                    offset = source.rindex(target)
                    self.assertEqual(diagnostic['range'], {
                        'start': {'line': 0, 'character': offset},
                        'end': {'line': 0, 'character': offset + len(target)}})

    def test_sdk_rejects_missing_extra_and_wrong_typed_arguments(self) -> None:
        for expression in ('str_len()', 'str_len("x", "y")'):
            with self.subTest(expression=expression):
                self.check(f'import strings; native main{{{expression};return 0;}}',
                           'wrong argument count')
        for module, expression in (('strings', 'str_len(12)'),
                                   ('math', 'abs_i("x")'),
                                   ('http', 'http_respond(1,200,4)'),
                                   ('gpu', 'gpu_free("x")')):
            with self.subTest(module=module):
                self.check(f'import {module}; native main{{{expression};return 0;}}',
                           'type mismatch')

    def test_sdk_return_types_are_checked(self) -> None:
        for module, expression in (('strings', 'str_len("x")'),
                                   ('time', 'sleep_ms(0)'),
                                   ('gpu', 'gpu_available()')):
            with self.subTest(module=module):
                self.check(f'import {module}; fn bad():string{{return {expression};}}'
                           'native main{return 0;}', 'type mismatch')
        self.check('import strings; fn good():string{return str_concat("a","b");}'
                   'native main{let v:int=str_view("x");return str_view_len(v);}')

    def test_duplicate_locals_parameters_and_parameter_redeclaration(self) -> None:
        for source in ('native main{let x:int=1;let x:int=2;return 0;}',
                       'fn bad(x:int,x:int):int{return 0;}native main{return 0;}',
                       'fn bad(x:int):int{let x:int=2;return 0;}native main{return 0;}',
                       'extern fn bad(x:int,x:int):int;native main{return 0;}',
                       'process main{coroutine bad(x:int,x:int){return;} }'):
            with self.subTest(source=source):
                self.check(source, 'duplicate declaration', target='x')

    def test_nested_shadowing_and_for_initializer_scope(self) -> None:
        self.check('fn good(x:int):int{{let x:int=2;println(x);}return x;}'
                   'native main{let x:int=0;for(let x:int=0;x<1;x=x+1)'
                   '{let x:int=2;println(x);}return good(x);}')

    def test_opaque_extern_callback_forwarding_is_preserved(self) -> None:
        self.check('extern fn consume(cb:int):int;'
                   'fn bridge(cb:int):int{return consume(cb);}'
                   'fn callback(x:int):int{return x;}'
                   'native main{return bridge(callback);}')

    def test_untyped_binary_api_boundary_is_preserved(self) -> None:
        self.check('import opaque;fn callback(x:int):int{return x;}'
                   'native main{let n:int=opaque.external("x",1,callback);return n;}',
                   binary=True)

    def test_signature_error_points_to_argument_or_callee(self) -> None:
        self.check('import strings;native main{str_len("a",1);return 0;}',
                   'wrong argument count', target='str_len')
        self.check('import strings;native main{str_sub("a",0,"bad");return 0;}',
                   'type mismatch', target='"bad"')

    def test_semantic_rejection_precedes_backend_output_creation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / 'invalid.fg'
            output = work / 'output'
            source.write_text('import strings;native main{str_len();return 0;}')
            for mode in ('--check', '--emit-c', '--emit-js'):
                with self.subTest(mode=mode):
                    output.write_text('keep this output')
                    result = subprocess.run([config.forge, str(source), mode, '-o', str(output)],
                                            capture_output=True, text=True, timeout=20)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('wrong argument count', result.stderr)
                    self.assertEqual(output.read_text(), 'keep this output')

    def test_indexed_thread_spawn_has_void_result(self) -> None:
        self.check('import thread;fn worker(id:int,total:int):int{return 0;}'
                   'native main{let result:int=thread_spawn_indexed(worker,1);return 0;}',
                   'type mismatch')

    def test_every_sdk_mapping_has_an_authoritative_checked_signature(self) -> None:
        registry = (Path(__file__).resolve().parents[1] / 'compiler/mod_registry.c').read_text()
        sdk = Path(config.root) if config.root else Path(config.forge).resolve().parents[1]
        headers = '\n'.join(path.read_text() for path in (sdk / 'include').rglob('*.h'))
        headers = re.sub(r'/\*.*?\*/', '', headers, flags=re.DOTALL)
        rows = re.findall(r'\{"(\w+)", "(\w+)", "([visfp]+)"\}', registry)
        self.assertEqual(len(rows), len(re.findall(r'\{"\w+", "\w+"', registry)))
        self.assertGreater(len(rows), 150)

        def kind(declaration: str) -> str:
            if 'char' in declaration and '*' in declaration:
                return 's'
            if '*' in declaration:
                return 'p'
            if 'double' in declaration:
                return 'f'
            if declaration.strip() == 'void':
                return 'v'
            return 'i'

        modules = dict(re.findall(
            r'\.name = \{ "(\w+)", \d+ \}.*?\.fns = (\w+)', registry))
        for module, array in modules.items():
            array_body = registry.split(f'{array}[] = {{', 1)[1].split('};', 1)[0]
            for name, symbol, signature in rows:
                if f'"{name}"' not in array_body:
                    continue
                with self.subTest(function=name):
                    declaration = re.search(
                        r'(?:^|\n)\s*((?:const\s+)?(?:void|char|double|int64_t|int|fr_scheduler_t)'
                        r'\s*\*?)\s*' + symbol + r'\s*\(([^;{}]*)\)\s*;', headers)
                    self.assertIsNotNone(declaration, symbol)
                    if declaration is None:
                        continue
                    arguments = [] if declaration[2].strip() == 'void' else declaration[2].split(',')
                    self.assertEqual(signature, kind(declaration[1]) + ''.join(map(kind, arguments)))
                    values = {'i': '1', 'f': '1.0', 's': '"x"', 'p': 'pointer()'}
                    args = [values[code] for code in signature[1:]]
                    prefix = f'import {module}; extern fn pointer():ptr; native main{{'
                    self.check(prefix + f'{name}({",".join(args)});return 0;}}')
                    self.check(prefix + f'{name}({",".join([*args,"1"])});return 0;}}',
                               'wrong argument count')
                    for index, code in enumerate(signature[1:]):
                        invalid = args.copy()
                        invalid[index] = '1' if code == 's' else '"wrong"'
                        self.check(prefix + f'{name}({",".join(invalid)});return 0;}}',
                                   'type mismatch')


if __name__ == '__main__':
    unittest.main()
