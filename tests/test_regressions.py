"""Linux compiler/runtime regressions; run with python3 tests/test_regressions.py."""
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Regressions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="forge-regressions-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.prefix = cls.work / "installed prefix"
        (cls.prefix / "bin").mkdir(parents=True)
        (cls.prefix / "lib").mkdir()
        shutil.copytree(ROOT / "include", cls.prefix / "include")
        cls.cc = os.environ.get("CC", "gcc")
        cls.forge = cls.prefix / "bin" / "forge"
        cls.run_cmd([cls.cc, "-std=c11", "-I" + str(ROOT / "include"),
                     *sorted((ROOT / "compiler").glob("*.c")),
                     ROOT / "runtime/platform.c", "-o", cls.forge])
        for name, sources in (
            ("std", [*sorted((ROOT / "stdlib").glob("*.c")), ROOT / "runtime/arena.c"]),
            ("runtime", [p for p in sorted((ROOT / "runtime").glob("*.c")) if p.name != "arena.c"]),
        ):
            objects = []
            for source in sources:
                obj = cls.work / (name + "_" + source.stem + ".o")
                cls.run_cmd([cls.cc, "-std=c11", "-I" + str(ROOT / "include"),
                             "-c", source, "-o", obj])
                objects.append(obj)
            cls.run_cmd(["ar", "rcs", cls.prefix / "lib" / ("libforge_" + name + ".a"), *objects])
        cls.shared = cls.work / "runtime-tests.so"
        cls.run_cmd([cls.cc, "-std=c11", "-shared", "-fPIC", "-I" + str(ROOT / "include"),
                     ROOT / "runtime/arena.c", ROOT / "stdlib/process.c", "-o", cls.shared])

    @classmethod
    def run_cmd(cls, args, **kwargs):
        env = os.environ.copy()
        env.pop("FORGE_ROOT", None)
        env.pop("FORGE_ENABLE_LTO", None)
        result = subprocess.run(list(map(str, args)), cwd=cls.work, env=env,
                                capture_output=True, text=True, timeout=30, **kwargs)
        if result.returncode:
            raise AssertionError(f"{args}\n{result.stdout}\n{result.stderr}")
        return result.stdout

    def test_external_library_and_installed_detection(self):
        libdir = self.work / "external libs"
        libdir.mkdir()
        self.run_cmd([self.forge, "--lib", ROOT / "examples/external-project/libs/greeting_ext/greeting_ext.fg",
                      "-o", libdir / "libforge_greeting_ext.a", "--header", libdir / "greeting_ext.h", "--cc", self.cc])
        output = self.work / "external-app"
        self.run_cmd([self.forge, ROOT / "examples/external-project/main.fg",
                      "-I", libdir, "-L", libdir, "-l", "forge_greeting_ext", "-o", output, "--cc", self.cc])
        self.assertIn("Hello from an external project, Forge", self.run_cmd([output]))

    def test_explicit_root_overrides_installed_detection(self):
        other = self.work / "other prefix"
        shutil.copytree(self.prefix, other)
        # A compiler-only location must still honor an explicit toolchain root.
        compiler = self.work / "standalone-forge"
        shutil.copy2(self.forge, compiler)
        output = self.work / "hello"
        self.run_cmd([compiler, ROOT / "examples/hello.fg", "--forge-root", other,
                      "--cc", self.cc, "-o", output])
        self.assertIn("Hello from Forge!", self.run_cmd([output]))

    def test_stdlib_string_return_printing(self):
        source = self.work / "strings.fg"
        source.write_text('import docstore; import proc; import json;\n'
                          'native main { doc_set("k", "value"); println(doc_get("k")); '
                          'proc_run("printf captured"); println(proc_output()); '
                          'println(json_array_at("[123]", 0)); return 0; }\n')
        output = self.work / "strings"
        self.run_cmd([self.forge, source, "--cc", self.cc, "-o", output])
        self.assertEqual(self.run_cmd([output]), "value\ncaptured\n123\n")

    def test_lsp_diagnostics(self):
        output = self.work / "forge-lsp"
        self.run_cmd([self.forge, ROOT / "tools/forge-lsp/main.fg", "--cc", self.cc, "-o", output])
        messages = [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "initializationOptions": {"forge": {"path": str(self.forge),
                    "forgeRoot": str(self.prefix), "libDir": str(self.prefix / "lib")}}}},
            {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": "file:///test.fg", "version": 1,
                    "text": "process main { let x: int = ; }"}}},
            {"jsonrpc": "2.0", "id": 2, "method": "shutdown"},
            {"jsonrpc": "2.0", "method": "exit"},
        ]
        bodies = [json.dumps(m) for m in messages]
        payload = "".join(f"Content-Length: {len(b.encode())}\r\n\r\n{b}" for b in bodies)
        response = self.run_cmd([output], input=payload)
        decoded = []
        while response:
            _, body = response.split("\n\n", 1)
            message, end = json.JSONDecoder().raw_decode(body)
            decoded.append(message)
            response = body[end:]
        self.assertEqual(decoded[0]["id"], 1)
        diagnostic = decoded[1]["params"]["diagnostics"][0]
        self.assertEqual(diagnostic["severity"], 1)
        self.assertIn("expected expression", diagnostic["message"])
        self.assertEqual(decoded[-1], {"jsonrpc": "2.0", "id": 2, "result": None})

    def test_optimization_levels_preserve_results(self):
        source = self.work / "optimization.fg"
        source.write_text('''
fn effect(): int { println("effect"); return 3; }
native main {
    let outer: int = 7;
    if (true) { let outer: int = 11; println(outer); }
    println(outer);
    let half: float = 1.0 / 2.0;
    if (half == 0.5) { println("division"); }
    let precise: float = 1.23456789012345;
    if (precise > 1.23456789 && precise < 1.23456790) { println("precision"); }
    let zero: float = 0.0;
    let nan: float = zero / zero;
    let result: float = nan * 0;
    if (result != result) { println("nan"); }
    println(effect() * 0);
    return 0;
}
''')
        for level in range(4):
            with self.subTest(level=level):
                output = self.work / f"optimization-{level}"
                self.run_cmd([self.forge, source, f"-O{level}", "--cc", self.cc, "-o", output])
                self.assertEqual(self.run_cmd([output]), "11\n7\ndivision\nprecision\nnan\neffect\n0\n")

    def test_arena_alignment_and_reset_reuse(self):
        lib = ctypes.CDLL(str(self.shared))
        lib.fr_arena_create.argtypes = [ctypes.c_size_t]
        lib.fr_arena_create.restype = ctypes.c_void_p
        lib.fr_arena_alloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t]
        lib.fr_arena_alloc.restype = ctypes.c_void_p
        lib.fr_arena_reset.argtypes = [ctypes.c_void_p]
        lib.fr_arena_destroy.argtypes = [ctypes.c_void_p]
        arena = lib.fr_arena_create(4096)
        self.assertTrue(arena)
        try:
            first = [lib.fr_arena_alloc(arena, 3000, 64) for _ in range(8)]
            self.assertTrue(all(p and p % 64 == 0 for p in first))
            for _ in range(20):
                lib.fr_arena_reset(arena)
                self.assertEqual([lib.fr_arena_alloc(arena, 3000, 64) for _ in range(8)], first)
            self.assertIsNone(lib.fr_arena_alloc(arena, 16, 3))
            self.assertIsNone(lib.fr_arena_alloc(arena, ctypes.c_size_t(-1).value, 64))
        finally:
            lib.fr_arena_destroy(arena)

    def test_process_drains_large_output(self):
        # A separate process with a timeout turns the former deadlock into a failure.
        code = ('import ctypes,sys; l=ctypes.CDLL(sys.argv[1]); '
                'l.fr_proc_run.argtypes=[ctypes.c_char_p]; '
                'l.fr_proc_output.restype=ctypes.c_char_p; '
                'assert l.fr_proc_run(b"head -c 2097152 /dev/zero | tr \\\"\\\\0\\\" x") == 0; '
                'assert len(l.fr_proc_output()) == 1048575')
        self.run_cmd(["python3", "-c", code, self.shared])

    def test_arena_sanitizers(self):
        output = self.work / "arena-sanitizer"
        self.run_cmd([self.cc, "-std=c11", "-fsanitize=address,undefined", "-g",
                      "-I" + str(ROOT / "include"), ROOT / "tests/arena_sanitizer.c",
                      ROOT / "runtime/arena.c", "-o", output])
        self.run_cmd([output])


if __name__ == "__main__":
    unittest.main(verbosity=2)
