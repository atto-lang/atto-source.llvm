#!/usr/bin/env python3
"""Real native LLVM MessagePackWireV2 executable ↔ independent Python byte oracle.

This checks a trusted native pipe, not full native OresVM source actor interop.
"""
import argparse
import hashlib
import struct
import subprocess

p = argparse.ArgumentParser()
p.add_argument("--binary", required=True)
args = p.parse_args()

SHAPE = hashlib.sha256(b"oreslang.actor.shape.v2/T[I,I]").digest()
REQUEST = "oreslang.native_wasm.data.request.v0"
RESPONSE = "oreslang.native_wasm.data.response.v0"
ACK = "oreslang.native_wasm.data.ack.v0"

def integer(n):
    if 0 <= n <= 127: return bytes([n])
    if -32 <= n < 0: return bytes([n & 0xff])
    if 0 <= n <= 255: return b"\xcc" + bytes([n])
    if 0 <= n <= 65535: return b"\xcd" + struct.pack(">H", n)
    if 0 <= n <= 0xffffffff: return b"\xce" + struct.pack(">I", n)
    if n >= 0: return b"\xcf" + struct.pack(">Q", n)
    if n >= -128: return b"\xd0" + struct.pack(">b", n)
    if n >= -32768: return b"\xd1" + struct.pack(">h", n)
    if n >= -2147483648: return b"\xd2" + struct.pack(">i", n)
    return b"\xd3" + struct.pack(">q", n)

def encode(schema, ticket, value):
    name = schema.encode("ascii")
    if not 32 <= len(name) <= 255:
        raise ValueError("schema length")
    return b"\x94\x02\xd9" + bytes([len(name)]) + name + \
        b"\xc4\x20" + SHAPE + b"\x92" + integer(ticket) + integer(value)

def read_exact(stream, n):
    data = bytearray()
    while len(data) < n:
        chunk = stream.read(n-len(data))
        if not chunk:
            raise AssertionError("truncated native frame")
        data.extend(chunk)
    return bytes(data)

def read_frame(child):
    length = struct.unpack(">I", read_exact(child.stdout,4))[0]
    assert 0 < length <= 4096, length
    return read_exact(child.stdout,length)

def send_frame(child, data):
    child.stdin.write(struct.pack(">I",len(data))+data)
    child.stdin.flush()

with subprocess.Popen([args.binary, "3", "2"], stdin=subprocess.PIPE,
                      stdout=subprocess.PIPE, stderr=subprocess.PIPE) as child:
    for ticket, value in ((1,1),(2,1),(3,2)):
        assert read_frame(child) == encode(REQUEST, ticket, 1), ticket
        send_frame(child,encode(RESPONSE,ticket,value))
        assert read_frame(child) == encode(ACK,ticket,value+100), ticket
    child.stdin.close()
    assert child.wait(timeout=10) == 0, child.stderr.read().decode("utf8")

# Reject mismatched schema and replay: the native decoder enforces pinned
# schema IDs and the harness checks ticket monotonicity before ACK.
for schema, ticket, value in ((ACK,1,1),(RESPONSE,9,1)):
    with subprocess.Popen([args.binary,"1","1"],stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE,stderr=subprocess.PIPE) as child:
        assert read_frame(child) == encode(REQUEST,1,1)
        send_frame(child,encode(schema,ticket,value))
        child.stdin.close()
        assert child.wait(timeout=10)!=0
        assert b"NATIVE_WASM_WIRE_REJECTED" in child.stderr.read()

with subprocess.Popen([args.binary,"1","1"],stdin=subprocess.PIPE,
                      stdout=subprocess.PIPE,stderr=subprocess.PIPE) as child:
    read_frame(child)
    child.stdin.write(struct.pack(">I",4097))
    child.stdin.close()
    assert child.wait(timeout=10)!=0

print("PASS native C++ wire v2 / Python byte-identical request-response-ack and negatives")
