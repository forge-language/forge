"""Compare the native binary64 formatter with ECMAScript's decimal oracle."""
import argparse
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--root', required=True)
parser.add_argument('--cc', required=True)
parser.add_argument('--node', required=True)
config, remaining = parser.parse_known_args()
sys.argv = [sys.argv[0], *remaining]

NATIVE = r'''
#include "forge/float_format.h"
#include <fenv.h>
#include <inttypes.h>
#include <locale.h>
#include <stdio.h>
int main(int argc, char **argv) {
    (void)argv;
    setlocale(LC_NUMERIC, "");
    if (argc > 1) fesetround(FE_UPWARD);
    uint64_t bits;
    while (scanf("%" SCNx64, &bits) == 1) {
        double value;
        memcpy(&value, &bits, sizeof(value));
        char text[64];
        __forge_format_float(value, text);
        puts(text);
    }
    return ferror(stdin) ? 1 : 0;
}
'''
JAVASCRIPT = r'''
const fs=require('fs');
for(const line of fs.readFileSync(0,'utf8').trim().split('\n')) {
  const bytes=new ArrayBuffer(8), view=new DataView(bytes);
  view.setBigUint64(0,BigInt('0x'+line));
  const value=view.getFloat64(0);
  console.log(Object.is(value,-0)?'-0':String(value));
}
'''


class FloatFormatTests(unittest.TestCase):
    def test_seeded_bits_boundaries_rounding_and_ecmascript_bytes(self) -> None:
        samples = {0, 1, 0x8000000000000000, 0x8000000000000001,
                   0x0010000000000000, 0x000fffffffffffff,
                   0x7fefffffffffffff, 0x7ff0000000000000,
                   0xfff0000000000000, 0x7ff0000000000001,
                   0xfff8000000000000, 0x3fb999999999999a}
        for exponent in range(1, 2047):
            for offset in (-1, 0, 1):
                samples.add((exponent << 52) + offset)
                samples.add(((exponent << 52) + offset) | (1 << 63))
        rng = random.Random(20261022)
        samples.update(rng.getrandbits(64) for _ in range(20000))
        payload = ''.join(f'{value:016x}\n' for value in sorted(samples))
        oracle = subprocess.run([config.node, '-e', JAVASCRIPT], input=payload,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(oracle.returncode, 0, oracle.stderr)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'format.c'
            binary = Path(directory) / 'format'
            source.write_text(NATIVE)
            for optimization in ('-O0', '-O3'):
                compiled = subprocess.run(
                    [config.cc, '-std=c11', optimization, '-Wall', '-Wextra', '-Werror',
                     '-fsanitize=undefined', '-fno-sanitize-recover=all',
                     '-I', str(Path(config.root) / 'include'), str(source),
                     '-lm', '-o', str(binary)],
                    capture_output=True, text=True, timeout=30)
                self.assertEqual(compiled.returncode, 0, compiled.stderr)
                for mode in ([], ['upward']):
                    with self.subTest(optimization=optimization, rounding=mode):
                        result = subprocess.run([str(binary), *mode], input=payload,
                                                capture_output=True, text=True, timeout=30)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(result.stderr, '')
                        actual, expected = result.stdout.splitlines(), oracle.stdout.splitlines()
                        self.assertEqual(len(actual), len(samples))
                        for bits, text, reference in zip(sorted(samples), actual, expected, strict=True):
                            self.assertEqual(text, reference, f'bits={bits:016x}')


if __name__ == '__main__':
    unittest.main()
