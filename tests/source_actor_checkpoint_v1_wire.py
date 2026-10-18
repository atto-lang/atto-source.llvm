#!/usr/bin/env python3
"""Independent source-actor scalar-state wire oracle.

This verifies the ORESAS01 payload format shared by Java evaluator snapshots
and the native host parity fixture. It is not executable-code authentication.
"""
import hashlib
import struct

MAGIC = b"ORESAS01"
MAX = 1_048_576

def require(ok, message):
    if not ok:
        raise AssertionError(message)

def encode(fields):
    out = bytearray(MAGIC)
    out += struct.pack(">I", len(fields))
    for name, kind, value in fields:
        raw_name = name.encode("utf-8")
        require(len(raw_name) <= 65535, "name too long")
        out += struct.pack(">H", len(raw_name)) + raw_name
        if kind == "int":
            out += b"\x01" + struct.pack(">q", value)
        elif kind == "bool":
            out += b"\x02" + (b"\x01" if value else b"\x00")
        elif kind == "string":
            raw = value.encode("utf-8")
            out += b"\x03" + struct.pack(">I", len(raw)) + raw
        else:
            raise ValueError("unsupported kind")
        require(len(out) <= MAX, "state too large")
    return bytes(out)

def decode(spec, data):
    require(len(data) <= MAX, "state too large")
    require(data.startswith(MAGIC), "bad magic")
    pos = len(MAGIC)
    require(pos + 4 <= len(data), "missing field count")
    count = struct.unpack_from(">I", data, pos)[0]
    pos += 4
    require(count == len(spec), "field count mismatch")
    result = []
    for wanted_name, wanted_kind in spec:
        require(pos + 2 <= len(data), "truncated name length")
        n = struct.unpack_from(">H", data, pos)[0]
        pos += 2
        require(pos + n <= len(data), "truncated name")
        raw_name = data[pos:pos+n]
        pos += n
        name = raw_name.decode("utf-8", errors="strict")
        require(name == wanted_name, "field name/order mismatch")
        require(pos < len(data), "missing tag")
        tag = data[pos]
        pos += 1
        if wanted_kind == "int":
            require(tag == 1 and pos + 8 <= len(data), "int tag/size mismatch")
            value = struct.unpack_from(">q", data, pos)[0]
            pos += 8
        elif wanted_kind == "bool":
            require(tag == 2 and pos < len(data), "bool tag/size mismatch")
            bit = data[pos]
            pos += 1
            require(bit in (0, 1), "noncanonical bool")
            value = bool(bit)
        elif wanted_kind == "string":
            require(tag == 3 and pos + 4 <= len(data), "string tag/size mismatch")
            n = struct.unpack_from(">I", data, pos)[0]
            pos += 4
            require(pos + n <= len(data), "truncated string")
            value = data[pos:pos+n].decode("utf-8", errors="strict")
            pos += n
        else:
            raise ValueError("unsupported spec")
        result.append((name, wanted_kind, value))
    require(pos == len(data), "trailing bytes")
    return result

def main():
    values = [("count", "int", 42), ("ready", "bool", True),
              ("name", "string", "worker-π")]
    spec = [(a,b) for a,b,_ in values]
    golden = encode(values)
    require(len(golden) == 57, "golden length drift")
    require(hashlib.sha256(golden).hexdigest()
            == "eab52d5bea5c2b873c925edf9f4d1905cded85daac3ca0f6b920b97da22d5b40",
            "golden SHA-256 drift")
    require(decode(spec, golden) == values, "golden roundtrip")

    checks = 3
    for value in (-2**63, -1, 0, 1, 2**63 - 1):
        case = [("count","int",value),("ready","bool",value >= 0),
                ("name","string","µ-🙂-" + str(value))]
        require(decode(spec, encode(case)) == case, "int/unicode roundtrip")
        checks += 1

    for index in range(len(golden)):
        changed = bytearray(golden)
        changed[index] ^= 1
        try:
            decode(spec, bytes(changed))
            raise AssertionError("accepted one-bit mutation at " + str(index))
        except (AssertionError, UnicodeError, ValueError) as failure:
            if isinstance(failure, AssertionError) and str(failure).startswith("accepted"):
                raise
        checks += 1

    for cut in range(len(golden)):
        try:
            decode(spec, golden[:cut])
            raise AssertionError("accepted truncation at " + str(cut))
        except (AssertionError, UnicodeError, ValueError) as failure:
            if isinstance(failure, AssertionError) and str(failure).startswith("accepted"):
                raise
        checks += 1

    try:
        decode(spec, golden + b"\x00")
        raise AssertionError("accepted trailing byte")
    except AssertionError as failure:
        if str(failure) == "accepted trailing byte":
            raise
    checks += 1
    print(f"PASS: {checks} ORESAS01 scalar-state wire checks")

if __name__ == "__main__":
    main()
