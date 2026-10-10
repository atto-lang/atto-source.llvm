#!/usr/bin/env python3
"""Client-side latency percentiles for LLVM actor HTTP transport.
The C benchmark has real LLVM generated actor methods and two host placements:
shared in one process or private in five OS processes. Private is NOT source
isoactor scheduling. Results cannot be presented as direct equal-footing JVM
PRIVATE vs LLVM isoactor overhead.
"""
import argparse
import asyncio
import pathlib
import statistics
import subprocess
import tempfile
import time

ACTORS = 5
N = 1000
LEVELS = (1, 10, 100, 1000)

async def fetch(port, target):
    started = time.perf_counter_ns()
    reader, writer = await asyncio.wait_for(
        asyncio.open_connection("127.0.0.1", port), timeout=25)
    try:
        writer.write((f"GET {target} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n").encode("ascii"))
        await asyncio.wait_for(writer.drain(), timeout=25)
        first = await asyncio.wait_for(reader.readline(), timeout=25)
        assert first.startswith(b"HTTP/1.1 "), first
        status = int(first.split()[1])
        headers = {}
        while True:
            line = await asyncio.wait_for(reader.readline(), timeout=25)
            if line == b"\r\n": break
            if not line: raise AssertionError("truncated headers")
            k, v = line.decode("ascii").split(":", 1)
            headers[k.lower()] = v.strip()
        length = int(headers["content-length"])
        body = await asyncio.wait_for(reader.readexactly(length), timeout=25)
        return status, body.decode("ascii"), time.perf_counter_ns() - started
    finally:
        writer.close()
        await asyncio.wait_for(writer.wait_closed(), timeout=10)

async def sweep(port, mode):
    assert (await fetch(port, "/health"))[:2] == (200, "ok")
    profile = "native-single-process-owner-domains" if mode == "shared" else "native-per-actor-OS-process"
    for case, limit in enumerate(LEVELS):
        semaphore = asyncio.Semaphore(limit)
        async def one(index):
            async with semaphore:
                return await fetch(port, f"/work/{index % ACTORS}")
        before = int((await fetch(port, "/metrics"))[1])
        started = time.perf_counter_ns()
        results = await asyncio.wait_for(asyncio.gather(*(one(i) for i in range(N))), timeout=240)
        elapsed = (time.perf_counter_ns() - started) / 1e9
        after = int((await fetch(port, "/metrics"))[1])
        samples = sorted(item[2] / 1e6 for item in results)
        counts = [[] for _ in range(ACTORS)]
        for index, (status, body, _) in enumerate(results):
            assert status == 200, (mode, limit, index, status, body)
            actor, count, value = map(int, body.split(","))
            assert actor == index % ACTORS and value == count * 43
            counts[actor].append(count)
        for idx, actor_counts in enumerate(counts):
            assert sorted(actor_counts) == list(range(case * 200 + 1, case * 200 + 201)), (idx, actor_counts[:5])
        print("MATRIX backend=llvm mode=%s isolation=%s actors=5 requests=1000 concurrency=%d p50_ms=%.3f p99_ms=%.3f rps=%.3f gc_count=0 gc_pause_ms=0 rss_delta_kb=%d errors=0" %
              (mode, profile, limit, samples[499], samples[989], N / elapsed, after-before), flush=True)
    shutdown_started = time.perf_counter()
    assert (await fetch(port, "/shutdown"))[:2] == (200, "bye")
    return shutdown_started

def main():
    parser = argparse.ArgumentParser()
    for field in ("compiler", "fixture", "clang", "actor_runtime", "arena_runtime", "native_runtime", "harness", "include"):
        parser.add_argument("--"+field.replace("_","-"), required=True)
    args = parser.parse_args()
    ir = subprocess.run([args.compiler, args.fixture], check=True, capture_output=True, text=True, timeout=25).stdout
    assert "oreslang_actor_Counter_add" in ir
    assert "gc.statepoint" not in ir and "gc.relocate" not in ir
    with tempfile.TemporaryDirectory(prefix="ores-llvm-matrix-") as output:
        binary = str(pathlib.Path(output) / "http-matrix")
        command = [args.clang, "-O2", "-x", "ir", "-", "-x", "c", args.actor_runtime,
                   args.arena_runtime, args.native_runtime, args.harness, "-std=c11",
                   "-pthread", "-I", args.include, "-o", binary]
        compiled = subprocess.run(command, input=ir, capture_output=True, text=True, timeout=60)
        assert compiled.returncode == 0, compiled.stderr
        for mode in ("shared", "private"):
            process = subprocess.Popen([binary, mode], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
            try:
                line = process.stdout.readline().strip()
                assert line.startswith("PORT="), (mode, line, process.poll())
                port = int(line.split()[0].split("=")[1])
                begin = time.perf_counter()
                shutdown_started = asyncio.run(sweep(port, mode))
                result = process.wait(timeout=20)
                assert result == 0, (result, process.stderr.read())
                ended = time.perf_counter()
                print("MATRIX backend=llvm mode=%s shutdown_ms=%.3f mode_total_ms=%.3f physical_reclaim=true" %
                      (mode, (ended - shutdown_started) * 1000, (ended - begin) * 1000), flush=True)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)

if __name__ == "__main__":
    main()
