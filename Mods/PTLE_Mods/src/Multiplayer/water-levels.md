# Levels with water

Listed by William after the 2026-11-14 playtest. In these levels the water may
draw the scene a second time for its reflection, which is the suspected cause of
name tags flickering (Great Tree) or disappearing (Jungle Canyon): the mod may take
the reflection's camera for the main one.

The autotest visits all of them with `PTLE_MP_SURVEY=3` (`SURVEY_WATER` in main.cpp)
and logs a `TAGS` line per level: look-ats a frame, where the puppet was drawn, how
many tags were placed.

## What the survey found (2026-11-16)

- One camera look-at a frame in every level: the camera the mod captures is always
  the main view's, never the reflection's.
- The water's reflection is a 512x512 texture. The game draws the puppet into it
  and, in most levels, once more to the screen. In six levels it draws him only
  once, while the reflection is the target, and the screen shows that same draw:
  Bittenbinder's Camp, Cavern Lake, Copacanti Lake, Ekkeko Ice Cavern, Flooded
  Courtyard, Jungle Canyon.
- The tag used to come only from a draw to the screen: none in those six levels
  (Jungle Canyon's missing tags), and gaps in frames where the Great Tree skipped
  its screen draw (the flicker). Now every draw places it, with the main camera on
  the screen's own size: a tag every frame the puppet is drawn, in all of them.
- Viracocha Monoliths: the puppet wasn't drawn at all where the survey spawned it.

| Level | Code | CRC | Notes |
| --- | --- | --- | --- |
| Bittenbinder's Camp | m05 | 0x0EF63551 | One draw a frame, to the reflection |
| Butterfly Glade | b62 | 0xCD944049 | |
| Cavern Lake | s57 | 0x8B372E42 | One draw a frame, to the reflection |
| Copacanti Lake | s06 | 0x8147EA91 | One draw a frame, to the reflection |
| Ekkeko Ice Cavern | s60 | 0x3E7EE822 | One draw a frame, to the reflection |
| Flooded Courtyard | b03 | 0xECC9D759 | One draw a frame, to the reflection |
| Great Tree | a202 | 0x3292B6C9 | Tags flicker (playtest) |
| Jungle Canyon | a100 | 0xDEDA69BC | Tags disappear (playtest); one draw a frame, to the reflection |
| Ladder of Miles | save_a3 | 0x619E1126 | |
| Mama-Oullo Tower | a05 | 0x07ECCC35 | |
| Mountain Sled Run | s58 | 0x1B8833D3 | Water at the very end only |
| Penguin Den | s50 | 0x1553BBE1 | |
| Spinja Lair | s14 | 0x7652BAFC | Cold water (freezes Harry) |
| St. Claire's Excavation Camp (Day) | b63 | 0xBA9370DF | |
| St. Claire's Excavation Camp (Night) | b04 | 0x72AD42FA | |
| Twin Outposts | b52 | 0xE6B9138A | |
| Underground Dam | c06 | 0x9D6149E1 | |
| Viracocha Monoliths | s04 | 0x6F498BBD | Puppet not drawn in the survey |
| White Valley | s51 | 0x62548B77 | |
