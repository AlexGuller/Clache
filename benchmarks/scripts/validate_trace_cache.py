#!/usr/bin/env python3
"""Check a matrix trace's identity without reading or modifying its payload."""
import json
import sys
from pathlib import Path


def unsigned(value, field):
    if isinstance(value, str) and value.isascii() and value.isdecimal():
        value = int(value)
    if type(value) is not int or not 0 <= value <= (1 << 64) - 1:
        raise ValueError(f"{field}: expected an unsigned 64-bit integer")
    return value


def validate(trace, workload, seed, requests, keys):
    trace = Path(trace)
    sidecar = Path(str(trace) + '.meta.json')
    if not trace.is_file():
        raise ValueError(f"trace is missing or is not a regular file: {trace}")
    try:
        meta = json.loads(sidecar.read_text())
    except (OSError, UnicodeError, ValueError) as error:
        raise ValueError(f"cannot read valid sidecar {sidecar}: {error}") from error
    if not isinstance(meta, dict) or not isinstance(meta.get('params'), dict):
        raise ValueError(f"{sidecar}: expected an object with a params object")
    expected = {
        'workload': workload,
        'seed': unsigned(seed, 'requested seed'),
        'request_count': unsigned(requests, 'requested requests'),
        'params.seed': unsigned(seed, 'requested seed'),
        'params.requests': unsigned(requests, 'requested requests'),
        'params.keys': unsigned(keys, 'requested keys'),
    }
    for field, wanted in expected.items():
        actual = meta['params'].get(field[7:]) if field.startswith('params.') else meta.get(field)
        if field != 'workload':
            actual = unsigned(actual, field)
        if actual != wanted:
            raise ValueError(f"{sidecar}: {field} mismatch: cached {actual!r}, requested {wanted!r}")


def main():
    if len(sys.argv) != 6:
        print('usage: validate_trace_cache.py TRACE WORKLOAD SEED REQUESTS KEYS', file=sys.stderr)
        return 2
    try:
        validate(*sys.argv[1:])
    except ValueError as error:
        print(f"error: refusing cached trace: {error}. Use a different TRACE_DIR or explicitly replace the stale trace and sidecar; existing data was not changed.", file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
