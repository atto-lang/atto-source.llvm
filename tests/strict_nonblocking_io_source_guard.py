#!/usr/bin/env python3
"""Source guard for the native nonblocking TCP/readiness substrate."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
path = root / "src/main/cpp/oreslang/runtime/NonblockingTcpLinux.hpp"
text = path.read_text(encoding="utf-8")

required = {
    "socket_nonblock": r"SOCK_STREAM\s*\|\s*SOCK_NONBLOCK\s*\|\s*SOCK_CLOEXEC",
    "recv_dontwait": r"::recv\([^;]+MSG_DONTWAIT\)",
    "send_dontwait": r"::send\([^;]+MSG_DONTWAIT\s*\|\s*MSG_NOSIGNAL\)",
    "accept4_nonblock": r"::accept4\([^;]+SOCK_NONBLOCK\s*\|\s*SOCK_CLOEXEC\)",
    "eventfd_nonblock": r"eventfd\(0,\s*EFD_NONBLOCK\s*\|\s*EFD_CLOEXEC\)",
    "one_shot_readiness": r"EPOLLONESHOT",
}
missing = [name for name, pattern in required.items()
           if re.search(pattern, text, flags=re.S) is None]
if missing:
    raise SystemExit("DENIED: native nonblocking I/O invariants missing: " + ", ".join(missing))

forbidden = {
    "blocking_accept": r"::accept\s*\(",
    "blocking_recv_flags_zero": r"::recv\([^;]+,\s*0\s*\)",
    "blocking_send_flags_zero": r"::send\([^;]+,\s*0\s*\)",
}
found = [name for name, pattern in forbidden.items()
         if re.search(pattern, text, flags=re.S)]
if found:
    raise SystemExit("DENIED: blocking socket primitive introduced: " + ", ".join(found))

print("PASS: Linux TCP substrate remains nonblocking/readiness-driven")
