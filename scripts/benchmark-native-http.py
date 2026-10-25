#!/usr/bin/env python3
"""Native transport integration baseline, NOT full Spin/async source parity."""
import argparse
from array import array
import asyncio
import hashlib
import json
import math
from pathlib import Path
import platform
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]

def git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args], text=True).strip()

def percentile(ordered, fraction):
    return round(ordered[max(0, math.ceil(len(ordered)*fraction)-1)], 6) if ordered else None

async def response(reader, writer):
    start = time.perf_counter()
    writer.write(b'GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n')
    await writer.drain()
    head = await reader.readuntil(b'\r\n\r\n')
    lines = head.decode('ascii').split('\r\n')
    headers = {}
    for line in lines[1:]:
        if not line: continue
        name, value = line.lower().split(': ', 1)
        if name in headers: raise RuntimeError('duplicate header')
        headers[name] = value
    if lines[0].split()[:2] != ['HTTP/1.1', '200'] or 'transfer-encoding' in headers:
        raise RuntimeError('invalid response framing/status')
    if headers.get('content-length') != '3' or headers.get('connection') != 'keep-alive':
        raise RuntimeError('unexpected response length/connection')
    body = await reader.readexactly(3)
    if body != b'ok\n' or headers.get('x-guest-aot') != '1': raise RuntimeError('guest actor response invalid')
    identity = int(headers['x-actor-id'])
    return (time.perf_counter()-start)*1000, identity

async def measure(args, port):
    ready = asyncio.Event()
    epoch = deadline = 0.0
    elapsed = array('d')
    cycle_sum = 0.0
    completed_window = warm = connected = 0
    ids = set()
    failures = []
    async def client(index):
        nonlocal cycle_sum, completed_window, warm, connected
        writer = None
        try:
            reader, writer = await asyncio.wait_for(asyncio.open_connection('127.0.0.1', port), 10)
            connected += 1
            await ready.wait()
            while time.perf_counter() < deadline and not failures:
                started = time.perf_counter()
                latency, identity = await asyncio.wait_for(response(reader, writer), 10)
                ended = time.perf_counter()
                if not 1 <= identity <= args.actors: raise RuntimeError('unknown actor identity')
                ids.add(identity)
                if started >= epoch:
                    elapsed.append(latency)
                    cycle_sum += (ended-started)*1000
                    completed_window += ended <= deadline
                else: warm += 1
        except Exception as error:
            failures.append({'client': index, 'error': str(error)[:160], 'type': type(error).__name__})
        finally:
            if writer:
                writer.close()
                try: await writer.wait_closed()
                except Exception: pass
    clients = [asyncio.create_task(client(i)) for i in range(args.clients)]
    try:
        connect_deadline = time.perf_counter()+10
        while connected < args.clients and not failures:
            if time.perf_counter() >= connect_deadline: raise TimeoutError('connection setup')
            await asyncio.sleep(.01)
        epoch = time.perf_counter()+args.warmup
        deadline = epoch+args.seconds
        ready.set()
        next_report = epoch+30
        while not failures and any(not task.done() for task in clients):
            await asyncio.sleep(.25)
            if time.perf_counter() >= next_report:
                print(json.dumps({'measured_s': round(time.perf_counter()-epoch, 1), 'responses': len(elapsed), 'failures': len(failures)}), flush=True)
                next_report += 30
        await asyncio.gather(*clients)
    finally:
        for task in clients:
            if not task.done(): task.cancel()
        await asyncio.gather(*clients, return_exceptions=True)
    ordered = sorted(elapsed)
    return {'clients': args.clients, 'actors': args.actors, 'carriers': args.carriers,
            'warmup_s': args.warmup, 'measurement_s': args.seconds,
            'requests_started_in_window': len(elapsed), 'completed_in_window': completed_window,
            'completed_rps': round(completed_window/args.seconds, 2),
            'http_mean_ms': round(sum(elapsed)/len(elapsed), 6) if elapsed else None,
            'client_cycle_mean_ms': round(cycle_sum/len(elapsed), 6) if elapsed else None,
            'http_p50_ms': percentile(ordered, .5), 'http_p95_ms': percentile(ordered, .95),
            'http_p99_ms': percentile(ordered, .99), 'warmup_requests': warm,
            'actors_observed': len(ids), 'failures': failures, 'retries': 0,
            'connections_opened': connected,
            'semantics': 'compiled scalar Oreslang actor; native HTTP adapter; no guest await/cooperate, Spin module imports or OTel module lowering'}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--server', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--clients', type=int, default=100)
    p.add_argument('--actors', type=int, default=128)
    p.add_argument('--carriers', type=int, default=4)
    p.add_argument('--seconds', type=float, default=180)
    p.add_argument('--warmup', type=float, default=10)
    a = p.parse_args()
    if not 1 <= a.clients <= 1024 or not 1 <= a.actors <= 1024 or not 1 <= a.carriers <= 64 or not math.isfinite(a.seconds) or not math.isfinite(a.warmup) or a.seconds <= 0 or a.warmup < 0:
        p.error('invalid limits')
    if git('status', '--porcelain'): raise RuntimeError('commit source changes before measuring')
    if a.output.exists(): raise RuntimeError('output directory already exists')
    a.output.mkdir(parents=True)
    binary = a.server.resolve()
    receipt = json.loads((binary.parent/'build-receipt.json').read_text())
    if receipt['dirty'] or receipt['source_commit'] != git('rev-parse', 'HEAD'):
        raise RuntimeError('rebuild native server from this clean source commit')
    if receipt['binary_sha256'] != hashlib.sha256(binary.read_bytes()).hexdigest():
        raise RuntimeError('native binary differs from build receipt')
    for relative, expected in receipt['sources_sha256'].items():
        if hashlib.sha256((ROOT/relative).read_bytes()).hexdigest() != expected:
            raise RuntimeError('native binary source changed: '+relative)
    (a.output/'build-receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    metadata = {'source_commit': git('rev-parse', 'HEAD'), 'source_tree': git('rev-parse', 'HEAD^{tree}'),
                'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'platform': platform.platform(),
                'machine': platform.machine(), 'python': platform.python_version(),
                'command': [str(binary), '0', str(a.actors), str(a.carriers)]}
    (a.output/'metadata.json').write_text(json.dumps(metadata, indent=2)+'\n')
    with (a.output/'server.log').open('w+') as log:
        process = subprocess.Popen(metadata['command'], stdout=subprocess.PIPE, stderr=log, text=True)
        try:
            line = process.stdout.readline(); log.write(line); log.flush()
            if not line.startswith('LISTENING '): raise RuntimeError('server did not start: '+line)
            port = int(dict(item.split('=') for item in line.split()[1:])['port'])
            result = asyncio.run(measure(a, port))
            process.send_signal(signal.SIGTERM)
            output, _ = process.communicate(timeout=20)
            log.write(output); log.flush()
            if process.returncode: raise RuntimeError('server failed; see server.log')
            result['server_including_warmup'] = json.loads(output.strip())
            (a.output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
            print(json.dumps(result, indent=2))
            server = result['server_including_warmup']
            expected = result['requests_started_in_window']+result['warmup_requests']
            if result['failures'] or not result['requests_started_in_window'] or result['actors_observed'] != a.actors:
                raise RuntimeError('request success/reuse gate failed')
            if any(server[key] != expected for key in ('accepted', 'completed', 'scheduler_completed')):
                raise RuntimeError('HTTP/mailbox completion counts differ')
            if any(server[key] for key in ('bad_frames', 'transport_errors')):
                raise RuntimeError('transport failure')
        finally:
            if process.poll() is None: process.kill(); process.wait()

if __name__ == '__main__': main()
