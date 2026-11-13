#!/usr/bin/env python3
"""PTLE multiplayer relay server.

Players join a room over UDP; every gameplay packet a player sends is forwarded to
the other players of the room (and back to the sender too in "echo" mode, for
testing alone). No port forwarding is needed on the players' side : they only
ever talk to this server.

Packets (see net/protocol.h) start with :
    u16 magic 'PM', u8 version, u8 type, u8 player id, u32 sequence number.

Control packets :
    JOIN    (2) client -> server : u8 flags (1 : echo), u8 name length, name,
                                   u8 room length, room. Sequence : a nonce.
    WELCOME (3) server -> client : player id in the header, u8 max players.
                                   Sequence : the JOIN's nonce.
    REJECT  (4) server -> client : u8 reason (1 : room full).
    LEAVE   (5) client -> server : leaving. Server -> client : that player left.
    PING    (6) client -> server : any payload, sent back as is in a PONG (7).
Everything else (snapshots, game modes) is forwarded, with the sender's player id
written into the header. Players silent for TIMEOUT seconds are dropped.

    python3 relay.py [--port 27777] [--host 0.0.0.0]
"""

import argparse
import asyncio
import logging
import struct
import time

MAGIC = 0x4D50
HEADER = struct.Struct('<HBBBI')   # magic, version, type, player, sequence.

JOIN, WELCOME, REJECT, LEAVE, PING, PONG = 2, 3, 4, 5, 6, 7
CONTROL = {JOIN, WELCOME, REJECT, LEAVE, PING, PONG}
REJECT_ROOM_FULL = 1
FLAG_ECHO = 1

MAX_PLAYERS = 8
MAX_PACKET = 1400
TIMEOUT = 10.0
MAX_PACKETS_PER_SECOND = 150   # Snapshots come at the game's tick rate (60).

log = logging.getLogger('relay')


class Player:
    def __init__(self, addr, room, pid, name, echo):
        self.addr = addr
        self.room = room
        self.id = pid
        self.name = name
        self.echo = echo
        self.last_seen = time.monotonic()
        self.window_start = self.last_seen
        self.window_count = 0


class Relay(asyncio.DatagramProtocol):
    def __init__(self):
        self.transport = None
        self.players = {}   # addr -> Player.
        self.rooms = {}     # (version, room name) -> {player id: Player}.

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        if len(data) < HEADER.size or len(data) > MAX_PACKET:
            return
        magic, version, ptype, _, seq = HEADER.unpack_from(data)
        if magic != MAGIC:
            return
        player = self.players.get(addr)
        if player:
            now = time.monotonic()
            player.last_seen = now
            if now - player.window_start >= 1.0:
                player.window_start, player.window_count = now, 0
            player.window_count += 1
            if player.window_count > MAX_PACKETS_PER_SECOND:
                return

        if ptype == JOIN:
            self.on_join(data, addr, version, seq)
        elif ptype == PING:
            self.send(addr, version, PONG, player.id if player else 0, seq, data[HEADER.size:])
        elif ptype == LEAVE:
            if player:
                self.remove(player, 'left')
        elif ptype not in CONTROL and player and player.room[0] == version:
            # Forward, with the sender's id stamped in.
            out = bytearray(data)
            out[4] = player.id
            for other in self.rooms[player.room].values():
                if other is not player or player.echo:
                    self.transport.sendto(out, other.addr)

    def on_join(self, data, addr, version, nonce):
        try:
            p = HEADER.size
            flags = data[p]
            n = data[p + 1]
            name = data[p + 2:p + 2 + n].decode('utf-8', 'replace')
            p += 2 + n
            r = data[p]
            room_name = data[p + 1:p + 1 + r].decode('utf-8', 'replace') or 'default'
        except IndexError:
            return
        room_key = (version, room_name)

        player = self.players.get(addr)
        if player and player.room != room_key:
            self.remove(player, 'changed room')
            player = None
        if not player:
            room = self.rooms.setdefault(room_key, {})
            free = [i for i in range(MAX_PLAYERS) if i not in room]
            if not free:
                self.send(addr, version, REJECT, 0, nonce, bytes([REJECT_ROOM_FULL]))
                return
            player = Player(addr, room_key, free[0], name, bool(flags & FLAG_ECHO))
            room[player.id] = player
            self.players[addr] = player
            log.info('%s:%d "%s" joined room "%s" (v%d) as player %d%s (%d in room)',
                     addr[0], addr[1], name, room_name, version, player.id,
                     ', echo' if player.echo else '', len(room))
        # (Again if the WELCOME was lost : the client keeps sending JOIN until it gets one.)
        self.send(addr, version, WELCOME, player.id, nonce, bytes([MAX_PLAYERS]))

    def send(self, addr, version, ptype, pid, seq, payload=b''):
        self.transport.sendto(HEADER.pack(MAGIC, version, ptype, pid, seq) + payload, addr)

    def remove(self, player, why):
        room = self.rooms.get(player.room, {})
        room.pop(player.id, None)
        self.players.pop(player.addr, None)
        log.info('%s:%d "%s" (player %d) %s, room "%s" has %d', player.addr[0], player.addr[1],
                 player.name, player.id, why, player.room[1], len(room))
        for other in room.values():
            self.send(other.addr, player.room[0], LEAVE, player.id, 0)
        if not room:
            self.rooms.pop(player.room, None)

    async def reap(self):
        while True:
            await asyncio.sleep(1.0)
            now = time.monotonic()
            for player in [p for p in self.players.values() if now - p.last_seen > TIMEOUT]:
                self.remove(player, 'timed out')


async def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--host', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=27777)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(message)s')

    loop = asyncio.get_running_loop()
    _, relay = await loop.create_datagram_endpoint(Relay, local_addr=(args.host, args.port))
    log.info('Relay listening on %s:%d (UDP).', args.host, args.port)
    await relay.reap()


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
