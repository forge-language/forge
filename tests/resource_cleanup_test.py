"""Exercise scoped resource cleanup through generated native programs."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
for name in ('forge', 'root', 'lib-dir', 'cc'):
    parser.add_argument('--' + name, required=True)
config, remaining = parser.parse_known_args()
sys.argv = [sys.argv[0], *remaining]

BRIDGE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int64_t next_id;
static int live[128];
static int64_t handles[128];
static int allocation_failure;
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t count, size_t size) {
    if (allocation_failure) {allocation_failure=0;puts("oom");return NULL;}
    return __real_calloc(count,size);
}
int64_t __real_fr_os_command(const char *);
void __real_fr_os_command_free(int64_t);
int64_t __real_fr_os_command_run(int64_t);
static void verify_cleanup(void) {
    for (int64_t i=1;i<=next_id;++i) if(live[i]) abort();
}
static int64_t identify(int64_t handle) {
    for (int64_t i=next_id;i>0;--i) if(live[i] && handles[i]==handle) return i;
    abort();
}
int64_t __wrap_fr_os_command(const char *name) {
    int inject=!strcmp(name,"__forge_test_oom__");
    allocation_failure=inject;
    int64_t handle=__real_fr_os_command(inject ? "true" : name);
    if (inject && (allocation_failure || handle)) abort();
    if (!handle) return 0;
    if (!next_id) atexit(verify_cleanup);
    int64_t id = ++next_id;
    if (id >= 128) abort();
    live[id] = 1; handles[id]=handle;
    printf("new %lld\n", (long long)id);
    return handle;
}
void __wrap_fr_os_command_free(int64_t handle) {
    if (!handle) return;
    int64_t id=identify(handle);
    live[id] = 0;
    printf("free %lld\n", (long long)id);
    __real_fr_os_command_free(handle);
}
int64_t __wrap_fr_os_command_run(int64_t handle) {
    int64_t id=identify(handle);
    printf("run %lld\n", (long long)id);
    return __real_fr_os_command_run(handle);
}
'''


class ResourceCleanupTests(unittest.TestCase):
    def execute(self, source: str, expected: str) -> None:
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            fg, c, bridge, binary = (work / name for name in ('main.fg', 'main.c', 'bridge.c', 'main'))
            fg.write_text('import os;\n' + source)
            bridge.write_text(BRIDGE)
            emitted = subprocess.run([config.forge, str(fg), '--emit-c', '-o', str(c)],
                                     capture_output=True, text=True, timeout=20)
            self.assertEqual(emitted.returncode, 0, emitted.stderr)
            for optimization in ('-O0', '-O3'):
                with self.subTest(optimization=optimization):
                    compiled = subprocess.run([
                        config.cc, '-std=c11', optimization, '-I', str(Path(config.root) / 'include'),
                        str(c), str(bridge), '-L', config.lib_dir, '-lforge_std', '-lforge_runtime',
                        '-pthread', '-lm', '-Wl,--wrap=fr_os_command',
                        '-Wl,--wrap=fr_os_command_free', '-Wl,--wrap=fr_os_command_run',
                        '-Wl,--wrap=calloc',
                        '-o', str(binary)], capture_output=True, text=True, timeout=30)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout, expected)

    def test_reverse_cleanup_when_return_expression_uses_owner(self) -> None:
        self.execute('''fn task():int {
 own let a:int=os_command("true");own let b:int=os_command("b");
 return os_command_run(a);
} native main{println(task());return 0;}''',
                     'new 1\nnew 2\nrun 1\nfree 2\nfree 1\n0\n')

    def test_allocation_failure_releases_old_owner_and_skips_null(self) -> None:
        self.execute('''native main{
 own let failed:int=os_command("__forge_test_oom__");
 own let cmd:int=os_command("true");cmd=os_command("__forge_test_oom__");
 println(failed==0);println(cmd==0);return 0;
}''', 'oom\nnew 1\noom\nfree 1\n1\n1\n')

    def test_cleanup_when_nested_shadow_returns_and_match_falls_through(self) -> None:
        self.execute('''fn task():float{
 own let cmd:int=os_command("true");
 {own let cmd:int=os_command("true");return os_command_run(cmd);}
} fn fallthrough(){own let cmd:int=os_command("true");}
native main{
 println(task());fallthrough();
 match 1 {1=>{own let cmd:int=os_command("true");} _=>{}}
 let legacy:int=os_command("true");os_command_free(legacy);return 0;
}''', 'new 1\nnew 2\nrun 2\nfree 2\nfree 1\n0\nnew 3\nfree 3\nnew 4\nfree 4\nnew 5\nfree 5\n')

    def test_for_owner_survives_continue_and_nested_break(self) -> None:
        self.execute('''native main{
 let i:int=0;
 for(own let cmd:int=os_command("true");i<2;i=i+1){
  own let item:int=os_command("true");
  if(os_command_arg(cmd,"arg")==0){return 1;}
  while(true){own let nested:int=os_command("true");break;}
  continue;
 }
 return 0;
}''', 'new 1\nnew 2\nnew 3\nfree 3\nfree 2\nnew 4\nnew 5\nfree 5\nfree 4\nfree 1\n')

    def test_cleanup_when_shadowing_loop_exits_and_overwriting(self) -> None:
        self.execute('''native main {
 own let a:int=os_command("outer");
 {own let a:int=os_command("inner");}
 a=os_command("replacement");
 for(let i:int=0;i<3;i=i+1){
  own let item:int=os_command("iteration");if(i==0){continue;}break;
 }
 own let failed:int=os_command("");
 return 0;
}''', 'new 1\nnew 2\nfree 2\nnew 3\nfree 1\nnew 4\nfree 4\nnew 5\nfree 5\nfree 3\n')

    def test_reject_escape_alias_and_manual_release(self) -> None:
        bodies = (
            'let alias:int=cmd;', 'println(cmd);', 'os_command_free(cmd);',
            'cmd=42;', 'let alias:bool=cmd==1;', 'send 0, Drop, cmd;',
        )
        with tempfile.TemporaryDirectory() as directory:
            fg = Path(directory) / 'main.fg'
            for body in bodies:
                with self.subTest(body=body):
                    fg.write_text('import os;native main{own let cmd:int=os_command("x");' + body + 'return 0;}')
                    result = subprocess.run([config.forge, str(fg), '--check'],
                                            capture_output=True, text=True, timeout=20)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn('owned resource', result.stderr)

    def test_reject_return_process_and_unverified_factory(self) -> None:
        sources = (
            'fn task():int{own let cmd:int=os_command("true");return cmd;}native main{return 0;}',
            'process main{coroutine task(){own let cmd:int=os_command("true");yield;}spawn task();}',
            'fn other():int{return 42;}native main{own let cmd:int=other();return 0;}',
        )
        with tempfile.TemporaryDirectory() as directory:
            fg, output = (Path(directory) / name for name in ('main.fg', 'preserved.c'))
            for source in sources:
                for mode in ('--check', '--emit-c', '--emit-js'):
                    with self.subTest(source=source, mode=mode):
                        fg.write_text('import os;' + source)
                        output.write_text('existing output')
                        result = subprocess.run([config.forge, str(fg), mode, '-o', str(output)],
                                                capture_output=True, text=True, timeout=20)
                        self.assertEqual(result.returncode, 1, result.stderr)
                        self.assertIn('owned resource', result.stderr)
                        self.assertEqual(output.read_text(), 'existing output')

    def test_sdk_factory_when_source_function_has_same_name(self) -> None:
        # Imported SDK functions have precedence over same-named source functions.
        self.execute('''fn os_command(s:string):int{return 42;}
native main{own let cmd:int=os_command("true");return 0;}''', 'new 1\nfree 1\n')

    def test_source_helpers_borrow_owner_without_releasing_it(self) -> None:
        self.execute('''fn run(cmd:int):int{return os_command_run(cmd);}
fn argument(text:string,cmd:int):int{return os_command_arg(cmd,text);}
fn execute_once(cmd:int){os_command_run(cmd);}
fn task():int{
 own let cmd:int=os_command("true");
 if(argument("borrowed",cmd)==0){return 1;}
 execute_once(cmd);
 return run(cmd);
}native main{println(task());return 0;}''', 'new 1\nrun 1\nrun 1\nfree 1\n0\n')

    def test_reject_helpers_that_escape_or_reuse_owner(self) -> None:
        helpers = (
            'fn helper(cmd:int):int{return cmd;}',
            'extern fn helper(cmd:int):int;',
            'fn helper(cmd:bool):int{return os_command_run(cmd);}',
            'fn helper(cmd:int):int{let alias:int=cmd;return os_command_run(alias);}',
            'fn helper(cmd:int):int{return os_command_run(cmd)+cmd;}',
            'extern fn text(cmd:int):string;'
            'fn helper(cmd:int):int{return os_command_arg(cmd,text(cmd));}',
        )
        with tempfile.TemporaryDirectory() as directory:
            fg, output = (Path(directory) / name for name in ('main.fg', 'preserved.c'))
            for helper in helpers:
                with self.subTest(helper=helper):
                    fg.write_text('import os;' + helper + 'native main{'
                                  'own let cmd:int=os_command("true");return helper(cmd);}')
                    output.write_text('existing output')
                    result = subprocess.run([config.forge, str(fg), '--emit-c', '-o', str(output)],
                                            capture_output=True, text=True, timeout=20)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn('owned resource', result.stderr)
                    self.assertEqual(output.read_text(), 'existing output')


if __name__ == '__main__':
    unittest.main()
