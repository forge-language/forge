"""Reproducible native-code comparison: python3 benchmark/compare_optimization.py.

Uses the regression suite's isolated toolchain build. Emits measurements as JSON;
does not infer application-wide performance from this integer microbenchmark.
"""
import json
import platform
from pathlib import Path
import runpy
import statistics
import time

ROOT = Path(__file__).resolve().parents[1]
Harness = runpy.run_path(str(ROOT / "tests/test_regressions.py"))["Regressions"]
try:
    Harness.setUpClass()
    source = Harness.work / "optimization-bench.fg"
    source.write_text('''native main {
    let i: int = 0;
    let x: int = 1;
    while (i < 10000000) {
        x = (x * 1664525 + 1013904223) % 2147483647;
        i = i + 1;
    }
    println(x);
    return 0;
}
''')
    measurements = []
    expected = None
    for level in range(4):
        binary = Harness.work / f"bench-{level}"
        start = time.perf_counter()
        Harness.run_cmd([Harness.forge, source, f"-O{level}", "--cc", Harness.cc, "-o", binary])
        compile_ms = (time.perf_counter() - start) * 1000
        checksum = Harness.run_cmd([binary]).strip()
        if expected is None:
            expected = checksum
        assert checksum == expected, (level, checksum, expected)
        samples = []
        for _ in range(7):
            start = time.perf_counter()
            assert Harness.run_cmd([binary]).strip() == expected
            samples.append((time.perf_counter() - start) * 1000)
        measurements.append(dict(level=level, compile_ms=compile_ms,
                                 median_run_ms=statistics.median(samples),
                                 samples_ms=samples, binary_bytes=binary.stat().st_size,
                                 checksum=checksum))
    print(json.dumps(dict(platform=platform.platform(),
                          compiler=Harness.run_cmd([Harness.cc, "--version"]).splitlines()[0],
                          measurements=measurements), indent=2))
finally:
    Harness.doClassCleanups()
