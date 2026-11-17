# Multiplayer mod : changes

What's new in each build given to players. The version shows in the F1 menu's
title; players with another version are marked on its Players page.

## Unreleased (0.2.0)

### New
- Outfit **Default** : Harry wears what the area gives him, jungle or snowy
  clothes, as in the game. **Harry** now always means the jungle clothes, even in
  the snow. The list reads Default, Harry, Snowy Harry, Nicole, Native, Retro Harry.
- Retro Harry's colors : a **Color** row with a preview. Press Enter to type a hex
  code (#DC3228) or red, green, blue (220, 50, 40); Ctrl + V pastes one and
  Ctrl + C copies it. **Reset colors to default** puts back Retro Harry's own.
- Two sliders on the Display page choose where name tags **start fading** and
  where they're **gone** (100 and 200 to start with).
- The menu shows the mod's version, and you're told when someone in the room has
  a different one.

### Fixed
- Pitfalls no longer swallow other players.
- Other players face the way their Harry faces, not the way he moves (rafts too).
- Other players and enemies no longer fade away in the distance. Only name tags
  fade.
- Name tags show in every water level and stay steady behind walls, in every
  camera mode.
- No frozen copy of a player is left behind at loadzones, and nobody shows up in
  a level before they've really arrived.
- Players caught by a pitfall come back; rafts stay visible.
- Other players no longer turn into a block of ice.
- The menu's bottom hints are centred and no longer cut off.

### Changed
- Retro Harry's parts are now Hair, Head, Shirt and Pants. Settings saved by
  older versions aren't read : custom Head and Pants colors go back to the
  default, and the outfit goes back to Default.

## 0.1.0 (2026-11-14)

The first build given to players.

- Up to 8 players in a room, through the relay server. Each player is shown in
  the others' games with their animations, items (pickaxes, torch, sling, TNT,
  raft, shield, gas mask, canteen), rolls, jumps and climbs.
- Name tags over other players, joined / left messages and a player list.
- The in-game menu (F1) : players, appearance and display settings, keys.
- Outfits, one per player : Harry, Snowy Harry, Nicole, Native, and Retro Harry
  with your own colors.
- Other players stay through Harry's deaths and level changes.
- Crash logs, to send when something goes wrong.
