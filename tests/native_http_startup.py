"""Bounded listener announcement from the native HTTP test child (POSIX)."""
import os
import selectors
import time


def wait_for_listener(process, timeout=10.0):
    if timeout <= 0:
        raise ValueError('startup timeout must be positive')
    deadline = time.monotonic() + timeout
    line = bytearray()
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        while len(line) < 512:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not selector.select(remaining):
                raise TimeoutError('native HTTP listener announcement timed out')
            # Read only the announcement; leave subsequent metrics for communicate().
            byte = os.read(process.stdout.fileno(), 1)
            if not byte:
                raise RuntimeError('native HTTP child closed stdout before listener announcement')
            if byte == b'\n':
                break
            line.extend(byte)
        else:
            raise ValueError('native HTTP listener announcement exceeds 511 bytes')
    try:
        fields = line.decode('ascii').split()
        if not fields or fields[0] != 'LISTENING':
            raise ValueError('missing LISTENING marker')
        pairs = [field.split('=', 1) for field in fields[1:]]
        if any(len(pair) != 2 or not pair[0] for pair in pairs):
            raise ValueError('malformed listener field')
        values = dict(pairs)
        if len(values) != len(pairs):
            raise ValueError('duplicate listener field')
        port = values['port']
        if not port.isascii() or not port.isdecimal() or not 1 <= int(port) <= 65535:
            raise ValueError('invalid listener port')
        return int(port)
    except (KeyError, UnicodeError, ValueError) as error:
        raise ValueError('invalid native HTTP listener announcement') from error
