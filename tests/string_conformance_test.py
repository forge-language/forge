"""Compare C-string behavior across the native and JavaScript backends."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
for option in ('forge', 'root', 'lib-dir', 'cc', 'node'):
    parser.add_argument('--' + option, required=True)
config, remaining = parser.parse_known_args()
sys.argv = [sys.argv[0], *remaining]


class StringConformanceTests(unittest.TestCase):
    def test_terminated_strings_and_null_results(self) -> None:
        source = r'''import strings;
native main {
 let s:string="ab\0cd";
 let missing:string=str_sub("a",-1,1);
 println(s);println(str_len(s));println(s[2]);
 println(str_sub(s,1,5));println(str_concat(s,"xy\0zz"));
 println(str_eq(s,"ab"));
 println(str_contains(s,"cd"));println(str_contains(s,"\0cd"));
 println(str_trim(" \t a \r\n\0tail"));
 println(str_append(s,120));println(str_len(str_append(s,0)));
 println(str_append_str(s,"xy\0zz"));
 println("[",missing,"]");println(str_len(missing));
 println(str_char_at(missing,0));println(str_sub(missing,0,1));
 println(str_trim(missing));println(str_concat(missing,"x"));
 println(str_append(missing,120));println(str_contains(missing,""));
 println(str_contains("",missing));println(str_eq(missing,""));
 println(str_eq(missing,str_sub("b",-1,1)));
 println(str_sub(s,9223372036854775807,9223372036854775807));
 println(str_trim(" x "));println(str_len("é"));
 println(str_sub("éx",0,2));println(str_char_at("é",1));
 println("[",str_view_sub(str_view("abc"),-1,1),"]");
 return 0;
}'''
        expected = ('ab\n2\n-1\nb\nabxy\n1\n0\n1\na\nabx\n2\n'
                    'abxy\n[]\n0\n-1\n\n\nx\nx\n0\n0\n0\n1\n\n x \n2\né\n169\n[]\n')
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            fg = work / 'main.fg'
            fg.write_text(source, encoding='utf-8')
            for backend in ('native-O0', 'native-O3', 'javascript'):
                with self.subTest(backend=backend):
                    javascript = backend == 'javascript'
                    output = work / ('main.js' if javascript else 'main.c')
                    emitted = subprocess.run(
                        [config.forge, str(fg), '--emit-js' if javascript else '--emit-c',
                         '-o', str(output)], capture_output=True, text=True, timeout=20)
                    self.assertEqual(emitted.returncode, 0, emitted.stderr)
                    command = [config.node, str(output)]
                    if not javascript:
                        binary = work / backend
                        compiled = subprocess.run(
                            [config.cc, '-std=c11', '-' + backend.split('-')[1],
                             '-fsanitize=undefined', '-fno-sanitize-recover=all',
                             '-I', str(Path(config.root) / 'include'), str(output),
                             '-L', config.lib_dir, '-lforge_std', '-lforge_runtime',
                             '-pthread', '-lm', '-o', str(binary)],
                            capture_output=True, text=True, timeout=30)
                        self.assertEqual(compiled.returncode, 0, compiled.stderr)
                        command = [str(binary)]
                    result = subprocess.run(command, capture_output=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout, expected.encode('utf-8'))
                    self.assertEqual(result.stderr, b'')


if __name__ == '__main__':
    unittest.main()
