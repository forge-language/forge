"""Compare int64 arithmetic to a Python oracle, with UBSan and JavaScript."""
import argparse
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
for name in ('forge', 'root', 'lib-dir', 'cc'):
    parser.add_argument('--' + name, required=True)
parser.add_argument('--node', default='node')
config, remaining = parser.parse_known_args()
sys.argv = [sys.argv[0], *remaining]
MIN, MAX = -(1 << 63), (1 << 63) - 1


def literal(value):
    return '(-9223372036854775807 - 1)' if value == MIN else str(value)


def wrap(value):
    return (value + (1 << 63)) % (1 << 64) - (1 << 63)


class ArithmeticTests(unittest.TestCase):
    def execute(self, source, expected='', failure=False, bridge='', js_bridge='', js=True,
                library_bridge=None, modules=None, extra_c_flags=(), c_suffix='', require_c=None):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            fg, c, binary = [work / name for name in ('test.fg', 'test.c', 'test')]
            fg.write_text(source)
            for name, contents in (modules or {}).items():
                (work / name).write_text(contents)
            args = [config.forge, str(fg), '--emit-c', '-o', str(c)]
            if library_bridge is not None:
                args += ['--lib', '--header', str(work / 'arithmetic.h')]
            result = subprocess.run(args, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            if require_c:
                self.assertIn(require_c, c.read_text())
            if c_suffix:
                c.write_text(c.read_text() + '\n' + c_suffix)
            extras = []
            if bridge or library_bridge is not None:
                extra = work / 'bridge.c'
                extra.write_text(bridge + (library_bridge or ''))
                extras.append(str(extra))
            for opt in ('-O0', '-O3'):
                with self.subTest(optimization=opt):
                    result = subprocess.run([
                        config.cc, '-std=gnu11', opt, '-g', '-fsanitize=undefined',
                        '-fno-sanitize-recover=all', '-Werror', *extra_c_flags,
                        '-I', str(Path(config.root) / 'include'), str(c), *extras,
                        '-L', config.lib_dir, '-lforge_std', '-lforge_runtime',
                        '-lpthread', '-lm', '-o', str(binary)
                    ], capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
                    if failure:
                        self.assertEqual(result.returncode, 1, result.stderr)
                        self.assertEqual(result.stderr, 'Forge integer division\n')
                    else:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(result.stdout, expected)
                        self.assertEqual(result.stderr, '')
            if js:
                script = work / 'test.js'
                result = subprocess.run([config.forge, str(fg), '--emit-js', '-o', str(script)],
                                        capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr)
                script.write_text(js_bridge + '\n' + script.read_text())
                result = subprocess.run([config.node, str(script)], capture_output=True, text=True, timeout=20)
                if failure:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('Forge integer division', result.stderr)
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout, expected)

    def test_reserved_builder_signed_byte_and_index_oracle(self):
        source = '''import strings;
fn make(n:int,seed:int):string {
 let b:int=str_builder_with_capacity(n);if(b==0){return "";}let i:int=0;
 while(i<n){if(str_builder_char(b,65+(i+seed)%26)==0){return "";}i=1+i;}
 println(i);return str_builder_finish_take(b);
}
native main {'''
        expected = []
        for n in (0, 1, 13, 64):
            for seed in (MIN, MIN+1, -100, -26, -1, 0, 1, MAX-1, MAX):
                chars = []
                for i in range(n):
                    value = wrap(i+seed)
                    remainder = (abs(value) % 26) * (-1 if value < 0 else 1)
                    chars.append(chr(65+remainder))
                source += f'println(make({n},{literal(seed)}));str_reset_arena();'
                expected += [str(n), ''.join(chars)]
        source += 'return 0;}'
        output = '\n'.join(expected)+'\n'
        self.execute(source, output)
        self.execute(source, output, js=False, extra_c_flags=('-DFORGE_STRING_NO_INLINE',))

    def test_mersenne_loop_version_full_width_and_affine_oracles(self):
        source = '''fn recurrence(seed:int,n:int):int {
 let value:int=seed;let i:int=0;
 while(i<n){value=(value*48271)%2147483647;i=1+i;}
 println(i);return value;
}
fn affine(seed:int,n:int):int {
 let value:int=seed;let i:int=0;
 while(i<n){value=(value*7+3)%31;i=i+1;}
 println(i);return value;
}
native main {'''
        expected = []
        for name, multiplier, addend, modulus in (('recurrence',48271,0,2147483647),('affine',7,3,31)):
            for n in (-1,0,1,17):
                for seed in (MIN, MIN+1,-2147483647,-1,0,1,30,31,2147483646,2147483647,MAX):
                    value = seed
                    for _ in range(max(n,0)):
                        value = wrap(wrap(value*multiplier)+addend)
                        quotient = abs(value)//modulus * (-1 if value<0 else 1)
                        value -= quotient*modulus
                    source += f'println({name}({literal(seed)},{n}));'
                    expected += [str(max(n,0)), str(value)]
        source += 'return 0;}'
        self.execute(source, '\n'.join(expected)+'\n')

    def test_bounded_modular_power_full_width_and_guard_boundaries(self):
        source = '''fn raise_residue(a:int,bits:int,d:int):int {
 let answer:int=1;let x:int=a%d;let k:int=bits;
 while(k>0){if(k%2==1){answer=(x*answer)%d;}x=(x*x)%d;k=k/2;}
 return answer;
}
native main {'''
        def remainder(a, b):
            if b == 0 or (a == MIN and b == -1):
                raise ArithmeticError
            q = abs(a)//abs(b) * (-1 if (a < 0) != (b < 0) else 1)
            return a-q*b
        expected = []
        for d in (MIN,-26,-1,1,2,3,26,65537,1000003,1373651,
                  2097151,2097152,2097153,4194304,998244353,1000000007,
                  2147483647,2147483648,3037000498,3037000499,3037000500,3037000501,
                  1 << 32,1 << 40,1 << 62,MAX):
            bases = (MIN,MIN+1,-1,0,1,2,MAX) + ((d-1,) if d > 0 else ())
            for a in bases:
                for exponent in (-1,0,1,2,3,31,63,MAX):
                    try:
                        result, factor, power = 1, remainder(a,d), exponent
                        while power > 0:
                            if power%2 == 1:result=remainder(wrap(result*factor),d)
                            factor=remainder(wrap(factor*factor),d)
                            power//=2
                    except ArithmeticError:
                        continue
                    source += f'println(raise_residue({literal(a)},{literal(exponent)},{literal(d)}));'
                    expected.append(str(result))
        source += 'return 0;}'
        output='\n'.join(expected)+'\n'
        self.execute(source, output)
        self.execute(source, output, js=False, extra_c_flags=('-U__SIZEOF_INT128__',))
        self.execute(source, output, js=False, extra_c_flags=('-D__aarch64__=1',))

    def test_wide_modular_power_oracle(self):
        source = '''fn residue(a:int,bits:int,d:int):int {
 let r:int=1;let x:int=a%d;let k:int=bits;
 while(k>0){if(k%2==1){r=(r*x)%d;}x=(x*x)%d;k=k/2;}return r;
}
native main {'''
        # Exact nonzero multiples exercise zero residues and modulus boundaries.
        cases = [(1 << shift, exponent, 1 << (2*shift))
                 for shift in (11, 12, 13, 14, 15) for exponent in (2, 3, 4, 31, MAX)]
        # A mixed even divisor must keep the fallback: a^2 % d = d/2 when
        # a=6t and d=24t^2, and (d/2)*a is a nonzero multiple of d.
        cases += [(6*t, 3, 24*t*t) for t in (512, 1024, 4096, 8192)]
        rng = random.Random(20261015)
        for _ in range(160):
            d = rng.choice((2097153, 1 << 22, 998244353, 1000000007,
                            1 << 31, 3037000499, rng.randrange(2097153,3037000500)))
            a = rng.choice((0, d-1, d, MAX, rng.randrange(MAX+1)))
            cases.append((a, rng.choice((0, 1, 2, 3, MAX, rng.randrange(MAX+1))), d))
        expected = []
        for a, exponent, d in cases:
            source += f'println(residue({a},{exponent},{d}));'
            expected.append(str(pow(a, exponent, d)))
        source += 'return 0;}'
        output = '\n'.join(expected)+'\n'
        self.execute(source, output, require_c='__forge_pow_mask_')
        self.execute(source, output, js=False, extra_c_flags=('-U__SIZEOF_INT128__',))

    def test_power_of_two_modular_power_preserves_wrapping_signs(self):
        source = '''fn residue(a:int,bits:int,d:int):int {
 let r:int=1;let x:int=a%d;let k:int=bits;
 while(k>0){if(k%2==1){r=(r*x)%d;}x=(x*x)%d;k=k/2;}return r;
}
native main {'''
        rng = random.Random(20261016)
        cases = []
        for shift in (21,22,23,31,32,33,40,62):
            d = 1 << shift
            for a in (MIN,MIN+1,-d-1,-d,-d+1,-1,0,1,d-1,d,MAX):
                for exponent in (-1,0,1,2,3,4,31,MAX):
                    cases.append((a,exponent,d))
        cases += [(rng.randrange(MIN,MAX+1), rng.randrange(MAX+1),
                   1 << rng.randrange(22,63)) for _ in range(160)]
        cases += [(1 << 21,3,1 << 40), (3037000500,2,1 << 40)]
        expected = []
        for a, exponent, d in cases:
            # Python signed division oracle, independent of masking/reduction.
            def remainder(value):
                return value%d if value >= 0 else -((-value)%d)
            result, factor, power = 1, remainder(a), exponent
            while power > 0:
                if power%2 == 1:
                    result = remainder(wrap(result*factor))
                factor = remainder(wrap(factor*factor))
                power //= 2
            source += f'println(residue({literal(a)},{literal(exponent)},{d}));'
            expected.append(str(result))
        source += 'return 0;}'
        output = '\n'.join(expected)+'\n'
        self.execute(source, output, require_c='__forge_pow_mask_')
        self.execute(source, output, js=False, extra_c_flags=('-U__SIZEOF_INT128__',))

    def test_bounded_modular_power_preserves_initial_traps(self):
        fn='''fn raise_residue(a:int,bits:int,d:int):int {
 let answer:int=1;let x:int=a%d;let k:int=bits;
 while(k>0){if(k%2==1){answer=(answer*x)%d;}x=(x*x)%d;k=k/2;}
 return answer;
}'''
        for a,d,exponent in ((0,0,0),(1,0,-1),(7,0,5),(MIN,-1,0),(MIN,-1,-1),(MIN,-1,3)):
            with self.subTest(a=a,d=d,exponent=exponent):
                self.execute(fn+f'native main {{println(raise_residue({literal(a)},{literal(exponent)},{literal(d)}));return 0;}}', failure=True)

    def test_bounded_modular_power_exported_library_and_module(self):
        body='''let answer:int=1;let x:int=a%d;let k:int=bits;
 while(k>0){if(k%2==1){answer=(answer*x)%d;}x=(x*x)%d;k=k/2;}
 return answer;'''
        cases=[(7,31,1000003,pow(7,31,1000003)),
               (2097151,MAX,2097152,2097151),
               (7,31,2097153,pow(7,31,2097153)),
               (2048,3,4194304,0),
               (MAX,MAX,3037000499,pow(MAX,MAX,3037000499)),
               (-1,3,1 << 62,-1),
               (MIN+1,2,1 << 62,-(1 << 62)+1),
               ((1 << 62)-1,2,1 << 62,-(1 << 62)+1),
               (-2,3,13,-8),(0,0,2,1),(MIN,2,MAX,1),(MAX,0,2,1),
               (MAX-1,1,MAX,MAX-1),(MIN+1,1,MIN,MIN+1)]
        source='library arithmetic {export fn residue(a:int,bits:int,d:int):int {'+body+'}}'
        bridge='#include "arithmetic.h"\n#include <stdint.h>\nint main(void){\n'
        def c_int(value):
            if value==MIN:return 'INT64_MIN'
            return ('-' if value<0 else '')+f'INT64_C({abs(value)})'
        for index,(a,e,d,wanted) in enumerate(cases,1):
            bridge+=f'if(frlib_arithmetic_residue({c_int(a)},{c_int(e)},{c_int(d)})!={c_int(wanted)})return {index};\n'
        bridge+='return 0;}\n'
        self.execute(source, js=False, library_bridge=bridge, require_c='__forge_pow_')
        module='fn residue(a:int,bits:int,d:int):int {'+body+'}\n'
        # The base and exponent roles can refer to the same bound parameter.
        module+='fn shared(a:int,d:int):int {'+body.replace('k:int=bits','k:int=a')+'}\n'
        source='import powers;fn residue():int{return 42;}native main {println(residue());'
        expected=['42']
        for a,e,d,wanted in cases:
            source+=f'println(powers.residue({literal(a)},{literal(e)},{literal(d)}));'
            expected.append(str(wanted))
        for a,d,wanted in ((7,1000003,pow(7,7,1000003)),(-1,13,1),(MAX,2,1)):
            source+=f'println(powers.shared({literal(a)},{literal(d)}));'
            expected.append(str(wanted))
        source+='return 0;}'
        self.execute(source, '\n'.join(expected)+'\n', modules={'powers.fg':module}, require_c='__forge_pow_')

    def test_bounded_modular_power_exported_library_trap(self):
        source='''library arithmetic {export fn residue(a:int,bits:int,d:int):int {
 let answer:int=1;let x:int=a%d;let k:int=bits;
 while(k>0){if(k%2==1){answer=(answer*x)%d;}x=(x*x)%d;k=k/2;}
 return answer;}}'''
        bridge='''#include "arithmetic.h"
#include <stdint.h>
int main(void){frlib_arithmetic_residue(INT64_C(7),INT64_C(5),INT64_C(0));return 0;}'''
        self.execute(source, failure=True, js=False, library_bridge=bridge, require_c='__forge_pow_')
        self.execute(source, failure=True, js=False, library_bridge=bridge,
                     extra_c_flags=('-U__SIZEOF_INT128__','-D__aarch64__=1'))

    def test_periodic_view_sum_utf8_signed_offsets_and_tails(self):
        source = 'import strings;'
        for period in (1,3,13,32):
            source += f'''fn checksum_{period}(bytes:int,phase:int):int {{
 let size:int=str_view_len(bytes);let cursor:int=0;let total:int=0;let scale:int=phase%{period}+1;
 while(cursor<size){{total=total+str_view_at(bytes,cursor)*scale;cursor=cursor+1;
 scale=1+scale;if(scale>{period}){{scale=1;}}}}return total;
}}'''
        source += 'native main {'
        expected = []
        for period in (1,3,13,32):
            texts = [''.join(chr(65+i%26) for i in range(n))
                     for n in sorted({0,1,period-1,period,period+1,2*period,2*period+1})]
            texts += ['é漢字€', '한글']
            for text in texts:
                for offset in (MIN,-33,-13,-1,0,1,12,31,MAX):
                    quotient=abs(offset)//period * (-1 if offset<0 else 1)
                    weight=offset-quotient*period+1;total=0
                    for byte in text.encode():
                        total=wrap(total+byte*weight);weight+=1
                        if weight>period:weight=1
                    source += f'println(checksum_{period}(str_view({json.dumps(text,ensure_ascii=False)}),{literal(offset)}));str_reset_arena();'
                    expected.append(str(total))
            for offset in (MIN,-1,0,MAX):
                source += f'println(checksum_{period}(0,{literal(offset)}));'
                expected.append('0')
        source += 'return 0;}'
        output='\n'.join(expected)+'\n'
        self.execute(source, output)
        self.execute(source, output, js=False, extra_c_flags=('-DFORGE_STRING_NO_INLINE',))

    def test_reserved_builder_real_allocation_failure_and_capacity_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / 'builder-faults.h'
            header.write_text('''#define FORGE_STRING_NO_INLINE
#include "forge/string.h"
static int64_t mode;
void test_mode(int64_t value) { mode=value; }
static int64_t test_capacity(int64_t n) {
 if (mode==1) return 0;
 if (mode==2) return fr_str_builder_with_capacity(0);
 if (mode==3) return fr_str_builder_with_capacity(n-1);
 int64_t b=fr_str_builder_with_capacity(n+5);
 if (mode==4 && b) fr_str_builder_char(b,90);
 return b;
}
#define fr_str_builder_with_capacity(n) test_capacity(n)
''')
            source = '''import strings;extern fn test_mode(mode:int):void;
fn make(n:int):string {
 let b:int=str_builder_with_capacity(n);if(b==0){return "failed";}let i:int=0;
 while(i<n){if(str_builder_char(b,65+i%26)==0){return "byte-failed";}i=i+1;}
 println(i);return str_builder_finish_take(b);
}
native main {let m:int=0;while(m<5){test_mode(m);println(make(3));str_reset_arena();m=m+1;}
 return 0;}'''
            self.execute(source, '3\nABC\nfailed\n3\nABC\n3\nABC\n3\nZABC\n',
                         js=False, extra_c_flags=('-include', str(header)))

    def test_invariant_modulus_full_width_oracle(self):
        rng = random.Random(20261012)
        pairs = [(a, b) for a in (MIN, MIN+1, -26, -1, 0, 1, 2, MAX-1, MAX)
                 for b in (MIN, -26, -1, 1, 2, 3, 26, 1373653, MAX)]
        pairs = [(a, b) for a, b in pairs if (a, b) != (MIN, -1)]
        pairs += [(rng.randrange(MIN, MAX+1), rng.randrange(2, MAX+1)) for _ in range(64)]
        source = ('fn repeated(a:int,m:int):int {let i:int=0;let sum:int=0;'
                  'while(i<3){sum=sum+a%m;i=i+1;}return sum;} native main {')
        expected = []
        for a, b in pairs:
            quotient = abs(a)//abs(b) * (-1 if (a<0)!=(b<0) else 1)
            expected.append(str(wrap(3*(a-quotient*b))))
            source += f'println(repeated({literal(a)},{literal(b)}));'
        source += 'return 0;}'
        self.execute(source, '\n'.join(expected)+'\n')
        self.execute(source, '\n'.join(expected)+'\n', js=False,
                     extra_c_flags=('-U__SIZEOF_INT128__',))
        self.execute(source, '\n'.join(expected)+'\n', js=False,
                     extra_c_flags=('-D__aarch64__',))

    def test_invariant_modulus_mutation_shadowing_and_empty_loop(self):
        source = '''
fn before(a:int,m:int):int {m=m+1;let i:int=0;let s:int=0;
 while(i<2){s=s+a%m;i=i+1;}return s;}
fn during(a:int,m:int):int {let i:int=0;let s:int=0;
 while(i<2){m=m+2;s=s+a%m;i=i+1;}return s;}
fn after(a:int,m:int):int {let i:int=0;let s:int=0;
 while(i<2){s=s+a%m;i=i+1;}m=m+1;return s+a%m;}
fn shadow(a:int,m:int):int {let i:int=0;let s:int=0;
 while(i<2){let m:int=5;s=s+a%m;i=i+1;}return s;}
fn loop_shadow(a:int,m:int):int {let s:int=0;
 for(let m:int=5;m<8;m=m+1){s=s+a%m;}return s;}
fn empty(a:int,m:int,n:int):int {let i:int=0;let s:int=0;
 while(i<n){s=s+a%m;i=i+1;}return s;}
fn collision(a:int,m:int):int {let __forge_invariant_mod_0:int=11;
 let i:int=0;let s:int=0;while(i<2){s=s+a%m;i=i+1;}
 return s+__forge_invariant_mod_0;}
native main {println(before(17,3));println(during(17,3));println(after(17,3));
 println(shadow(29,7));println(loop_shadow(29,7));println(empty(1,0,0));
 println(empty((-9223372036854775807-1),-1,0));println(collision(17,3));return 0;}
'''
        self.execute(source, '2\n5\n5\n8\n10\n0\n0\n15\n')

    def test_invariant_modulus_checked_traps(self):
        for value, divisor in [(1, 0), (MIN, -1)]:
            with self.subTest(value=value, divisor=divisor):
                self.execute('fn trapped(a:int,m:int):int {let i:int=0;let s:int=0;'
                             'while(i<1){s=a%m;i=i+1;}return s;}native main {'
                             f'println(trapped({literal(value)},{divisor}));return 0;}}',
                             failure=True)

    def test_invariant_modulus_generated_helper_random_oracle(self):
        # Exercise the actual emitted helper, including both signed fallback
        # and the positive full-width reciprocal path, under UBSan.
        suffix = r'''
#undef main
int main(void) {
    uint64_t state = UINT64_C(20261012);
    for (unsigned i = 0; i < 100000; ++i) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        int64_t n; memcpy(&n, &state, sizeof(n));
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        int64_t d = (int64_t)(state & INT64_MAX);
        if (d < 2) d = 2;
        if (i % 3 == 0) d = d % 2000000 + 2;
        if (i % 5 == 0) d = -d;
        int64_t result = __forge_mod_invariant(n, __forge_mod_prepare(d));
        if (result != n % d) return 2;
    }
    puts("100000 emitted-helper oracle cases passed");
    return 0;
}
'''
        self.execute('fn repeated(a:int,m:int):int {let i:int=0;let s:int=0;'
                     'while(i<3){s=s+a%m;i=i+1;}return s;}native main{return 0;}',
                     '100000 emitted-helper oracle cases passed\n', js=False,
                     extra_c_flags=('-Dmain=forge_unused_main',), c_suffix=suffix)

    def test_invariant_modulus_single_evaluation(self):
        self.execute('extern fn tick():int;extern fn ticks():int;'
                     'fn repeated(m:int):int {let i:int=0;let s:int=0;'
                     'while(i<4){s=s+tick()%m;i=i+1;}return s;}'
                     'native main{println(repeated(3));println(ticks());return 0;}',
                     '4\n4\n',
                     bridge='#include <stdint.h>\nstatic int64_t count;\n'
                            'int64_t tick(void){return ++count;}\nint64_t ticks(void){return count;}\n',
                     js_bridge='let tickCount=0n;globalThis.ForgeNative={tick:()=>++tickCount,ticks:()=>tickCount};')

    def test_modular_constants_and_dynamic_oracle(self):
        rng = random.Random(20261009)
        pairs = [(MAX, 1), (MIN, 1), (MIN, -1), (MAX, MAX), (MIN, MIN), (0, MIN)]
        pairs += [(rng.randrange(MIN, MAX + 1), rng.randrange(MIN, MAX + 1)) for _ in range(32)]
        source = 'fn add(a:int,b:int):int{return a+b;} fn sub(a:int,b:int):int{return a-b;} fn mul(a:int,b:int):int{return a*b;} native main {'
        expected = []
        for a, b in pairs:
            for name, op, value in [('add', '+', a+b), ('sub', '-', a-b), ('mul', '*', a*b)]:
                # Identical mathematical result through both folding and dynamic parameters.
                source += f'println({literal(a)} {op} {literal(b)});println({name}({literal(a)},{literal(b)}));'
                expected += [str(wrap(value))] * 2
        source += 'return 0;}'
        self.execute(source, '\n'.join(expected) + '\n')

    def test_signed_division_and_remainder(self):
        source = 'fn quotient(a:int,b:int):int{return a/b;} fn mod(a:int,b:int):int{return a%b;} native main {'
        expected = []
        for a, b in [(MIN, 1), (MAX, -1), (-7, 3), (7, -3), (-7, -3), (0, 7)]:
            quotient = abs(a) // abs(b) * (-1 if (a < 0) != (b < 0) else 1)
            for name, op, value in [('quotient', '/', quotient), ('mod', '%', a - quotient*b)]:
                source += f'println({literal(a)}{op}{literal(b)});println({name}({literal(a)},{literal(b)}));'
                expected += [str(value)] * 2
        self.execute(source + 'return 0;}', '\n'.join(expected) + '\n')

    def test_positive_constant_remainder_int64_boundaries(self):
        # The native code generator may lower `a % 7` directly. Positive
        # divisors must retain truncating signed-remainder semantics even at
        # INT64_MIN, where the separate divisor=-1 trap does not apply.
        values = [MIN, MIN + 1, -15, -7, -1, 0, 1, 7, 15, MAX]
        source = 'fn positive_mod7(a:int):int{return a%7;} native main {'
        expected = []
        for value in values:
            quotient = abs(value) // 7 * (-1 if value < 0 else 1)
            expected.append(str(value - quotient * 7))
            source += f'println(positive_mod7({literal(value)}));'
        self.execute(source + 'return 0;}', '\n'.join(expected) + '\n')

    def test_positive_constant_division_int64_boundaries(self):
        # Positive literal divisors cannot trigger Forge's divide-by-zero or
        # INT64_MIN/-1 trap, so native codegen may emit division directly.
        values = [MIN, MIN + 1, -15, -7, -1, 0, 1, 7, 15, MAX]
        source = 'fn quotient7(a:int):int{return a/7;} native main {'
        expected = []
        for value in values:
            quotient = abs(value) // 7 * (-1 if value < 0 else 1)
            expected.append(str(quotient))
            source += f'println(quotient7({literal(value)}));'
        self.execute(source + 'return 0;}', '\n'.join(expected) + '\n')

    def test_mersenne_modmul_guard_preserves_wrapping_fallback(self):
        # The native backend may use a Mersenne fold only for a nonnegative
        # in-range multiplicand whose product is proven not to overflow.
        # Every other value must retain Forge's wrapping multiply and signed
        # remainder behavior.
        rng = random.Random(20261013)
        cases = [(31, 48271), (2, 2), (4, 7), (62, 1)]
        functions = ''
        calls = ''
        expected = []
        for index, (shift, multiplier) in enumerate(cases):
            modulus = (1 << shift) - 1
            name = f'mersenne_step_{index}'
            functions += (f'fn {name}(value:int):int{{'
                          f'return (value*{multiplier})%{modulus};}} ')
            values = [MIN, MIN + 1, -modulus, -1, 0, 1, modulus - 1,
                      modulus, modulus + 1, MAX]
            values += [rng.randrange(MIN, MAX + 1) for _ in range(24)]
            for value in values:
                product = wrap(value * multiplier)
                quotient = abs(product) // modulus * (-1 if product < 0 else 1)
                expected.append(str(product - quotient * modulus))
                calls += f'println({name}({literal(value)}));'
        self.execute(functions + 'native main {' + calls + 'return 0;}',
                     '\n'.join(expected) + '\n')

    def test_mersenne_affine_modmul_guard_preserves_wrapping_fallback(self):
        # Affine congruential updates can use the fold only when the full
        # multiply-add is positive and representable. All other inputs retain
        # the language's wrapping arithmetic and signed remainder.
        rng = random.Random(20261014)
        cases = [(31, 48271, 1), (31, 48271, 1000), (7, 3, 5), (62, 1, 3)]
        functions = ''
        calls = ''
        expected = []
        for index, (shift, multiplier, addend) in enumerate(cases):
            modulus = (1 << shift) - 1
            name = f'mersenne_affine_step_{index}'
            functions += (f'fn {name}(value:int):int{{'
                          f'return (value*{multiplier}+{addend})%{modulus};}} ')
            values = [MIN, MIN + 1, -modulus, -1, 0, 1, modulus - 1,
                      modulus, modulus + 1, MAX]
            values += [rng.randrange(MIN, MAX + 1) for _ in range(24)]
            for value in values:
                expression = wrap(value * multiplier + addend)
                quotient = abs(expression) // modulus * (-1 if expression < 0 else 1)
                expected.append(str(expression - quotient * modulus))
                calls += f'println({name}({literal(value)}));'
        self.execute(functions + 'native main {' + calls + 'return 0;}',
                     '\n'.join(expected) + '\n')

    def test_dynamic_division_width_boundaries_and_once_evaluation(self):
        # External calls keep operands dynamic even at -O3. Exercise both
        # sides of the unsigned-32 fast path and the full signed fallback.
        edge = (1 << 32) - 1
        pairs = [(0, 1), (edge, 1), (edge, 3), (edge, edge),
                 (edge + 1, 3), (edge, edge + 1), (edge + 1, edge),
                 (-(1 << 31), -1), (-(1 << 31), 3), ((1 << 31) - 1, 3),
                 (-1, 3), (-edge, 3), (edge, -3), (-edge, -3),
                 (MIN, 1), (MIN, 3), (MAX, -1), (MAX, edge), (MAX, MIN)]
        rng = random.Random(20261012)
        pairs += [(rng.randrange(0, edge + 1), rng.randrange(1, edge + 1))
                  for _ in range(16)]
        source = 'extern fn opaque(value:int):int; extern fn calls():int; native main {'
        expected = []
        for a, b in pairs:
            quotient = abs(a) // abs(b) * (-1 if (a < 0) != (b < 0) else 1)
            source += f'println(opaque({literal(a)})/opaque({literal(b)}));'
            source += f'println(opaque({literal(a)})%opaque({literal(b)}));'
            expected += [str(quotient), str(a - quotient*b)]
        source += 'println(calls());return 0;}'
        expected.append(str(4 * len(pairs)))
        bridge = ('#include <stdint.h>\nstatic int64_t count; '
                  'int64_t opaque(int64_t value){volatile int64_t result=value;count++;return result;} '
                  'int64_t calls(void){return count;}\n')
        js_bridge = ('let count=0n; globalThis.ForgeNative={'
                     'opaque:(value)=>{count++;return value;},calls:()=>count};')
        self.execute(source, '\n'.join(expected) + '\n', bridge=bridge, js_bridge=js_bridge)

    def test_dynamic_division_traps_with_external_operands(self):
        bridge = '#include <stdint.h>\nint64_t opaque(int64_t value){volatile int64_t result=value;return result;}\n'
        js_bridge = 'globalThis.ForgeNative={opaque:(value)=>value};'
        for op in ('/', '%'):
            for a, b in [(0, 0), ((1 << 32) - 1, 0), (MIN, -1)]:
                with self.subTest(op=op, a=a, b=b):
                    source = ('extern fn opaque(value:int):int; native main {'
                              f'println(opaque({literal(a)}){op}opaque({literal(b)}));return 0;}}')
                    self.execute(source, failure=True, bridge=bridge, js_bridge=js_bridge)

    def test_invalid_division_and_remainder(self):
        for op in ('/', '%'):
            for a, b in [(1, 0), (MIN, -1)]:
                for dynamic in (False, True):
                    with self.subTest(op=op, a=a, b=b, dynamic=dynamic):
                        expr = f'calculate({literal(a)},{literal(b)})' if dynamic else f'{literal(a)}{op}{literal(b)}'
                        source = f'fn calculate(a:int,b:int):int{{return a{op}b;}} native main{{println({expr});return 0;}}'
                        self.execute(source, failure=True)

    def test_float_promotion_both_operands_and_once_evaluation(self):
        source = '''extern fn tick(): int; extern fn ticks(): int; extern fn half(): float; extern fn minus_zero(): float;
fn floating(x:float):float{return x;} native main {
if (floating(1.5)+2 != 3.5) {return 1;} if (2+floating(1.5) != 3.5) {return 2;}
if (floating(1.5)-2 != -0.5) {return 3;} if (2-floating(1.5) != 0.5) {return 4;}
if (floating(1.5)*2 != 3.0) {return 5;} if (2*floating(1.5) != 3.0) {return 6;}
if (floating(1.5)/2 != 0.75) {return 7;} if (2/floating(0.5) != 4.0) {return 8;}
if (half()+1 != 1.5) {return 9;} if (1+half() != 1.5) {return 10;}
if (!(1.0/floating(0.0) > 0.0)) {return 11;}
if (!(1.0/(minus_zero()*2) < 0.0)) {return 12;}
println(tick()+tick()); println(ticks()); println(tick()*tick()); println(ticks());
return 0;}'''
        bridge = '#include <stdint.h>\nstatic int64_t count; int64_t tick(void){count++;return 2;} int64_t ticks(void){return count;} double half(void){return 0.5;} double minus_zero(void){return -0.0;}\n'
        js_bridge = 'let count=0n; globalThis.ForgeNative={tick:()=>{count++;return 2n;},ticks:()=>count,half:()=>0.5,minus_zero:()=>-0.0};'
        self.execute(source, '4\n2\n4\n4\n', bridge=bridge, js_bridge=js_bridge)

    def test_unary_compound_loop_and_min_constant(self):
        self.execute('''const low = -9223372036854775807 - 1;
native main {let x:int=9223372036854775807;x+=1;println(x);println(-x);
let i:int=0;while(i<3){x-=1;i+=1;}println(x);x*=2;println(x);println(low);return 0;}''',
                     f'{MIN}\n{MIN}\n{MAX-2}\n-6\n{MIN}\n')

    def test_guarded_unit_increments_at_signed_boundaries(self):
        source = '''fn count(start:int,limit:int):int {
let i:int=start; while(i<limit){i=i+1;} return i;}
fn through_continue(start:int,limit:int):int {
let i:int=start; for(i=start;i<limit;i+=1){continue;} return i;}
fn skip_once(start:int,limit:int):int {
let i:int=start;let gate:int=0;
while(i<limit){if(gate==0){gate=1;continue;}i=1+i;}return i;}
fn stop_early(start:int,limit:int):int {
let i:int=start;while(i<limit){if(i==limit-1){break;}i+=1;}return i;}
native main {'''
        expected = []
        for start, limit in [(MAX-2, MAX), (MIN, MIN+2), (0, -1), (-2, 1)]:
            for name in ('count', 'through_continue', 'skip_once'):
                source += f'println({name}({literal(start)},{literal(limit)}));'
                expected.append(str(max(start, limit)))
        source += f'println(stop_early({MAX-2},{MAX}));return 0;}}'
        expected.append(str(MAX-1))
        self.execute(source, '\n'.join(expected) + '\n')

    def test_unproven_loop_updates_keep_wrapping(self):
        # Every loop performs at most one overflowing iteration. A gate then
        # exits after wrap, so an unsafe signed lowering fails UBSan/output.
        cases = [
            (MAX, 'i<=limit', 'i+=1;'),
            (MAX-1, 'i<limit', 'i+=2;'),
            (MAX-1, 'i<limit', 'i+=1;i+=1;'),
            (MAX-1, 'i<limit', f'i={MAX};i+=1;'),
            (MAX-1, 'i<limit', f'let j:int=0;while(j<1){{i={MAX};j+=1;}}i+=1;'),
            (MAX-1, 'i<limit', f'for(let j:int=0;j<1;i={MAX}){{j+=1;}}i+=1;'),
            (MAX-1, 'i<limit', f'for(i={MAX};0;i=i){{}}i+=1;'),
            (MAX-1, 'i<limit', f'match gate {{1=>{{i={MAX};}}_=>{{}}}}i+=1;'),
        ]
        source = ''
        for index, (start, condition, updates) in enumerate(cases):
            source += (f'fn case{index}():int{{let i:int={start};let limit:int={MAX};'
                       f'let gate:int=0;while({condition}){{if(gate!=0){{break;}}'
                       f'gate=1;{updates}}}return i;}}')
        source += 'native main {' + ''.join(f'println(case{index}());' for index in range(len(cases))) + 'return 0;}'
        self.execute(source, (str(MIN) + '\n') * len(cases))

    def test_for_body_mutation_and_shadowed_bounds(self):
        source = f'''fn changed():int{{let i:int=0;let gate:int=0;
for(i={MAX-1};i<{MAX};i+=1){{if(gate!=0){{break;}}gate=1;i={MAX};continue;}}
return i;}}
fn shadowed():int{{let i:int={MAX-1};let result:int=0;
while(i<{MAX}){{{{let i:int={MAX};i+=1;result=i;}}i+=1;}}return result;}}
fn floating():int{{let limit:int=1;let result:int=0;
{{let limit:float=18446744073709551616.0;let i:int={MAX};let gate:int=0;
while(i<limit){{if(gate!=0){{break;}}gate=1;i+=1;}}result=i;}}return result;}}
native main{{println(changed());println(shadowed());println(floating());return 0;}}'''
        self.execute(source, (str(MIN) + '\n') * 3)

    def test_integer_bound_mutation_and_shadow(self):
        source = f'''fn changed():int{{let i:int={MAX-2};let limit:int={MAX};
while(i<limit){{limit=i+1;i+=1;}}return i;}}
fn shadowed():int{{let i:int={MAX-2};let limit:int={MAX};let sum:int=0;
while(i<limit){{{{let limit:int=i;sum=sum+limit;}}i+=1;}}return sum;}}
native main{{println(changed());println(shadowed());return 0;}}'''
        self.execute(source, f'{MAX-1}\n-5\n')

    def test_imported_module_loop_boundaries(self):
        module = f'''fn count(start:int,limit:int):int{{let i:int=start;
while(i<limit){{i+=1;}}return i;}}
fn wrapping():int{{let i:int={MAX};let limit:int={MAX};let gate:int=0;
while(i<=limit){{if(gate!=0){{break;}}gate=1;i+=1;}}return i;}}'''
        source = f'''import edge;
native main{{println(edge.count({MAX-2},{MAX}));
println(edge.count({literal(MIN)},{literal(MIN+2)}));println(edge.wrapping());return 0;}}'''
        self.execute(source, f'{MAX}\n{MIN+2}\n{MIN}\n', modules={'edge.fg': module})

    def test_nested_safe_loops_and_unbounded_add_afterwards(self):
        source = f'''fn nested(start:int,limit:int):int{{let outer:int=start;let total:int=0;
while(outer<limit){{let inner:int=start;
while(inner<limit){{total+=1;inner+=1;}}outer+=1;}}return total;}}
native main{{println(nested({MAX-2},{MAX}));
println(nested({literal(MIN)},{literal(MIN+2)}));
let i:int={MAX};i+=1;println(i);return 0;}}'''
        self.execute(source, f'4\n4\n{MIN}\n')

    def test_process_and_suspended_coroutine_loop_boundaries(self):
        source = f'''process main {{
coroutine work(){{let ci:int={MAX-1};let limit:int={MAX};
while(ci<limit){{yield;ci+=1;}}println(ci);
let cj:int={MAX};let gate:int=0;
while(cj<=limit){{if(gate!=0){{break;}}gate=1;yield;cj+=1;}}println(cj);}}
let pi:int={MAX-1};let bound:int={MAX};
while(pi<bound){{pi+=1;}}println(pi);
let pj:int={MAX};let gate:int=0;
while(pj<=bound){{if(gate!=0){{break;}}gate=1;pj+=1;}}println(pj);
spawn work();}}'''
        self.execute(source, f'{MAX}\n{MIN}\n{MAX}\n{MIN}\n', js=False)

    def test_coroutine_loop_control_after_suspension(self):
        # Each resumed continue must skip the following assignment. Each
        # resumed inner break must leave that loop rather than the outer loop
        # or the coroutine's enclosing resume switch.
        source = '''process main {coroutine work(){let outer:int=0;let sum:int=0;
while(outer<3){let inner:int=0;
while(inner<4){inner+=1;yield;if(inner==1){continue;}if(inner==3){break;}sum+=inner;}
outer+=1;}println(sum);println(outer);}spawn work();}'''
        self.execute(source, '6\n3\n', js=False)

    def test_mersenne_modmul_in_suspended_coroutine(self):
        # The matched local can live in the coroutine state struct; emit it
        # through normal identifier lowering so the generated state prefix is
        # retained across suspension.
        source = '''process main {coroutine work(value:int){let i:int=0;
while(i<2){println((value*2)%3);value=value+1;i=i+1;yield;}}
spawn work(1);}'''
        self.execute(source, '2\n1\n', js=False)

    def test_bool_promotion_and_short_circuit(self):
        self.execute('fn invalid():int{return 1/0;} native main {let yes:bool=true;let no:bool=false;println(yes+yes);println(5-no);println(yes*7);if(no && invalid()){return 1;}if(yes || invalid()){println(1);}return 0;}', '2\n5\n7\n1\n')

    def test_suspended_coroutine_arithmetic(self):
        self.execute('process main {coroutine work(){let x:int=9223372036854775807;yield;x+=1;println(x);x=x*-1;println(x);yield;println(x/2);}spawn work();}', f'{MIN}\n{MIN}\n{MIN//2}\n', js=False)

    def test_library_boundary(self):
        source = '''library arithmetic {export fn add(a:int,b:int):int{return a+b;}
export fn mixed(a:int,b:float):float{return a+b;} export fn div(a:int,b:int):int{return a/b;}
export fn count(a:int,b:int):int{while(a<b){a+=1;}return a;}}'''
        bridge = '''#include "arithmetic.h"
#include <stdint.h>
#include <stdio.h>
int main(void){if(frlib_arithmetic_add(INT64_MAX,1)!=INT64_MIN)return 1;
if(frlib_arithmetic_mixed(1,0.5)!=1.5)return 2;if(frlib_arithmetic_div(-7,3)!=-2)return 3;
if(frlib_arithmetic_count(INT64_MAX-2,INT64_MAX)!=INT64_MAX)return 4;
if(frlib_arithmetic_count(INT64_MIN,INT64_MIN+2)!=INT64_MIN+2)return 5;return 0;}'''
        self.execute(source, js=False, library_bridge=bridge)


if __name__ == '__main__':
    unittest.main()
