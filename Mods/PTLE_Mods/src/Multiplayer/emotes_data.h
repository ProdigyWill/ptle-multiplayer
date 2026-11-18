// The emote catalogue : written by make-emotes.py from the animation notes (don't edit by hand).
// { id (chains), name, category, loops, favourite, intro clip, clip, ending clips, hold frames, the ending's first frame }
static const char* const EMOTE_CATEGORIES[] = { "Idles and moves", "Combat", "Items", "Water", "Climbing and levers", "Damage and death", "Raft", "Unused" };
static const int NUM_EMOTE_CATEGORIES = 8;

static const EmoteDef EMOTES[] = {
	{ "breakdance", "Breakdance", 0, true, true, 0x20C73979, 0xE62C6A30, { 0x0CCA1088, 0xC144927F } },
	{ "pickaxe-spin", "Pickaxe Spin", 2, true, true, 0x6F332AB9, 0xFD13180C, { 0x676B0F87, 0x3CE7C4E2 } },
	{ 0, "Slow Walk", 0, true, false, 0, 0x36B0BED2 },   // 000
	{ 0, "Walk", 0, true, false, 0, 0xE3897990 },   // 001
	{ 0, "Run", 0, true, false, 0, 0x23738677 },   // 002
	{ 0, "Slow Sneak", 0, true, false, 0, 0x9B3D083B },   // 003
	{ 0, "Sneak", 0, true, false, 0, 0x37FF881A },   // 004
	{ 0, "Torch Slow Sneak", 2, true, false, 0, 0xA9EE7DAD },   // 005
	{ 0, "Torch Sneak", 2, true, false, 0, 0x288F7CE9 },   // 006
	{ 0, "Torch Slow Walk", 2, true, false, 0, 0x46778332 },   // 007
	{ 0, "Torch Walk", 2, true, false, 0, 0x582BB1E1 },   // 008
	{ 0, "Torch Run", 2, true, false, 0, 0x6F738D97 },   // 009
	{ 0, "Heroic Dash", 0, false, false, 0, 0x4A4B6CEE },   // 010
	{ 0, "Swim", 3, true, false, 0, 0x69797DCF },   // 011
	{ 0, "Fast Swim", 3, true, false, 0, 0x791845D6 },   // 012
	{ 0, "Slow Sling + Walking", 2, true, false, 0, 0x1838A955 },   // 013
	{ 0, "Fast Sling + Walking", 2, true, false, 0, 0x7E004FEA },   // 014
	{ 0, "Walk Shield Forward", 2, true, false, 0, 0xCF38D546 },   // 015
	{ 0, "Walk Shield Up Angle", 2, true, false, 0, 0xC070A539 },   // 016
	{ 0, "Walk Shield Up", 2, true, false, 0, 0x400ED780 },   // 017
	{ 0, "Idle", 0, true, false, 0, 0xB23CB6D0 },   // 018
	{ 0, "Teleport Zen", 0, false, true, 0, 0x9279E598, { 0x00000000, 0x00000000 }, { 246.0F, 288.0F }, 0.0F },   // 019
	{ 0, "Running Man", 0, true, true, 0, 0xF11F8411 },   // 020
	{ 0, "Bug Buzz", 0, false, true, 0, 0xD19BA126 },   // 021
	{ 0, "Jig", 0, false, true, 0, 0x66CE67E6 },   // 022
	{ 0, "Tired", 7, false, false, 0, 0x3444F367 },   // 023
	{ 0, "What Was That", 0, false, false, 0, 0x7291E136 },   // 024
	{ 0, "Scratch Butt", 0, false, true, 0, 0x1BFA84D6 },   // 025
	{ 0, "Idle Ice", 0, true, false, 0, 0x535FB509 },   // 026
	{ 0, "Idle Torch", 2, true, false, 0, 0x4EC34716 },   // 027
	{ 0, "Slip On Ice", 0, false, false, 0, 0x362D16AA },   // 028
	{ 0, "Stop Run", 0, true, false, 0, 0x3AE65F52 },   // 029
	{ 0, "Stop Run 2", 0, true, false, 0, 0xCB5EBF15 },   // 030
	{ 0, "Jump Intro", 0, false, false, 0, 0x1D195A05 },   // 031
	{ 0, "Single Jump", 0, false, false, 0, 0xB1EBF730 },   // 032
	{ 0, "Double Jump", 0, false, false, 0, 0x7241E753 },   // 033
	{ 0, "Falling", 0, false, false, 0, 0x0AAB3BF0 },   // 034
	{ 0, "Landing", 0, false, false, 0, 0x03F4F3C6 },   // 035
	{ 0, "Climb Ledge + Roll", 4, false, false, 0, 0xF4E83D7A },   // 036
	{ 0, "Climb Ledge", 4, false, false, 0, 0x238C6487 },   // 037
	{ 0, "Quick Turn", 7, false, false, 0, 0x1BA34784 },   // 038
	{ 0, "Teeter", 4, false, false, 0, 0x69D16895 },   // 039
	{ 0, "Canteen Empty", 2, false, false, 0, 0x06A67843 },   // 040
	{ 0, "Grab Lever Right", 4, false, false, 0, 0xAE390C9D },   // 041
	{ 0, "Holding Lever Right", 4, true, false, 0, 0x7D874F82 },   // 042
	{ 0, "Flipping Lever To Left", 4, false, false, 0, 0x34AE2674 },   // 043
	{ 0, "Grab Lever Left", 4, false, false, 0, 0xC8F559D2 },   // 044
	{ 0, "Hold Lever Left", 4, true, false, 0, 0x1B4B1ACD },   // 045
	{ 0, "Flip Lever To Right", 4, false, false, 0, 0x5262733B },   // 046
	{ 0, "Canteen Drink Still", 2, false, false, 0, 0x3C19DA6C },   // 047
	{ 0, "Canteen Fill Still", 2, false, false, 0, 0x9A641C02 },   // 048
	{ 0, "Canteen Drink Loop", 2, true, false, 0, 0xAD7C8BFD },   // 049
	{ 0, "Canteen Fill Loop", 2, true, false, 0, 0x0B014D93 },   // 050
	{ 0, "Gas Mask On Face", 2, false, false, 0, 0x5DFB9A89 },   // 051
	{ 0, "Torch Up", 2, false, false, 0, 0x4E683D48 },   // 052
	{ 0, "Torch Down", 2, false, false, 0, 0xBA50A638 },   // 053
	{ 0, "Torch Left", 2, false, false, 0, 0xDCC8D16B },   // 054
	{ 0, "Torch Right", 2, false, false, 0, 0x160BAB50 },   // 055
	{ 0, "Slow Sling", 2, false, false, 0, 0xDB301B09 },   // 056
	{ 0, "Fast Sling", 2, false, false, 0, 0xCB512310 },   // 057
	{ 0, "Super Sling Shoot", 2, false, false, 0, 0xBED506C1 },   // 058
	{ 0, "Sling Intro", 2, false, false, 0, 0xA03BC44F },   // 059
	{ 0, "Underhand Forward", 7, false, false, 0, 0xD0D286D1 },   // 060
	{ 0, "Underhand Left", 7, false, false, 0, 0x027FC65B },   // 061
	{ 0, "Underhand Down", 7, false, false, 0, 0x15D6E004 },   // 062
	{ 0, "Underhand Right", 7, false, false, 0, 0x300C2CEB },   // 063
	{ 0, "Grab Ledge", 4, false, false, 0, 0xD321FC25 },   // 064
	{ 0, "Crouch", 0, false, false, 0, 0x4B4FF1B7 },   // 065
	{ 0, "Put Away Item Part 1", 2, false, false, 0, 0xB19C57C9 },   // 066
	{ 0, "Put Away Item Part 2", 2, false, false, 0, 0xF1B62B0B },   // 067
	{ 0, "Heroic Dive", 1, false, false, 0, 0xC21F9769 },   // 068
	{ 0, "Start Evil Sprint", 0, false, false, 0, 0x7D1CF33B },   // 069
	{ 0, "Hit 1", 5, false, false, 0, 0xDF4FD875 },   // 070
	{ 0, "Hit 2", 5, false, false, 0, 0x38EA3C20 },   // 071
	{ 0, "Hit 3", 5, false, false, 0, 0xA1E36D9A },   // 072
	{ 0, "Swim Shock", 7, true, false, 0, 0x997291CB },   // 073
	{ 0, "Turn Left", 7, false, false, 0, 0xAD633DF5 },   // 074
	{ 0, "Turn Right", 7, false, false, 0, 0x01CDBEFF },   // 075
	{ 0, "Croc Bite", 5, false, false, 0, 0x67406B27 },   // 076
	{ 0, "Push Left", 7, false, false, 0, 0x55A4533D },   // 077
	{ 0, "Push Right", 7, false, false, 0, 0x948A3313 },   // 078
	{ 0, "Attack 1", 1, false, false, 0, 0xD83A8A20 },   // 079
	{ 0, "Attack 2", 1, false, false, 0, 0x1554B97D },   // 080
	{ 0, "Attack 3", 1, false, false, 0, 0x8C5DE8C7 },   // 081
	{ 0, "Breakdance Intro", 0, false, false, 0, 0x20C73979 },   // 082
	{ 0, "Breakdance Loop", 0, true, true, 0, 0xE62C6A30 },   // 083
	{ 0, "Breakdance Outro", 0, false, false, 0, 0x0CCA1088 },   // 084
	{ 0, "Breakdance Finish", 0, false, true, 0, 0xC144927F, { 0x00000000, 0x00000000 }, { -1.0F, -1.0F }, 0.0F },   // 085
	{ 0, "Idle Sneak", 0, true, false, 0, 0xF6097AAE },   // 086
	{ 0, "Walk Intro", 0, true, false, 0, 0xB1D74FB6 },   // 087
	{ 0, "Rising Strike", 1, false, false, 0, 0x10EE8519 },   // 088
	{ 0, "Smash Strike", 1, false, false, 0, 0x8C23B867 },   // 089
	{ 0, "Spin Kick", 1, false, false, 0, 0x376F2E4A },   // 090
	{ 0, "Sweep Kick", 1, false, false, 0, 0x0D27DD0C },   // 091
	{ 0, "Put Away Item Full", 2, false, false, 0, 0xA012CBF6 },   // 092
	{ 0, "Un-Crouch", 0, false, false, 0, 0xE049E263 },   // 093
	{ 0, "Crouch 2", 0, false, false, 0, 0x0118EBDB },   // 094
	{ 0, "Sneak Intro", 7, true, false, 0, 0xB3D5E715 },   // 095
	{ 0, "Hit-Head + Fall", 5, false, false, 0, 0xDFE2CB37 },   // 096
	{ 0, "Slow Sling + Foot Moving", 2, false, false, 0, 0x46EF2628 },   // 097
	{ 0, "Fast Sling + Foot Moving", 2, false, false, 0, 0x783253F2 },   // 098
	{ 0, "Quicksand Walk", 0, true, false, 0, 0x0831B3BF },   // 099
	{ 0, "Idle Swim", 3, true, false, 0, 0x805AB907 },   // 100
	{ 0, "Dive Into Water", 3, false, false, 0, 0x5A9A222C },   // 101
	{ 0, "Hit Swim", 5, true, false, 0, 0x7783DFD5 },   // 102
	{ 0, "Hit Swim 2 (bugged)", 5, true, false, 0, 0x97B430DA },   // 103
	{ 0, "Death", 5, false, true, 0, 0xE65F150E, { 0xBF3F14A6, 0x00000000 }, { -1.0F, -1.0F }, 50.0F },   // 104
	{ 0, "Death 2", 5, false, true, 0, 0xB52A8757, { 0xBF3F14A6, 0x00000000 }, { -1.0F, -1.0F }, 50.0F },   // 105
	{ 0, "Fall Death", 5, false, false, 0, 0x50F2F5F7 },   // 106
	{ 0, "Holding Pickaxe While On Wall Left", 4, true, false, 0, 0x4AE33CA0 },   // 107
	{ 0, "Holding Pickaxe While On Wall Right", 4, true, false, 0, 0x1A2B9B85 },   // 108
	{ 0, "Use Pickaxe While On Wall Left Hand + Up", 4, false, false, 0, 0x4D4A4574 },   // 109
	{ 0, "Use Pickaxe While On Wall Right Hand + Up", 4, false, false, 0, 0x9B922431 },   // 110
	{ 0, "Use Pickaxe While On Wall Left Hand + Down", 4, false, false, 0, 0x73DE167A },   // 111
	{ 0, "Use Pickaxe While On Wall Right Hand + Down", 4, false, false, 0, 0x7C149D65 },   // 112
	{ 0, "Use Pickaxe While On Wall Left Hand + Left", 4, false, false, 0, 0x7CDADC0E },   // 113
	{ 0, "Use Pickaxe While On Wall Right Hand + Left", 4, false, false, 0, 0x2B732C8A },   // 114
	{ 0, "Use Pickaxe While On Wall Left Hand + Right", 4, false, false, 0, 0xEF249500 },   // 115
	{ 0, "Use Pickaxe While On Wall Right Hand + Right", 4, false, false, 0, 0xCC58FFC4 },   // 116
	{ 0, "Holding Vine", 4, true, false, 0, 0xC6B0E439 },   // 117
	{ 0, "Ascending Ladder", 4, false, false, 0, 0xB751636C },   // 118
	{ 0, "Descending Ladder", 4, false, false, 0, 0x05FF84CB },   // 119
	{ 0, "Holding Onto Ladder Left", 4, true, false, 0, 0xA52E4E53 },   // 120
	{ 0, "Holding Onto Ladder Right", 4, true, false, 0, 0x3E70F551 },   // 121
	{ 0, "Slipping On Ladder", 4, false, false, 0, 0x4E8F8B07 },   // 122
	{ 0, "Ladder Dismount Top", 4, false, false, 0, 0x5B46475B },   // 123
	{ 0, "Ladder Mount Top", 4, false, false, 0, 0xF2AC00BD },   // 124
	{ 0, "Caught By Croc", 5, false, false, 0, 0x3B7F367D },   // 125
	{ 0, "In Mud Croc Mouth", 5, false, false, 0, 0xFC720EC6 },   // 126
	{ 0, "Mud Croc Throw In Air", 5, false, false, 0, 0x23F1A6AB },   // 127
	{ 0, "Caught By Water Croc", 5, false, false, 0, 0x1B92FF4B },   // 128
	{ 0, "Failed Croc Escape", 5, false, false, 0, 0x35C6C66E },   // 129
	{ 0, "Escaping Croc Start", 5, false, false, 0, 0xF7550A69 },   // 130
	{ 0, "Escaping Croc Finish", 5, false, false, 0, 0x386603AB },   // 131
	{ 0, "Escape Croc Launch", 5, false, false, 0, 0xF31D765B },   // 132
	{ 0, "Damage Ascend Spin", 5, false, false, 0, 0x7D1E9838 },   // 133
	{ 0, "Right Arm Grab Anim", 0, false, false, 0, 0x662E2C58 },   // 134
	{ 0, "Ascending Vine", 4, false, false, 0, 0x7FDA2192 },   // 135
	{ 0, "Descending Vine", 4, false, false, 0, 0xBA4DE006 },   // 136
	{ 0, "Tnt Throw Intro", 2, false, false, 0, 0x83816086 },   // 137
	{ 0, "Tnt Throw Loop", 2, true, false, 0, 0x2CAB01CD },   // 138
	{ 0, "Tnt Throw Hard", 2, false, false, 0, 0x4CE103F1 },   // 139
	{ 0, "Thrown From Ground / Hot Coal Damage", 5, false, false, 0, 0x34F3B688 },   // 140
	{ 0, "Thrown From Beast", 5, false, false, 0, 0xEC6F1E72 },   // 141
	{ 0, "Falling Recovery", 5, false, false, 0, 0xBF3F14A6 },   // 142
	{ 0, "Arm Up", 0, false, false, 0, 0xADDB6EE0 },   // 143
	{ 0, "Arm Forward", 0, false, false, 0, 0xA7495DC4 },   // 144
	{ 0, "Arm Left", 0, false, false, 0, 0x7B65C234 },   // 145
	{ 0, "Arm Right", 0, false, false, 0, 0xED784A26 },   // 146
	{ 0, "Top Of Vine", 4, false, false, 0, 0xA10E1427 },   // 147
	{ 0, "Grabbing Top Of Vine", 4, false, false, 0, 0x8D18CC3B },   // 148
	{ 0, "Bottom Of Vine", 4, false, false, 0, 0x933876A5 },   // 149
	{ 0, "Grabbing Bottom Of Vine", 4, false, false, 0, 0xE961777F },   // 150
	{ 0, "Smash Strike 1", 1, false, false, 0, 0xD30B084D },   // 151
	{ 0, "Smash Strike 2", 1, false, false, 0, 0x05D6B391 },   // 152
	{ 0, "Pickaxe Spin Loop", 2, true, true, 0, 0xFD13180C },   // 153
	{ 0, "Pickaxe Spin Start", 2, false, false, 0, 0x6F332AB9 },   // 154
	{ 0, "Pickaxe Stop Dizzy Intro", 7, false, false, 0, 0x676B0F87 },   // 155
	{ 0, "Pickaxe Stop Dizzy Outro", 7, false, false, 0, 0x3CE7C4E2 },   // 156
	{ 0, "Hit With Pickaxe Right", 5, false, false, 0, 0x1582172B },   // 157
	{ 0, "Hit With Pickaxe Left", 5, false, false, 0, 0x5FFDF3DC },   // 158
	{ 0, "Ball", 0, false, false, 0, 0xB5F6602F },   // 159
	{ 0, "Standing Shield Forward", 2, false, false, 0, 0x28070662 },   // 160
	{ 0, "Standing Shield Up Angle", 2, false, false, 0, 0x2E4220CE },   // 161
	{ 0, "Standing Shield Up", 2, false, false, 0, 0xB68E0B11 },   // 162
	{ 0, "Standing Shield Up To Forward", 2, false, false, 0, 0x44F2F3DB },   // 163
	{ 0, "Standing Shield Forward To Up", 2, false, false, 0, 0xE30F1663 },   // 164
	{ 0, "Standing Shield Up To Forward + Backstep", 2, false, false, 0, 0x10A1EE46 },   // 165
	{ 0, "Raft", 6, false, false, 0, 0xD458574B },   // 166
	{ 0, "Raft Backward", 6, false, false, 0, 0xFF2EE4DC },   // 167
	{ 0, "Raft Backward Left", 6, false, false, 0, 0xC91430FC },   // 168
	{ 0, "Raft Backward Left/forward Right", 6, false, false, 0, 0x3AD36513 },   // 169
	{ 0, "Raft Backward Right", 6, false, false, 0, 0x0E041AAE },   // 170
	{ 0, "Raft Forward Right", 6, false, false, 0, 0x40B81A01 },   // 171
	{ 0, "Raft Forward Left/backward Right", 6, false, false, 0, 0x483349EA },   // 172
	{ 0, "Raft Forward Left", 6, false, false, 0, 0x7197CB1E },   // 173
	{ 0, "Raft Forward", 6, false, false, 0, 0xEE330D69 },   // 174
	{ 0, "Raft Lean Left", 6, false, false, 0, 0x4D7FE2B5 },   // 175
	{ 0, "Raft Lean Right", 6, false, false, 0, 0x77F1E3B0 },   // 176
	{ 0, "Raft Fall Intro", 6, false, false, 0, 0xDFA71D3B },   // 177
	{ 0, "Raft Fall Loop", 6, true, false, 0, 0xB9BCB876 },   // 178
	{ 0, "Raft Fall Land", 6, false, false, 0, 0xB0E37040 },   // 179
	{ 0, "Swimming Underwater", 3, false, false, 0, 0x04CF77FE },   // 180
	{ 0, "Underwater Idle", 3, true, false, 0, 0x265F8047 },   // 181
	{ 0, "Underwater Idle 2", 3, true, false, 0, 0x96B94878 },   // 182
	{ 0, "Underwater Hit", 5, false, false, 0, 0x188A0A05 },   // 183
	{ 0, "Dizzy Walk", 0, true, true, 0, 0xE203A1ED },   // 184
	{ 0, "Poison Gas Walk", 0, true, true, 0, 0x0945FC67 },   // 185
	{ 0, "Poison Gas Idle", 0, true, false, 0, 0x58F03327 },   // 186
	{ 0, "Bat Idle", 7, true, false, 0, 0x8A5DAF91 },   // 187
	{ 0, "Bat Walk", 7, true, false, 0, 0x9AA803D5 },   // 188
	{ 0, "Tnt Toss", 2, false, false, 0, 0x7D7DEDFE },   // 189
	{ 0, "Frozen Damage Rise", 5, false, false, 0, 0x24E52610 },   // 190
	{ 0, "Frozen Damage Fall", 5, false, false, 0, 0xCB55C46D },   // 191
	{ 0, "Frozen Damage Land", 5, false, false, 0, 0x9E4F5E29 },   // 192
	{ 0, "Deflate Raft", 6, false, false, 0, 0xBC0C5890 },   // 193
	{ 0, "Inflate Raft", 6, false, false, 0, 0x0307B6D7 },   // 194
	{ 0, "Backflip Start", 0, false, false, 0, 0x5C44A6C0 },   // 195
	{ 0, "Backflip Loop", 0, true, false, 0, 0xCABAA850 },   // 196
	{ 0, "Backflip Land", 0, false, false, 0, 0xC3E56066 },   // 197
	{ 0, "T-Pose", 0, true, true, 0, 0x4FFA75E5 },   // 198
	{ 0, "T-Pose Right Hand", 0, false, false, 0, 0xCE47AB9C },   // 199
	{ 0, "T-Pose Left Hand", 0, false, false, 0, 0x35C04395 },   // 200
	{ 0, "Equip Raft Middle", 6, false, false, 0, 0x61558A8A },   // 201
	{ 0, "Equip Raft Land", 6, false, false, 0, 0x328DFC93 },   // 202
	{ 0, "Sling Dodge Left", 2, false, false, 0, 0xE7707489 },   // 203
	{ 0, "Sling Dodge Right", 2, false, false, 0, 0x583490A1 },   // 204
	{ 0, "Shield Attack", 2, false, false, 0, 0xB1B2D47E },   // 205
	{ 0, "Shield Jump Start", 2, false, false, 0, 0xBB8FC12E },   // 206
	{ 0, "Shield Jump Fall", 2, false, false, 0, 0xA7E23AA0 },   // 207
	{ 0, "Shield Jump End", 2, false, false, 0, 0xAEBDF296 },   // 208
	{ 0, "Shield Block Forward", 2, false, false, 0, 0x0A747C30 },   // 209
	{ 0, "Shield Hit Forward", 5, false, false, 0, 0x5FA0B654 },   // 210
	{ 0, "Shield Hit Up", 5, false, false, 0, 0x3CCC72F1 },   // 211
	{ 0, "Shield Block Up", 2, false, false, 0, 0x0858434A },   // 212
	{ 0, "Equip Shield", 2, false, false, 0, 0x0314B9DC },   // 213
	{ 0, "Un-Equip Shield", 2, false, false, 0, 0xFB9E106A },   // 214
	{ 0, "Mama Monkey Hit", 5, false, false, 0, 0xC70B63E1 },   // 215
	{ 0, "Mama Monkey Hit Flying", 5, false, false, 0, 0x556C2DA6 },   // 216
	{ 0, "Mama Monkey Hit Land", 5, false, false, 0, 0x5C33E590 },   // 217
	{ 0, "Ran Over Intro", 5, false, false, 0, 0xD31F54FA },   // 218
	{ 0, "Ran Over Middle", 5, false, false, 0, 0xC3211B5C },   // 219
	{ 0, "Ran Over Getup", 5, false, false, 0, 0xDA076AAC },   // 220
	{ 0, "Sling Reload", 2, false, false, 0, 0x1A1C797E },   // 221
	{ 0, "Hit With Sling Out", 5, false, false, 0, 0x5C215103 },   // 222
	{ 0, "Hit With Raft Out", 6, false, false, 0, 0x4B0D552C },   // 223
	{ 0, "Swim Water Death", 5, true, false, 0, 0x98E1F9B3 },   // 224
	{ 0, "Swim Underwater Death", 5, true, false, 0, 0xEF6075FE },   // 225
	{ 0, "Victory Dance", 0, false, true, 0, 0x3FC6C574 },   // 226
	{ 0, "Quicksand Jump", 0, false, false, 0, 0x66BBB900 },   // 227
	{ 0, "Vine Swinging Bottom Max", 4, false, false, 0, 0xBA697583 },   // 228
	{ 0, "Vine Swinging Bottom Light", 4, false, false, 0, 0x096B7464 },   // 229
	{ 0, "Holding Bottom Vine", 4, true, false, 0, 0x24FEDF50 },   // 230
	{ 0, "Swinging Top Vine Max", 4, false, false, 0, 0x21FB2F6E },   // 231
	{ 0, "Swinging Top Vine Light", 4, false, false, 0, 0x4580E696 },   // 232
	{ 0, "Holding Middle Vine", 4, true, false, 0, 0xD2C45F69 },   // 233
	{ 0, "Holding Top Vine", 4, true, false, 0, 0xDC1FD75B },   // 234
};
static const int NUM_EMOTES = sizeof(EMOTES) / sizeof(EMOTES[0]);
