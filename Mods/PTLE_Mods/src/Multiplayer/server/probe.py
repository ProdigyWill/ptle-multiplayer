#!/usr/bin/env python3
"""Checks a relay server from the outside : joins an echo room, measures the round
trip and how many 1 KB packets come back.

    python probe.py [host[:port]] [--count 300]
"""

import argparse
import os
import socket
import struct
import time

MAGIC, VERSION = 0x4D50, 3
HEADER = struct.Struct('<HBBBI')
SNAPSHOT, JOIN, WELCOME, PING, PONG, LEAVE = 1, 2, 3, 6, 7, 5


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('server', nargs='?', default='ribbitrally.com:27777')
    parser.add_argument('--count', type=int, default=300)
    args = parser.parse_args()
    host, _, port = args.server.partition(':')
    addr = (socket.gethostbyname(host), int(port or 27777))

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    nonce = int.from_bytes(os.urandom(4), 'little')
    room = b'probe-%d' % os.getpid()
    join = HEADER.pack(MAGIC, VERSION, JOIN, 0, nonce) + bytes([1, 5]) + b'probe' + bytes([len(room)]) + room

    for attempt in range(5):
        s.sendto(join, addr)
        try:
            data, _ = s.recvfrom(2048)
            if HEADER.unpack_from(data)[2] == WELCOME:
                print('Joined %s:%d as player %d.' % (addr[0], addr[1], data[4]))
                break
        except socket.timeout:
            print('No answer to JOIN (attempt %d).' % (attempt + 1))
    else:
        print('FAIL : the server never answered (port blocked?).')
        return 1

    rtts = []
    for i in range(10):
        t = time.perf_counter()
        s.sendto(HEADER.pack(MAGIC, VERSION, PING, 0, i) + b'abcd', addr)
        try:
            data, _ = s.recvfrom(2048)
            if HEADER.unpack_from(data)[2] == PONG:
                rtts.append((time.perf_counter() - t) * 1000)
        except socket.timeout:
            pass
    if rtts:
        print('Ping : %d/10 answered, min %.0f ms, avg %.0f ms, max %.0f ms.' % (len(rtts), min(rtts), sum(rtts) / len(rtts), max(rtts)))

    # A burst at the game's rate (60/s) of snapshot-sized packets, echoed back.
    s.settimeout(0.0)
    payload = os.urandom(1050)
    got = 0
    start = time.perf_counter()
    for i in range(args.count):
        s.sendto(HEADER.pack(MAGIC, VERSION, SNAPSHOT, 0, i) + payload, addr)
        next_send = start + (i + 1) / 60.0
        while time.perf_counter() < next_send:
            try:
                data, _ = s.recvfrom(2048)
                if HEADER.unpack_from(data)[2] == SNAPSHOT:
                    got += 1
            except (BlockingIOError, socket.error):
                time.sleep(0.001)
    end = time.perf_counter() + 1.0
    while time.perf_counter() < end:
        try:
            data, _ = s.recvfrom(2048)
            if HEADER.unpack_from(data)[2] == SNAPSHOT:
                got += 1
        except (BlockingIOError, socket.error):
            time.sleep(0.001)
    print('Echo : %d of %d packets (1059 bytes, 60/s) came back (%.1f%% lost).' % (got, args.count, 100.0 * (args.count - got) / args.count))
    s.sendto(HEADER.pack(MAGIC, VERSION, LEAVE, 0, 0), addr)
    return 0 if got >= args.count * 0.95 else 1


if __name__ == '__main__':
    raise SystemExit(main())
