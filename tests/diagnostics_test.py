"""Check exact compiler-owned source ranges, including imported and Unicode text."""
import argparse
import json
from pathlib import Path
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


def position(text, offset):
    prefix = text[:offset]
    return {'line': prefix.count('\n'),
            'character': len(prefix.rsplit('\n', 1)[-1].encode('utf-16-le')) // 2}


class DiagnosticsTests(unittest.TestCase):
    def check(self, text, target, *, occurrence=0, files=None, origin=None,
              entry='entry.fg', phase='semantic', message=None):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            path = work / entry
            path.write_bytes(text.encode())
            for name, content in (files or {}).items():
                (work / name).write_bytes(content.encode())
            result = subprocess.run([config.forge, str(path), '--check', '--diagnostics-json'],
                                    capture_output=True, text=True, timeout=20)
            self.assertNotEqual(result.returncode, 0)
            lines = result.stderr.splitlines()
            self.assertEqual(len(lines), 1, result.stderr)
            diagnostic = json.loads(lines[0])
            self.assertEqual(diagnostic['phase'], phase)
            self.assertEqual(diagnostic['severity'], 'error')
            self.assertEqual(diagnostic['source'], 'forge')
            expected_text = (files or {})[origin] if origin else text
            index = -1
            for _ in range(occurrence + 1):
                index = expected_text.index(target, index + 1)
            self.assertEqual(diagnostic['file'], str(work / (origin or entry)))
            self.assertEqual(diagnostic['range'], {'start': position(expected_text, index),
                                                   'end': position(expected_text, index + len(target))})
            if message:
                self.assertIn(message, diagnostic['message'])
            # Existing callers keep their original first diagnostic line.
            legacy = subprocess.run([config.forge, str(path), '--check'],
                                    capture_output=True, text=True, timeout=20)
            prefix = 'forge: parse error at ' if phase == 'parse' else 'forge: semantic: '
            self.assertTrue(legacy.stderr.startswith(prefix), legacy.stderr)
            self.assertIn('forge: location: ' + str(work / (origin or entry)), legacy.stderr)
            return diagnostic

    def test_repeated_identifier_not_comment_or_string_or_parameter(self):
        text = '''// missing is mentioned here
const NOTE = "missing";
fn valid(missing:int):int{return missing;}
native main {
    println(missing);
    return 0;
}
'''
        self.check(text, 'missing', occurrence=4, message='unknown value')

    def test_initializer_multiline_raw_string(self):
        self.check('native main {\n let value:int = "first\\n\nsecond";\n return 0;\n}',
                   '"first\\n\nsecond"', message='type mismatch')

    def test_argument_multiline_call(self):
        self.check('fn accept(value:int):int{return value;}\nnative main {\n println(accept(\n   "wrong"\n )); return 0;\n}',
                   '"wrong"', message='type mismatch')

    def test_return_expression(self):
        self.check('fn value():int {\n return "wrong";\n}\nnative main{return 0;}', '"wrong"')

    def test_qualified_call_focus(self):
        helper = 'fn valid():int{return 1;}'
        self.check('import helper;\nnative main {println(helper.missing());return 0;}',
                   'missing', files={'helper.fg': helper}, message='unknown function')

    def test_struct_field_focus(self):
        self.check('struct Item { value:int; }\nfn field(item:Item):int{return item.absent;}\nnative main {return 0;}',
                   'absent', message='unknown field')

    def test_operator_focus(self):
        self.check('native main {let x:int=1;println(x + "wrong");return 0;}',
                   '+', message='non-numeric operand')

    def test_imported_function_identity(self):
        helper = '// absent in comment\nfn read():int {\n return absent;\n}'
        self.check('import helper;native main {println(helper.read());return 0;}',
                   'absent', occurrence=1, files={'helper.fg': helper}, origin='helper.fg')

    def test_transitive_import_const_identity(self):
        files = {'middle.fg': 'import leaf;fn read():int{return leaf.read();}',
                 'leaf.fg': 'const BAD = missing;\nfn read():int{return 1;}'}
        self.check('import middle;native main{return 0;}', 'missing', files=files, origin='leaf.fg')

    def test_imported_extern_argument_reports_call_site(self):
        self.check('import helper;native main {external("wrong");return 0;}', '"wrong"',
                   files={'helper.fg': 'extern fn external(value:int):int;'})

    def test_utf16_crlf_and_filename_escaping(self):
        self.check('native main {\r\n let note:string="한😀"; println(absent);return 0;\r\n}', 'absent',
                   entry='한 글: "entry".fg')

    def test_utf16_parse_error(self):
        self.check('native main { let x:int="한😀" + ;return 0;}', ';', phase='parse')

    def test_for_step_identifier(self):
        self.check('native main {for(let i:int=0;i<1; absent=i+1){}return 0;}',
                   'absent', message='assignment to unknown value')

    def test_reachable_end_closing_brace(self):
        self.check('fn value():int {\n let x:int=1;\n}\nnative main{return 0;}', '}',
                   message='reachable end')

    def test_successful_declaration_cleanup(self):
        # With a sanitizer-built compiler this also guards transferred extern
        # params and parameter chains in entry/module/library/coroutine owners.
        sources = [
            ('import middle;native main {return middle.read(0)-42;}', {
                'leaf.fg': 'const BASE=41;extern fn callback(value:int):int;fn read(extra:int):int{return BASE+extra;}',
                'middle.fg': 'import leaf;fn read(extra:int):int{return leaf.read(extra)+1;}'}),
            ('extern fn foreign(a:int,b:int):int;fn identity(value:int):int{return value;}native main{return identity(0);}', {}),
            ('library cleanup {export fn identity(value:int):int{return value;}}', {}),
            ('process main {coroutine worker(value:int){println(value);}spawn worker(7);}', {})]
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            for text, files in sources:
                entry = work / 'entry.fg'
                entry.write_text(text)
                for name, body in files.items():
                    (work / name).write_text(body)
                result = subprocess.run([config.forge,str(entry),'--check'],
                                        capture_output=True,text=True,timeout=20)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual(result.stderr,'')

    def test_synthetic_unknown_range_and_optimizer_span_preservation(self):
        root = Path(__file__).resolve().parents[1] / 'compiler'
        harness = r'''
#include "parser.h"
#include "optimize.h"
#include "semantic.h"
int main(int argc,char **argv) {
    if(argc>1) {
        forge_set_diagnostics_json(true);
        Program p={0};p.const_count=1;p.consts=calloc(1,sizeof(*p.consts));
        p.consts[0].value=expr_ident(forge_str("unknown"));forge_check_program(&p);return 2;
    }
    const char *text="const A = 9223372036854775807 + 1; const B = 1 * 7;";
    Lexer lx;lexer_init(&lx,text,strlen(text));Program p=parse_program_named(&lx,"synthetic.fg");
    SourceSpan a=p.consts[0].value->span,b=p.consts[1].value->span;
    optimize_program(&p);
    if(p.consts[0].value->kind!=EXPR_INT || p.consts[0].value->as.int_val!=INT64_MIN)return 3;
    if(p.consts[0].value->span.file!=a.file || p.consts[0].value->span.start!=a.start || p.consts[0].value->span.end!=a.end)return 4;
    if(p.consts[1].value->span.file!=b.file || p.consts[1].value->span.start!=b.start || p.consts[1].value->span.end!=b.end)return 5;
    program_free(&p);return 0;
}'''
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source, binary = work / 'test.c', work / 'test'
            source.write_text(harness)
            result = subprocess.run([config.cc, '-std=c11', '-I', str(root), str(source),
                                     *[str(root / (name + '.c')) for name in
                                       ('ast', 'lexer', 'parser', 'source', 'optimize', 'semantic', 'mod_registry')],
                                     '-lm', '-o', str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary), 'unknown'], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 1, result.stderr)
            diagnostic = json.loads(result.stderr)
            self.assertIsNone(diagnostic['file'])
            self.assertIsNone(diagnostic['range'])


if __name__ == '__main__':
    unittest.main()
