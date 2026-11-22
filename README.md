# PTLE Multiplayer

Online multiplayer for **Pitfall: The Lost Expedition** (PC). You see the other
players running around as Harry in real time, in any level, up to 8 people per
room. It's a beta, so expect some rough edges.

**[Download the latest release](https://github.com/ProdigyWill/ptle-multiplayer/releases)**

It's a mod for [Gizmod](https://github.com/DaveUltra/ptle/tree/master/Mods/PTLE_Mods),
DaveUltra's mod loader for the PC version. Big thanks to him: this repo started
as a copy of his.

## What it does

- Other players show up in your game with name tags, their animations, items
  (pickaxes, torch, sling, TNT, raft, shield...), rolls, jumps and climbs.
- Outfits and colors so you can tell each other apart, including Retro Harry
  with your own colors.
- An emote wheel with nearly all of Harry's animations: hold **L1 / LB** (or
  **T** on the keyboard) and pick one.
- A **hide and seek** mode: seekers catch hiders by touching them.
- An in-game menu on **F1** for all the settings.

## Install

1. Install **Gizmod 0.3.0** first, from
   [DaveUltra/ptle › Mods/PTLE_Mods](https://github.com/DaveUltra/ptle/tree/master/Mods/PTLE_Mods).
   This mod doesn't work with Gizmod 0.2.
2. Download the zip from the [releases](https://github.com/ProdigyWill/ptle-multiplayer/releases)
   and unzip it into the game's `Game` folder (the one with
   `PITFALL The Lost Expedition.exe`). `Multiplayer.asi` goes in `Game\mods` and
   `Multiplayer.ini` in `Game\config\Multiplayer`.
3. Open `Game\config\Multiplayer\Multiplayer.ini` in Notepad: set `Name` to your
   name and `Room` to the same room name as your friends.
4. Start the game. **F1** opens the multiplayer menu.

Updating? Replace only `mods\Multiplayer.asi` and keep your own
`Multiplayer.ini` (it has your name, room and emotes). What changed in each
version is in [CHANGELOG.md](Mods/PTLE_Mods/src/Multiplayer/CHANGELOG.md).

## Privacy and the relay server

- The players don't connect to each other. Everything goes through a relay
  server, which forwards each player's moves to the others in the same room.
- The `Server=` in the release's `Multiplayer.ini` is a relay I host. Like any
  server it sees the IP address you connect from, and it logs it with your
  player name and room when you join or leave.
- Other players never get your IP, only the relay does.
- The mod only connects to the server in `Multiplayer.ini`.
- Rooms have no passwords. Anyone who types the same room name ends up with
  you, so pick something unique.
- Your player name is shown to everyone in the room.

To run your own relay, you only need Python 3 (no extra packages):

```
python3 relay.py --port 27777
```

[`relay.py`](Mods/PTLE_Mods/src/Multiplayer/server/relay.py) is in
`Mods/PTLE_Mods/src/Multiplayer/server`. Open UDP port 27777 on that machine,
then put its address in everyone's `Multiplayer.ini`, e.g.
`Server=203.0.113.5:27777`.

## About AI

I used Claude (Anthropic's AI) a lot while writing the code. Eventually I want a
version of the mod where I can stand behind 100% of the code, but right now I
don't fully understand every line the AI wrote.

## Building from source

The mod is in [`Mods/PTLE_Mods/src/Multiplayer`](Mods/PTLE_Mods/src/Multiplayer).
Open `Mods/PTLE_Mods/PTLE_Mods.sln` in Visual Studio 2022 and build the
**Multiplayer** project, **Release | Win32** (toolset v143). The mod loader and
the other mods in that solution are Gizmod's.

Use a Release build to play. Debug builds include the automatic tests, and only
work with a Gizmod that is built in Debug too.

## The rest of this repo

Everything outside `Mods/PTLE_Mods/src/Multiplayer` comes from
[DaveUltra/ptle](https://github.com/DaveUltra/ptle): his research on the game
(`/Memory Research`: Ghidra project, Cheat Engine table, virtual tables), Gizmod
and his other mods (`/Mods/PTLE_Mods`). Go there for the latest of those.
