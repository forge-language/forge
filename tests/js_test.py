#!/usr/bin/env python3
import pathlib,subprocess,tempfile,unittest,sys
FORGE=pathlib.Path(sys.argv.pop(1)).resolve()
class JavaScriptTest(unittest.TestCase):
 def compile_run(self,source,modules=None,bridge=''):
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'main.fg').write_text(source)
   for name,text in (modules or {}).items():(p/(name+'.fg')).write_text(text)
   r=subprocess.run([str(FORGE),str(p/'main.fg'),'--emit-js','-o',str(p/'main.js'),'-I',str(p)],capture_output=True,text=True)
   self.assertEqual(r.returncode,0,r.stderr)
   (p/'run.cjs').write_text(bridge+'\n'+(p/'main.js').read_text())
   return subprocess.run(['node',str(p/'run.cjs')],capture_output=True,text=True)
 def test_int64_loops_modules_ffi(self):
  r=self.compile_run('import helper; native main { let n: int=0; for(let i: int=0;i<5;i=i+1){n=n+i*i;} println(n);println(9007199254740993);println(helper.read());return 0; }',{'helper':'extern fn js_read(): string; fn read(): string {return js_read();}'},'globalThis.ForgeNative={js_read:()=>"연결 성공"};')
  self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout.splitlines(),['30','9007199254740993','연결 성공'])
 def test_callbacks_and_forward_calls(self):
  r=self.compile_run('import helper;native main { helper.start(helper.callback);return 0; }',{'helper':'extern fn js_start(callback: int): int;fn start(callback: int): int{return js_start(callback);}fn callback(value: int): int{println(value);return value;} '},'globalThis.ForgeNative={js_start:fn=>fn(7n)};')
  self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout.strip(),'7')
 def test_mixed_float_and_strings(self):
  r=self.compile_run('import strings; native main {println(1.5+2);println(str_len("한글"));println(str_char_at("한글",0));println(str_char_at("a",9));println(str_sub("한글",3,3));return 0;}')
  self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout.splitlines(),['3.5','6','237','-1','글'])
 def test_parameter_shadowing_module_function(self):
  r=self.compile_run('import helper;native main{println(helper.text("value"));return 0;}',{'helper':'fn text(text: string): string {let read: string=text;return read;}fn read(): string{return "wrong";}'})
  self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout.strip(),"value")
 def test_scoped_views_and_builder_bytes(self):
  r=self.compile_run('import strings;native main{let v: int=str_view("한글😀");println(str_view_len(v));println(str_view_at(v,0));println(str_view_at(v,9));println(str_view_at(v,-1));let b: int=str_builder();str_builder_append(b,"한글");let first: string=str_builder_finish(b);str_builder_char(b,240);str_builder_char(b,159);str_builder_char(b,152);str_builder_char(b,128);println(str_builder_finish(b));println(first);println(str_builder_char(b,0));println(str_builder_char(b,256));str_reset_arena();println(str_view_len(v));let next: int=str_builder();str_builder_append(next,"reset");println(str_builder_finish(next));return 0;}')
  self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout.splitlines(),['10','237','128','-1','한글😀','한글','0','0','0','reset'])
 def test_view_builder_nul_terminated_ffi(self):
  r=self.compile_run('import strings;import helper;native main{let s: string=helper.read();let v: int=str_view(s);println(str_view_len(v));let b: int=str_builder();str_builder_append(b,s);println(str_builder_finish(b));return 0;}',{'helper':'extern fn js_read(): string;fn read(): string{return js_read();}'},'globalThis.ForgeNative={js_read:()=>"a"+String.fromCharCode(0)+"suffix"};')
  self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout.splitlines(),['1','a'])
 def test_builder_invalid_utf8_rejected(self):
  r=self.compile_run('import strings;native main{let b: int=str_builder();str_builder_char(b,255);println(str_builder_finish(b));return 0;}')
  self.assertNotEqual(r.returncode,0)
 def test_zero_division(self):
  r=self.compile_run('fn divide(x: int): int{return 1/x;}native main{println(divide(0));return 0;}');self.assertNotEqual(r.returncode,0)
 def test_unsupported_native_rejected(self):
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'main.fg').write_text('import tcp;native main{tcp_listen(8080);return 0;}')
   r=subprocess.run([str(FORGE),str(p/'main.fg'),'--emit-js','-o',str(p/'main.js')],capture_output=True,text=True);self.assertNotEqual(r.returncode,0)
if __name__=='__main__':unittest.main()
