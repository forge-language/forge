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
 def test_declared_floats_convert_integer_and_boolean_values(self) -> None:
  result = self.compile_run('''import math;
fn half(x: float): float {return x/2;}
fn integer_return(): float {return 5;}
fn bool_return(): float {return true;}
fn once(): int {println("once");return 5;}
native main {
 let x: float=5;println(x/2==2.5);
 x=7;println(x/2==3.5);
 {let x: int=9;x=5;println(x/2==2);}
 x=9;println(x/2==4.5);
 {let x: float=5;x=7;println(x/2==3.5);}
 println(half(5)==2.5);println(half(true)==0.5);
 println(integer_return()/2==2.5);println(bool_return()/2==0.5);
 let flag: float=true;println(flag/2==0.5);
 for(let i: float=1;i<4;i=3){
  println((i==1 && i/2==0.5)||(i==3 && i/2==1.5));if(i==3){break;}
 }
 println(abs_f(-5)/2==2.5);println(half(once())==2.5);
 return 0;
}''')
  self.assertEqual(result.returncode, 0, result.stderr)
  self.assertEqual(result.stdout.splitlines(), ['1'] * 13 + ['once', '1'])
 def test_float_module_callbacks_and_extern_boundaries(self) -> None:
  result = self.compile_run('''import helper;
native main {println(helper.half(5)==2.5);println(helper.run()==2.5);return 0;}''',
   {'helper': '''extern fn js_half(x: float): float;
extern fn js_invoke(callback: int): float;
fn callback(x: float): float {return x/2;}
fn half(x: int): float {return js_half(x);}
fn run(): float {return js_invoke(callback)/2;}'''},
   '''globalThis.ForgeNative={
js_half:x=>{if(typeof x!=='number')throw new Error('float argument is not Number');return x/2;},
js_invoke:fn=>{if(fn(5n)!==2.5)throw new Error('float callback argument is not converted');return 5n;}
};''')
  self.assertEqual(result.returncode, 0, result.stderr)
  self.assertEqual(result.stdout.splitlines(), ['1', '1'])
 def test_parameter_shadowing_module_function(self):
  r=self.compile_run('import helper;native main{println(helper.text("value"));return 0;}',{'helper':'fn text(text: string): string {let read: string=text;return read;}fn read(): string{return "wrong";}'})
  self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout.strip(),"value")
 def test_indexed_module_owners_callbacks_and_shadowing(self):
  padding=''.join(f'fn filler_{i}():int{{return {i};}}' for i in range(32))
  helper=padding+'''fn target(x:int):int{return x+1;}
fn run():int{return target(41);}fn echo(target:string):string{return target;}
extern fn js_invoke(callback:int):int;
fn callback(x:int):int{return target(x);}fn call_callback():int{return js_invoke(callback);}'''
  peer=padding+'fn target(value:string):string{return value;}'
  source='import helper;import peer;'+padding+'''fn target(x:string):string{return x;}
native main{println(helper.run());println(helper.echo("shadow"));println(helper.call_callback());println(peer.target("peer"));println(target("global"));return 0;}'''
  r=self.compile_run(source,{'helper':helper,'peer':peer},'globalThis.ForgeNative={js_invoke:fn=>fn(41n)};')
  self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout,'42\nshadow\n42\npeer\nglobal\n')
 def test_scoped_views_and_builder_bytes(self):
  r=self.compile_run('import strings;native main{let v: int=str_view("한글😀");println(str_view_len(v));println(str_view_at(v,0));println(str_view_at(v,9));println(str_view_at(v,-1));let b: int=str_builder();str_builder_append(b,"한글");let first: string=str_builder_finish(b);str_builder_char(b,240);str_builder_char(b,159);str_builder_char(b,152);str_builder_char(b,128);println(str_builder_finish(b));println(first);println(str_builder_char(b,0));println(str_builder_char(b,256));str_reset_arena();println(str_view_len(v));let next: int=str_builder();str_builder_append(next,"reset");println(str_builder_finish(next));return 0;}')
  self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout.splitlines(),['10','237','128','-1','한글😀','한글','0','0','0','reset'])
 def test_indexed_constants_keep_shadowing(self):
  constants=''.join(f'const K_{i}={i};' for i in range(64))
  warm=''.join(f'fn warm_{i}():int{{return K_63;}}' for i in range(20))
  r=self.compile_run(constants+warm+'''const TEXT="constant";
fn show(TEXT:string):string{return TEXT;}
native main{println(warm_19());println(K_63);
println(show("parameter"));{let TEXT:int=9;println(TEXT);}println(TEXT);return 0;}''')
  self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout,'63\n63\nparameter\n9\nconstant\n')
 def test_builder_with_capacity(self):
  r=self.compile_run('import strings;native main{let b: int=str_builder_with_capacity(3);println(str_builder_char(b,65));str_builder_char(b,66);str_builder_char(b,67);println(str_builder_finish(b));let invalid: int=str_builder_with_capacity(-1);println(invalid);return 0;}')
  self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout.splitlines(),['1','ABC','0'])
 def test_builder_finish_take(self):
  r=self.compile_run('import strings;native main{let b: int=str_builder_with_capacity(3);str_builder_append(b,"ABC");let saved: string=str_builder_finish_take(b);str_builder_char(b,68);println(saved);println(str_builder_finish(b));println(str_builder_finish_take(b));return 0;}')
  self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout.splitlines(),['ABC','D','D'])
 def test_cached_length_immutable_view_append(self):
  r=self.compile_run('import strings;native main{let first: int=str_view("A");let second: int=str_view_append_byte(first,200);println(str_view_len(second));println(str_view_at(second,1));println(str_view_at(second,2));println(str_view_sub(second,0,9223372036854775807));let nul: int=str_view_append_byte(second,0);println(str_view_sub(nul,0,9223372036854775807));println(str_view_len(first));println(str_view_append_byte(0,65));return 0;}')
  self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout.splitlines(),['3','195','136','AÈ','AÈ','1','0'])
 def test_view_builder_nul_terminated_ffi(self):
  r=self.compile_run('import strings;import helper;native main{let s: string=helper.read();let v: int=str_view(s);println(str_view_len(v));println(str_view_sub(v,0,9223372036854775807));println(str_view_matches(v,0,s));println(str_view_matches(v,1,"suffix"));let b: int=str_builder();str_builder_append_view(b,v,0,9223372036854775807);println(str_builder_finish(b));return 0;}',{'helper':'extern fn js_read(): string;fn read(): string{return js_read();}'},'globalThis.ForgeNative={js_read:()=>"a"+String.fromCharCode(0)+"suffix"};')
  self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout.splitlines(),['1','a','1','0','a'])
 def test_builder_invalid_utf8_rejected(self):
  r=self.compile_run('import strings;native main{let b: int=str_builder();str_builder_char(b,255);println(str_builder_finish(b));return 0;}')
  self.assertNotEqual(r.returncode,0)
 def test_decoded_escapes_are_not_interpreted_twice(self):
  r=self.compile_run(r'import strings;native main{println("literal\\n");println("actual\nnewline");println("quote\"slash\\");return 0;}')
  self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout,'literal\\n\nactual\nnewline\nquote"slash\\\n')
 def test_view_ranges_matches_growth_and_snapshots(self):
  r=self.compile_run('''import strings;native main{
let v: int=str_view("한글😀");println(str_view_sub(v,3,3));println(str_view_sub(v,6,9223372036854775807));println(str_view_sub(v,9223372036854775807,1));println(str_view_sub(v,-1,1));println(str_view_sub(0,0,1));println(str_view_matches(v,3,"글"));println(str_view_matches(v,6,"😀!"));println(str_view_matches(v,10,""));println(str_view_matches(v,11,""));println(str_view_matches(v,-1,""));
let b: int=str_builder();str_builder_append_view(b,v,3,3);let snapshot: string=str_builder_finish(b);let saved: int=str_view(snapshot);str_builder_append_view(b,v,6,1);str_builder_append_view(b,v,7,9223372036854775807);println(str_builder_append_view(b,v,-1,1));println(str_builder_append_view(b,v,0,-1));println(str_builder_append_view(b,0,0,1));println(str_builder_finish(b));let i: int=0;while(i<1000){str_builder_append_view(b,saved,0,3);i=i+1;}println(str_len(str_builder_finish(b)));println(snapshot);str_reset_arena();println(str_view_matches(v,0,""));println(str_builder_append_view(b,saved,0,1));return 0;}''')
  self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout.splitlines(),['글','😀','','null','null','1','0','1','0','0','0','0','0','글😀','3007','글','0','0'])
 def test_view_sub_invalid_utf8_rejected(self):
  r=self.compile_run('import strings;native main{let v: int=str_view("😀");println(str_view_sub(v,0,1));return 0;}')
  self.assertNotEqual(r.returncode,0)
 def test_zero_division(self):
  r=self.compile_run('fn divide(x: int): int{return 1/x;}native main{println(divide(0));return 0;}');self.assertNotEqual(r.returncode,0)
 def test_unsupported_native_rejected(self):
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'main.fg').write_text('import tcp;native main{tcp_listen(8080);return 0;}')
   r=subprocess.run([str(FORGE),str(p/'main.fg'),'--emit-js','-o',str(p/'main.js')],capture_output=True,text=True);self.assertNotEqual(r.returncode,0)
if __name__=='__main__':unittest.main()
