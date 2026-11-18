# Writes emotes_data.h, the emote catalogue, from an anim-notes file (David's names) :
#     python make-emotes.py anim-notes-david.txt
# Each of Harry's clips becomes an emote with a display name, a category and whether
# it loops; starred clips are the favourites. Evil Harry's clips are left out (they
# don't play on Harry). A few emotes are written by hand below (chains, names).
import re, sys, os

CATEGORIES = ["Idles and moves", "Combat", "Items", "Water", "Climbing and levers", "Damage and death", "Raft", "Unused"]

# Display names for some favourites.
NAMES = { 0x9279E598: "Teleport Zen", 0xD19BA126: "Bug Buzz", 0xE62C6A30: "Breakdance Loop", 0xFD13180C: "Pickaxe Spin Loop" }
# Loop these whatever their name says (the favourites that are dances).
LOOPS = { 0xFD13180C, 0xF11F8411, 0x9279E598, 0xE203A1ED, 0x0945FC67, 0x4FFA75E5 }
# Play these once whatever their name says (Jig, Bug Buzz).
ONCE = { 0x66CE67E6, 0xD19BA126 }
# Chains : (id, name, category, intro clip, looping clip, ending clips), favourites. The
# ending plays when you move (breakdance outro and finish; pickaxe stop dizzy intro and outro).
CHAINS = [ ("breakdance", "Breakdance", 0, 0x20C73979, 0xE62C6A30, (0x0CCA1088, 0xC144927F)),
           ("pickaxe-spin", "Pickaxe Spin", 2, 0x6F332AB9, 0xFD13180C, (0x676B0F87, 0x3CE7C4E2)) ]

def category( note ):
    n = note.lower()
    if "(unused)" in n: return 7
    if "raft" in n: return 6
    if re.search( r"death|hit|damage|thrown|ran over|croc|mama monkey|recovery|shock", n ): return 5
    if re.search( r"swim|water|underwater|dive into", n ): return 3
    if re.search( r"ladder|vine|ledge|on wall|lever|teeter", n ): return 4
    if re.search( r"torch|sling|canteen|gas mask|pickaxe|tnt|shield|item|underhand", n ): return 2
    if re.search( r"attack|strike|kick|heroic dive", n ): return 1
    return 0

def pretty( note ):
    n = note.replace( "(unused)", "" ).replace( "?", "" ).strip()
    words = []
    for w in n.split():
        words.append( "-".join( p[:1].upper() + p[1:] for p in w.split( "-" ) ) )
    return " ".join( words )

def loops( note ):
    return bool( re.search( r"\b(loop|walk|walking|run|idle|swim|sneak|holding|hold|treading)\b", note.lower() ) )

def main():
    src = sys.argv[1]
    rows = []
    for line in open( src, encoding="utf-8" ):
        m = re.match( r"(\d+) 0x([0-9A-Fa-f]{8}) (\S+)\s+(\*?)\s*(.*)", line.rstrip( "\n" ) )
        if not m or line.startswith( "#" ): continue
        num, crc, where, star, note = int( m.group( 1 ) ), int( m.group( 2 ), 16 ), m.group( 3 ), m.group( 4 ) == "*", m.group( 5 ).strip()
        if not where.startswith( "harry/" ): continue
        rows.append( (num, crc, star, note) )
    seen = {}
    out = []
    for cid, name, cat, intro, clip, ending in CHAINS:
        out.append( '\t{ "%s", "%s", %d, true, true, 0x%08X, 0x%08X, { 0x%08X, 0x%08X } },' % (cid, name, cat, intro, clip, ending[0], ending[1]) )
        seen[name.lower()] = 1
    for num, crc, star, note in rows:
        name = NAMES.get( crc ) or pretty( note ) or ("Clip %03d" % num)
        if name.lower() in seen:
            seen[name.lower()] += 1
            name = "%s %d" % (name, seen[name.lower()])
        else: seen[name.lower()] = 1
        loop = (crc in LOOPS or loops( note )) and crc not in ONCE
        out.append( '\t{ 0, "%s", %d, %s, %s, 0, 0x%08X },   // %03d' % (name.replace( '"', "'" ), category( note ), "true" if loop else "false", "true" if star else "false", crc, num) )
    here = os.path.dirname( os.path.abspath( __file__ ) )
    with open( os.path.join( here, "emotes_data.h" ), "w", encoding="utf-8", newline="\r\n" ) as f:
        f.write( "// The emote catalogue : written by make-emotes.py from the animation notes (don't edit by hand).\n" )
        f.write( "// { id (chains), name, category, loops, favourite, intro clip, clip, ending clips (chains) }\n" )
        f.write( "static const char* const EMOTE_CATEGORIES[] = { %s };\n" % ", ".join( '"%s"' % c for c in CATEGORIES ) )
        f.write( "static const int NUM_EMOTE_CATEGORIES = %d;\n\n" % len( CATEGORIES ) )
        f.write( "static const EmoteDef EMOTES[] = {\n" + "\n".join( out ) + "\n};\n" )
        f.write( "static const int NUM_EMOTES = sizeof(EMOTES) / sizeof(EMOTES[0]);\n" )
    print( "%d emotes" % len( out ) )

main()
