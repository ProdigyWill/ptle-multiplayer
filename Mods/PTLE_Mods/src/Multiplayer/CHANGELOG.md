# Multiplayer mod : changes

What's new in each build given to players. The version shows in the F1 menu's
title; players with another version are marked on its Players page.

## Unreleased (0.3.0)

## 0.2.0 (2026-11-17)

### New

- **Hide and seek** : F1 > **Gamemodes** > **Hide and seek**, and turn
  **Enabled** on (everyone who plays turns it on; one game mode at a time).
  - Everyone starts as a seeker; switch yourself to **Hider** and go hide.
  - Swap roles without the menu : hold **L1 / LB** and press left or right on
    the d-pad (keyboard : **Left Shift** + left or right arrow). No item comes
    out while you do. Both can be changed on the Keys page.
  - Your role and hide time show at the top left, and the page has a table of
    who's playing. Your hide time adds up while you're hidden (not while
    loading or paused) and is kept between games, until you use **Reset my
    timer**.
  - A seeker who touches a hider catches them : the hider's Harry dies, comes
    back at the last loadzone as a seeker, and everyone sees who caught whom.
  - Only seekers have name tags, and the player lists say "hiding" instead of
    where a hider is.
  - There's no head start or area limit in the game : agree on those among
    yourselves.
- **Emotes** : hold **L1 / LB**, point the right stick at an emote on the wheel
  and let go to play it (keyboard : hold **T**, point the mouse or the arrows,
  or press 1 - 8). **R1** (Tab) flips to your next wheel. Everyone sees you
  dance.
  - You need to be standing still with no item out to start one.
  - Loops keep going until you move or press anything (turning the camera
    doesn't count).
  - Breakdance and Pickaxe Spin play their ending when you do (press something
    again to skip it).
  - Some stay put until you move : Teleport Zen floats in meditation, Death and
    Death 2 lie there and then get up, Breakdance Finish keeps its pose.
  - While the wheel is out, the right stick (the arrows) doesn't move Harry's
    arm.
  - F1 > **Emotes** builds your wheels (up to 4 of 8) from all of Harry's
    animations, with a search, categories and a preview beside you (choose a
    slot to see its emote). Two wheels to start with : Party and Silly.
  - The buttons can be changed on the Keys page, including **needs R1 too**
    (L1 + R1) if L1 alone gets in your way.
- Messages ("Bob joined", "F1 : menu"...) now show at the bottom of the screen.
- Outfit **Default** : Harry wears what the area gives him, jungle or snowy
  clothes, as in the game. **Harry** now always means the jungle clothes, even
  in the snow. The list reads Default, Harry, Snowy Harry, Nicole, Native,
  Retro Harry.
- Retro Harry's colors : a **Color** row with a preview. Press Enter to type a
  hex code (#DC3228) or red, green, blue (220, 50, 40); Ctrl + V pastes one and
  Ctrl + C copies it. **Reset colors to default** puts back Retro Harry's own.
- Two sliders on the Display page choose where name tags **start fading** and
  where they're **gone** (100 and 200 to start with).
- The menu shows the mod's version, and you're told when someone in the room
  has a different one.

### Fixed

- Pitfalls no longer swallow other players.
- Other players face the way their Harry faces, not the way he moves (rafts
  too).
- Other players and enemies no longer fade away in the distance. Only name tags
  fade.
- Name tags show in every water level and stay steady behind walls, in every
  camera mode.
- No frozen copy of a player is left behind at loadzones, and nobody shows up
  in a level before they've really arrived.
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
