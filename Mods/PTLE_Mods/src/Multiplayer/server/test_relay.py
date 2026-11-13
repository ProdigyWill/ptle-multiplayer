#!/usr/bin/env python3
"""Tests for relay.py : runs the relay in-process and talks to it over UDP.

    python test_relay.py
"""

import asyncio
import socket
import struct
import sys

import relay

HEADER = relay.HEADER
VERSION = 3
SNAPSHOT = 1


def join_packet(name, room, echo=False, nonce=1234):
    n, r = name.encode(), room.encode()
    return (HEADER.pack(relay.MAGIC, VERSION, relay.JOIN, 0, nonce)
            + bytes([relay.FLAG_ECHO if echo else 0, len(n)]) + n + bytes([len(r)]) + r)


def snapshot_packet(seq, payload=b'hello', spoofed_id=99):
    return HEADER.pack(relay.MAGIC, VERSION, SNAPSHOT, spoofed_id, seq) + payload


class Client:
    def __init__(self, port):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.setblocking(False)
        self.server = ('127.0.0.1', port)

    def send(self, data):
        self.sock.sendto(data, self.server)

    async def recv(self, timeout=1.0):
        loop = asyncio.get_running_loop()
        end = loop.time() + timeout
        while loop.time() < end:
            try:
                data, _ = self.sock.recvfrom(2048)
                return data
            except BlockingIOError:
                await asyncio.sleep(0.005)
        return None

    async def recv_all(self, timeout=0.3):
        out = []
        while True:
            d = await self.recv(timeout)
            if d is None:
                return out
            out.append(d)


failures = 0


def check(cond, what):
    global failures
    print(('PASS ' if cond else 'FAIL ') + what)
    if not cond:
        failures += 1


async def run():
    loop = asyncio.get_running_loop()
    transport, server = await loop.create_datagram_endpoint(relay.Relay, local_addr=('127.0.0.1', 0))
    port = transport.get_extra_info('sockname')[1]
    relay.TIMEOUT = 1.5
    reaper = asyncio.ensure_future(server.reap())

    a, b, c = Client(port), Client(port), Client(port)
    a.send(join_packet('alice', 'room1', nonce=111))
    w = await a.recv()
    check(w is not None and HEADER.unpack_from(w)[2] == relay.WELCOME, 'alice gets WELCOME')
    check(w is not None and HEADER.unpack_from(w)[4] == 111, 'WELCOME echoes the nonce')
    a_id = HEADER.unpack_from(w)[3]

    b.send(join_packet('bob', 'room1'))
    w = await b.recv()
    b_id = HEADER.unpack_from(w)[3]
    check(a_id != b_id, 'players get different ids (%d, %d)' % (a_id, b_id))

    c.send(join_packet('carol', 'room2'))
    await c.recv()

    a.send(snapshot_packet(5))
    got = await b.recv()
    check(got is not None and got[HEADER.size:] == b'hello', 'bob receives alice\'s snapshot')
    check(got is not None and got[4] == a_id, 'sender id stamped (not the spoofed 99)')
    check(await a.recv(0.2) is None, 'alice doesn\'t get her own snapshot back (no echo)')
    check(await c.recv(0.2) is None, 'carol (other room) gets nothing')

    # Joining again (lost WELCOME) keeps the same id.
    a.send(join_packet('alice', 'room1', nonce=222))
    w = await a.recv()
    check(HEADER.unpack_from(w)[3] == a_id and HEADER.unpack_from(w)[4] == 222, 'rejoin keeps the id')

    # Echo mode.
    e = Client(port)
    e.send(join_packet('echo', 'solo', echo=True))
    await e.recv()
    e.send(snapshot_packet(1))
    got = await e.recv()
    check(got is not None and got[HEADER.size:] == b'hello', 'echo player gets its own snapshot back')

    # Ping.
    a.send(HEADER.pack(relay.MAGIC, VERSION, relay.PING, 0, 77) + b'\x01\x02\x03\x04')
    got = await a.recv()
    check(got is not None and HEADER.unpack_from(got)[2] == relay.PONG and got[HEADER.size:] == b'\x01\x02\x03\x04', 'PING -> PONG')

    # Room full.
    fillers = []
    for i in range(relay.MAX_PLAYERS - 2):
        f = Client(port)
        f.send(join_packet('f%d' % i, 'room1'))
        await f.recv()
        fillers.append(f)
    x = Client(port)
    x.send(join_packet('extra', 'room1'))
    got = await x.recv()
    check(got is not None and HEADER.unpack_from(got)[2] == relay.REJECT, 'ninth player is rejected')

    # Other versions don't share the room.
    v = Client(port)
    v.send(HEADER.pack(relay.MAGIC, VERSION + 1, relay.JOIN, 0, 1) + b'\x00\x01v\x05room1')
    got = await v.recv()
    check(got is not None and HEADER.unpack_from(got)[2] == relay.WELCOME, 'another version gets its own room1')

    # Leave : the others hear about it.
    await b.recv_all(0.1)
    a.send(HEADER.pack(relay.MAGIC, VERSION, relay.LEAVE, 0, 0))
    got = await b.recv()
    check(got is not None and HEADER.unpack_from(got)[2] == relay.LEAVE and got[4] == a_id, 'bob is told alice left')

    # Timeout : everyone goes quiet but bob.
    for _ in range(5):
        b.send(snapshot_packet(9))
        await asyncio.sleep(0.5)
    in_room = {p.name for p in server.rooms.get((VERSION, 'room1'), {}).values()}
    check(in_room == {'bob'}, 'silent players time out (left : %s)' % sorted(in_room))

    reaper.cancel()
    transport.close()


asyncio.run(run())
print('%d failure(s)' % failures)
sys.exit(1 if failures else 0)
