#!/usr/bin/env python3
"""Black-box native TCP -> mailbox -> compiled actor -> response ownership test."""
import argparse
import asyncio
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile

async def response(reader):
    raw = await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'), 10)
    lines = raw.decode('ascii').split('\r\n')
    status = int(lines[0].split()[1])
    headers = dict(line.lower().split(': ', 1) for line in lines[1:] if line)
    body = await asyncio.wait_for(reader.readexactly(int(headers['content-length'])), 10)
    return status, headers, body

async def exercise(port):
    identities = set()
    async def client():
        reader, writer = await asyncio.open_connection('127.0.0.1', port)
        try:
            for _ in range(20):
                writer.write(b'GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n')
                await writer.drain()
                status, h, body = await response(reader)
                if status != 200 or body != b'ok\n' or h.get('x-guest-aot') != '1':
                    raise AssertionError((status, h, body))
                identities.add(h['x-actor-id'])
        finally:
            writer.close(); await writer.wait_closed()
    await asyncio.gather(*(client() for _ in range(100)))
    if len(identities) != 128:
        raise AssertionError(('actors did not all receive requests', len(identities)))
    valid = [
        (b'GET /missing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n', 404),
        (b'POST /health HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nConnection: close\r\n\r\n', 405),
    ]
    malformed = [
        b'GET /health HTTP/1.1\r\n\r\n',
        b'GET /health HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n',
        b'GET /health HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n',
        b'GET /health HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx',
        b'GET /health HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n',
        b'GET /health HTTP/1.1\r\nHost : x\r\n\r\n',
        b'GET /health HTTP/1.0\r\nHost: x\r\n\r\n',
        b'GET /health HTTP/1.1\r\nHost: x\r\nX: ' + b'x'*8200 + b'\r\n\r\n',
    ]
    for request, expected in valid + [(request, 400) for request in malformed]:
        reader, writer = await asyncio.open_connection('127.0.0.1', port)
        writer.write(request); await writer.drain()
        status, h, body = await response(reader)
        if status != expected or body != str(expected).encode()+b'\n':
            raise AssertionError((status, h, body))
        if expected != 400 and h.get('x-guest-aot') != '1':
            raise AssertionError('valid 404/405 bypassed actor')
        if expected == 400 and 'x-guest-aot' in h:
            raise AssertionError('invalid framing was executed by guest')
        if await asyncio.wait_for(reader.read(), 10) != b'':
            raise AssertionError('connection not closed')
        writer.close(); await writer.wait_closed()
    # Header fragmentation and peer input half-close: response must still arrive.
    reader, writer = await asyncio.open_connection('127.0.0.1', port)
    for piece in (b'GET /health HTTP/1.1\r', b'\nHost: x\r\nConnection: close\r\n', b'\r\n'):
        writer.write(piece); await writer.drain(); await asyncio.sleep(.002)
    writer.write_eof()
    status, h, body = await response(reader)
    if status != 200 or h.get('x-guest-aot') != '1' or body != b'ok\n':
        raise AssertionError((status, h, body))
    writer.close(); await writer.wait_closed()
    return 2003, len(malformed)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryFile(mode='w+') as log:
        process = subprocess.Popen([str(args.server.resolve()), '0', '128', '4'], stdout=subprocess.PIPE, stderr=log, text=True)
        try:
            line = process.stdout.readline()
            if not line.startswith('LISTENING '): raise AssertionError(line)
            port = int(dict(item.split('=') for item in line.split()[1:])['port'])
            accepted, malformed = asyncio.run(exercise(port))
            process.send_signal(signal.SIGTERM)
            out, _ = process.communicate(timeout=20)
            log.seek(0); errors = log.read()
            if process.returncode != 0 or errors: raise AssertionError((process.returncode, errors))
            metrics = json.loads(out.strip())
            for key in ('accepted', 'completed', 'scheduler_completed'):
                if metrics[key] != accepted: raise AssertionError((key, metrics))
            if metrics['bad_frames'] != malformed or metrics['transport_errors'] != 0 or metrics['actors_used'] != 128:
                raise AssertionError(metrics)
            print('PASS compiled native HTTP actor ownership:', json.dumps(metrics, sort_keys=True))
        finally:
            if process.poll() is None: process.kill(); process.wait()

if __name__ == '__main__': main()
