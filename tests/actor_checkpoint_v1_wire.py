#!/usr/bin/env python3
"""Independent source-free checkpoint-v1 envelope oracle, NOT Oreslang guest execution."""
import hashlib
import hmac
import random
import struct
import uuid

MAGIC = b'ORESCP01'
HEADER = 168
MAX = 1_048_576
TRAILER = 32
ID = uuid.UUID('12345678-1234-4321-8123-000000000001')
DIGESTS = ('a' * 64, 'b' * 64, 'c' * 64, 'd' * 64)


def encode(actor_id, epoch, hashes, state):
    if not (isinstance(actor_id, uuid.UUID) and 0 < epoch < (1 << 64)):
        raise ValueError('bad actor/epoch')
    if len(state) > MAX:
        raise ValueError('state too large')
    if len(hashes) != 4 or any(len(h) != 64 or
        any(c not in '0123456789abcdef' for c in h) for h in hashes):
        raise ValueError('noncanonical digest')
    body = MAGIC + struct.pack('>I', 1) + actor_id.bytes + struct.pack('>Q', epoch)
    body += b''.join(bytes.fromhex(h) for h in hashes)
    body += struct.pack('>I', len(state)) + state
    if len(body) != HEADER + len(state):
        raise AssertionError('encoder size mismatch')
    return body + hashlib.sha256(body).digest()


def decode(frame, actor_id, epoch, hashes):
    if not (HEADER + TRAILER <= len(frame) <= HEADER + MAX + TRAILER):
        raise ValueError('bad frame length')
    if frame[:8] != MAGIC or struct.unpack_from('>I', frame, 8)[0] != 1:
        raise ValueError('bad magic/version')
    if frame[12:28] != actor_id.bytes or struct.unpack_from('>Q', frame, 28)[0] != epoch:
        raise ValueError('foreign identity or incarnation')
    expected = b''.join(bytes.fromhex(h) for h in hashes)
    if frame[36:164] != expected:
        raise ValueError('foreign code/state/wire/capability ABI')
    size = struct.unpack_from('>I', frame, 164)[0]
    if size > MAX or HEADER + size + TRAILER != len(frame):
        raise ValueError('invalid length')
    if not hmac.compare_digest(hashlib.sha256(frame[:-32]).digest(), frame[-32:]):
        raise ValueError('corrupt checksum')
    return frame[HEADER:-TRAILER]


def run_tests():
    rng = random.Random(0xC0DEC0DE)
    checks = 0
    for i in range(1024):
        data = rng.randbytes(rng.randrange(0, 500))
        frame = encode(ID, 42, DIGESTS, data)
        if decode(frame, ID, 42, DIGESTS) != data:
            raise AssertionError('round-trip mismatch')
        checks += 1
    golden = encode(ID, 42, DIGESTS, b'\x0a\xff')
    # Golden fixture is deliberately pinned for Java/C++ parity reviews.
    print('checkpoint_v1_golden_sha256=' + hashlib.sha256(golden).hexdigest())
    if len(golden) != 202:
        raise AssertionError('golden size mismatch')
    if decode(golden, ID, 42, DIGESTS) != b'\x0a\xff':
        raise AssertionError('golden content mismatch')
    checks += 2
    for i in range(len(golden)):
        changed = bytearray(golden)
        changed[i] ^= 1
        try:
            decode(changed, ID, 42, DIGESTS)
            raise AssertionError('accepted flipped bit at byte ' + str(i))
        except ValueError:
            checks += 1
    negatives = [golden[:-1], golden + b'\x00',
                 golden[:164] + b'\xff\xff\xff\xff' + golden[168:]]
    for frame in negatives:
        try:
            decode(frame, ID, 42, DIGESTS)
            raise AssertionError('accepted invalid-length envelope')
        except ValueError:
            checks += 1
    for ident, epoch, hashes in [
        (uuid.UUID(int=0), 42, DIGESTS), (ID, 43, DIGESTS),
        (ID, 42, ('f' * 64,) + DIGESTS[1:]),
        (ID, 42, DIGESTS[:3] + ('f' * 64,))]:
        try:
            decode(golden, ident, epoch, hashes)
            raise AssertionError('accepted foreign identity/epoch/ABI')
        except ValueError:
            checks += 1
    large = bytes(MAX)
    if len(decode(encode(ID, 42, DIGESTS, large), ID, 42, DIGESTS)) != MAX:
        raise AssertionError('max-size round-trip mismatch')
    checks += 1
    print('PASS: ' + str(checks) + ' independent wire checks (model-only)')


if __name__ == '__main__':
    run_tests()
