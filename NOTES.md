# WildWaters PC: port notes

WildWaters (a.k.a. Extreme Kayak / X-Stream), Looking Glass Studios, N64, unreleased.
ROM: Jun 15 1999 prototype, Team CARROT dump, 16 MB, MD5 b867880c96c9e8d3130dde5526c95439,
XXH3 0x3EF9EA187C5BBEDE. Needs the Expansion Pak (8 MB). Not in Decomp Buddy's lists.

## How it is built
1. `tools/libsig.py` finds libultra functions by relocation-masked matches against the SDK
   libs (reference only) -> `recomp/libultra_matches.txt`; `recomp/names_manual.txt` adds
   hand-identified ones (cache/cop0 asm, printf family, SI/PI access queues...).
2. `tools/funcfind.py` splits .text into functions -> `recomp/ww.syms.toml`:
   `python tools/funcfind.py baserom.z64 80008000 1000 80045080 recomp/libultra_matches.txt,recomp/names_manual.txt recomp/ww.syms.toml 80092B40`
3. `N64Recomp wildwaters.toml` -> `RecompiledFuncs/`; `RSPRecomp recomp/aspMain.toml` -> audio ucode.
4. `build.cmd` (RT64 window build) or `build.cmd headless` (test build). MSVC + Ninja,
   uses N64ModernRuntime / rt64 / RecompFrontend from `Tools/N64 tools` (`N64_TOOLS`).
   RecompFrontend includes `../../../../../patches/ui_funcs.h`, so `Tools/patches/` holds a copy of `patches/`.

## Memory layout
- .text 0x80008000-0x80045080 (rom 0x1000), .data to 0x80092B40, .bss 0xDA860.
- RSP ucode linked inside .text (excluded from functions): rspboot 0x80014AD0,
  aspMain 0x80014BA0 (stock, == SDK), F3DEX2 2.07 0x800161B0 (data 0x80049740),
  S2DEX2 2.07 0x80017540 (data 0x80049B60). The game switches ucode with G_LOAD_UCODE (func_80031300).
- exceptasm 0x800243C0-0x800251A0 excluded (runtime replaces it).

## Gotchas fixed
- Functions that start with a `nop` (0x800395E8): the splitter skipped them as padding and
  N64Recomp made a 1-instruction static -> object pool never filled -> NULL crash on race load.
  Splitter now never skips past a call target; check `grep static_ RecompiledFuncs` is empty.
- 0x80026240 is osSetEventMesg (was misnamed) -> runtime SI events.
- osDpGetCounters isn't provided by the runtime: host returns idle counters with a non-zero
  clock (the HUD divides by it; x86 traps on /0).
- osCicId (0x80000310) set to 6102 in on_init.

## Asset directory
Table at 0x80090370 (0x20-byte entries, count at 0x80091B50 = 191), data base ROM 0x8E0C0.
Ids 0x13880.. textures, 0x9C40 / 0xA028 course environment packs, 0x111xx kayaker models,
0x186A0/0x186A1 big packs, 0x75xx sounds. Sound banks: 0x31AFB0, 0x50E2F0, 0x701630.

## Courses (func_80010820, course byte at 0x8015912F)
| course | env pack | water block | track-segment pack |
|---|---|---|---|
| 0 Devil's Elbow | 0xA028 (rom 0x16D7C4) | 0x713CC0: 34 x 0x28D0 records | 0x76A870: 34 segments |
| 1 Woodchuck Ridge | 0x9C40 (rom 0xC0B20) | 0x7B7F30: header says 30, only 29 present | 0x8047A0: MISSING |

Devil's Elbow plays in the port. Woodchuck Ridge fails on hardware too: from 0x801EC4 on, the ROM
holds F3DEX2 display lists with absolute RAM addresses (vertices in the env pack at its load
address 0x80753340), not the expected record 30 + segment pack (count, offsets, segments that
start with a float bbox and `cccc 00ff 00ff 00ff 0001`). The pack header read there gives a
133 MB size. Restoring it means rebuilding the water record and the segment pack.

## The 8 MB cut-off (root cause of the broken course)
The build is larger than 8 MB, but only ROM 0x000000-0x7FFFFF was written to the dev cart.
Woodchuck Ridge's water record 28 changes from water data to display lists exactly at 0x800000;
record 29 and the whole track-segment pack (0x8047A0) were never written. Everything from
0x800000 to 0xFFFFFF is stale data from earlier builds flashed to the same cart.

## Rebuilt Woodchuck Ridge (tools/rebuild_course2.py -> host/assets/restore/course2.patch)
Applied by host/src/restore.cpp to the in-memory ROM at the addresses the game reads (ROM file
untouched; WW_RESTORE=off disables). Formats, from the Ghidra decompile (re/decomp) + Devil's Elbow:
- Water tile 0x28D0: s32 link[4] (-x,+x,-z,+z), 50, 50, f32 xmin xmax zmin zmax level, 51x51x4
  grid [x][z]: u8 height (surface = level + 0.2*h), s8 flow x/y/z (*0.2). Query: func_80013C2C.
  Streaming: func_8000BC84 swaps neighbour tiles + segments near tile edges.
- Segment (= tile i): f32 bbox[6], u16 nodes/tris/verts, 0xCCCC, BVH nodes 14 B (u8 box quantised
  to bbox, u16 left/right, s16 tri0/tri1, leaf = no children), tris 3xu16, pad 4, verts 3xf32.
  Walk: func_80039694; collision use: func_8001122C.
- Collision = terrain mesh (textures 0x801F2xxx-0x801FFxxx) clipped to each tile (checked on Devil's
  Elbow: ~85% exact, last tiles 100%). The terrain comes from a display-list capture of a race
  (null renderer, WW_CAPTURE_OBJ=file.obj; tools/meshlib.py). The water mesh is not world-space.
- Rebuilt: all 30 segments; tile 28's lost 2/3 (surface/flow interpolated from tile 27 and the
  surviving columns, wet where the bed is under it); tile 29 = plunge pool (level = bed min + 2).
- Test: the scripted bot runs Woodchuck Ridge to tile 28/30 without crashes (it also gets stuck on
  Devil's Elbow at tile 16/34 with original data, so it can't judge the last stretch).
  WW_TRACE_POS=1 logs the kayak position; tools/run_test.ps1 for hidden window runs.

## Restored music (host/src/music.cpp, Sound > Music)
- The race asks for song asset 0x75A3 (single player) / 0x759E (multiplayer) in func_80036A98;
  all song assets point at the 1-channel stub 0x701AA0, so the prototype is silent.
- The unplayed songs are an older libmus format: nch, nwaves, vol_list, pbend_list, env, drum,
  data_list[nch] inline (no version/flags/wave table/master track). music.cpp converts them to the
  game's song_t (new 0x38 header, body moved after it, identity wave table, master track = Fstop)
  and writes the result over the stub in the song buffer (hook after the DMA in func_80031034).
- They use wave numbers up to 110: they belong to the 129-wave bank in the stale area (ptr 0xDCB230,
  "N64 WaveTables" at 0xDD1300), not the 38-wave effects bank this build loads into both slots.
  The header is copied to N64 0x80F00000 (below 16 MB: the aspMain DMA masks to 24 bits) and
  initialised with the game's MusPtrBankInitialize (func_80019ED4); a hook on MusPtrBankSetSingle
  (func_80019F10) swaps it in for the restored song.
- Mapping: Restored = this build's songs (Devil's Elbow 1, Woodchuck 4, multiplayer 2/3); Older
  Build = old songs 9/4/11/12; Off = prototype. Test switches: WW_MUSIC=off|old, WW_MUSIC_SONG=n,
  WW_MUSIC_BANK=80, WW_TRACE_MUSIC=1, WW_AUDIO_DUMP=file.wav. tools/render_songs.ps1 renders all 18.
- tools/musparse.py walks the song channels (command sizes from the SDK's player_commands.c).

## What the unused areas hold (raw extracts + LIST.md in ..\extracted)
- Music in this build: libmus (Software Creations "N64 PtrTablesV2"). fx bank 0x701630 (40 fx,
  38 waves). The game only ever starts the 1-channel stub at 0x701AA0 (asset ids 0x759E/0x75A3/
  0x75A8 all point there). Four full 24-channel songs (0x701C20, 0x707F80, 0x70B460, 0x70E320)
  and a 16-channel one (0x701B10) are never played. Song loader: func_80036BC8 -> func_80031034.
- Sound banks: the two used banks (0x31AFB0, 0x50E2F0) are byte-identical (38 waves). Unused
  older banks: 0xBF0170 (80 waves, ~1.9 MB), 0xDCB230 (129 waves, ~1.7 MB), partial copy 0xFEE170.
- 13 older-build songs, 24 channels: 0xF7F6A0 ... 0xFBB640 (list in extracted\LIST.md).
- 0x800000-0x89BEE0: prebaked F3DEX2 display lists with absolute RAM addresses and G_MODIFYVTX
  texture-coordinate animation (water drawing), older-build leftovers.
- 0x89BEE0-0x8CB2E0: older copy of kayaker textures/models (partly matches 0x217980-0x246D80).
- 0x8CB2E0-0xBF0170: mixed older data (copy of asset 0x112A8 at 0x8D81E0, object records with
  this build's code pointers at 0xBCD720). 0xBC4000-0xC02000 repeats at 0xFC2000-0x1000000.
- No older code, asset directory or collision-segment packs exist past 8 MB.
- The small gap 0x705C80-0x713CC0 is just the unplayed songs above.

## Testing (hidden, muted)
`tools/run_test.ps1 -Name x -Seconds 60 -Script "600:A,780:A,960:A,1140:A,1320:A,1500-3400:A" -Shots 24,36`
(course 1; add `800:SDOWN` before the second A for course 2). `-TraceDma` logs ROM reads.
The window opens off-screen with no focus or taskbar button; `tools/capture.ps1` grabs frames.

## Speed (frame pacing)
- Race physics is per game frame. func_80035A80 measures elapsed fields from osGetTime (count
  ticks * 64/3 = ns), clamps to 4 and falls back to 2 after a stall: the game expects 30 fps.
  The game clock at 0x8010B2E8 (ns) is real time; the HUD timer is MM:SS:milliseconds.
- host/src/frame_limit.cpp holds each display list to 1/30 s (renderer wrapper + headless);
  WW_FPS=n overrides (0 = off). Graphics > Framerate is set to Original once
  (RT64 interpolation matching on this scene costs ~1.7 s/frame).
- Diagnostics: WW_TRACE_GFX, WW_PROFILE_GFX, WW_TRACE_AUDIO, WW_TRACE_INPUT, WW_TRACE_POS.
- Hand-written 64-bit math (0x800140F0-0x80014A00): the divide entries pass a mode flag in HI
  into a shared core (0x800142C8); N64Recomp keeps HI per function, so funcfind.py's coverage
  pass makes each entry contain the code it branches/falls into (overlapping functions).
  WW_SELFTEST=1 checks divide/remainder/conversions at startup. The race timer is MM:SS:hundredths.

## Asset extraction (..\assets, open index.html)
- raw/: all 190 asset-directory entries + LIST.md. images/, fonts/: 0x03F3 images and 0x03F0 strips.
- Textures and models come from the headless build: WW_ASSET_DIR=<dir> (+ WW_ASSET_FRAMES=vi,...)
  runs host/src/asset_capture.cpp on every display list (textures by TMEM load, per-matrix objects,
  vertex colours unless G_LIGHTING). Woodchuck Ridge script: 600:A,800:SDOWN,960:A,1140:A,1320:A,1500:A,1700-3400:A.
- tools/assets_post.py (PNGs, ROM-offset match), tools/assets_models.py (models/), tools/render_obj.py
  (Blender previews), tools/vadpcm_banks.py (sounds/), tools/assets_index.py (index.html).
- Findings: all 112 asset-table textures are drawn; the whole course and sky box are one static
  object per course (sky = edges > 100 units); the river surface is the animated per-tile pieces.

## ROM 8-16 MB = Destruction Derby 64 leftovers (..\Destruction Derby 64 leftovers, index.html)
- The dev cartridge held a Destruction Derby 64 build before WildWaters was written over the first
  8 MB: menu art (Psygnosis logo, car select, bomb tag/capture the flag/deathmatch/destruction race,
  track logos), bank names ddsfx.wbk/ddmusic.wbk. So the 80/129-wave banks and the 13 "older build"
  songs are DD64's; WildWaters' own unplayed songs (0x701B10-0x713CC0) need ddmusic too.
- 0x89BEE0-0x8CB2E0 is identical to this build's data (fonts, kayaker). No DD64 code or F3DEX2
  model display lists survive. Tools: stale_census.py, stale_images.py (headerless picture carver:
  width by row-period dip, RGBA16/32 by pixel smoothness, end by per-row fit, seam re-alignment).

## Deep pass 2026-11-22 (everything identified in the ROM)
- DD64 leftovers: pictures cut exactly with DD64's own tables (font table 0xBCB6F8, picture table 0xBCBA40,
  data from 0x89A780/0x8CB3F0); 33 menu screens (0x03F1/0x03F0 records); music table 0xBDA1D0; track edge
  paths 0xBDA2B0+ rebuilt as road meshes. 0x800000-0x89A780 = DLs with runtime vertex buffers (not renderable).
- WildWaters assets 0x186A0/0x186A1 = river-surface meshes per course (func_80038DCC "REINITIALIZING WATER"):
  16-byte records (vertex ptr, tri ptr, counts); vertex 18 bytes (s16 xyz, rgba, foam, s, t); world = /32, y down.
  Woodchuck Ridge has 48 meshes reaching far past the captured terrain; its raw heights wrap past s16.
- 0xEA60 = sky dome (scale 10, 50 triangles, sky textures 0x138E5-0x138EF; header at the end of the pack).
- 0x111A2-0x111A5, 0x11369, 0x11497-0x11499 = WildWaters' own menu screens (title screen layout exists).
- Three kayaker models in the data segment (pointers at ROM 0x7AF28, material table 0x7A974, ids 0x0C0x/
  0x0C8x/0x0C4x) = the kayaker menu's KATANA, TOKOMO, TAMARA (all selectable). Format in tools/ww_kayakers.py; posed with animation frame 0.
- "MWo2" headers = file headers: "sfx" 0x8BB40, "graphics" 0x8BDF0 (kayak DLs), "sd" 0x8E0C0 (asset area).
- Effects bank 0x701630: effect n = one note on one wave (map in assets/sounds/LIST.md); triggers not traced.

## Restored events (host/src/restored_events.cpp, hook ww_game_tick on func_80036628)
- Attract demo: race mode 1 (0x8015912C, next-race value u16 0x8010B318) = title overlay (layout 0x11498:
  logo 0x10/0x14, "PRESS START" 0x13 blinking via func_8003ECA4, func_8003EB94/EBF0). Never set by the game,
  and mode 1 skips the kayak update + lacks the race screen's elements (crashes). Port: after 30 s idle on
  the first menu page it presses through the menus itself (A x5, SDOWN on the course screen every other
  demo; injected presses are not activity), then adds layout 0x11498 to the race screen (func_8003456C),
  marks Welcome/GO done (0x80159F44 = 2, starts clocks e08/e30) and runs EB94/EBF0. Any press or 60 s:
  back to the menus (func_8003CAAC sequence). func_8003493C (hide) assumes the element is visible.
- "YOU SURVIVED!" (element 0x15 of layout 0x11499): shown at 70% through the last water tile (tile found
  from position + tile bounds; obj+0x178 only follows the streamer), menus 6 s later. Test switches:
  WW_ATTRACT/WW_SURVIVED=off, WW_ATTRACT_IDLE=s, WW_FINISH_TILE=n, WW_FINISH_AT=0..1, WW_TRACE_EVENTS=1.
- Multiplayer = 2 players, horizontal split screen (players = menu choice + 1); works in the game. The
  player-2 kayaker pick only accepts controller 2. Port: General > Two-player mode switches RecompFrontend
  to multiplayer input (1-2 players, Controls > Assign players) while players are assigned or a menu is
  open; otherwise single-player input. Test script: "P2" in a step drives controller 2 (reported plugged in).

## Controllers
- assets/controllerdb.txt = the full SDL_GameControllerDB (2,233 mappings, merged from local copies by
  tools/merge_controllerdb.py) + the 48 N64 pad / adapter entries (tools/controllerdb_n64_entries.txt), last.
- frontend.cpp: SDL native drivers on for PS4/PS5, Switch Pro, Joy-Cons (pair = one pad), GameCube adapters,
  Steam, Stadia, Luna, Shield; Xbox via XInput/WGI. A pad in no database gets a generic layout
  (give_generic_mapping, also on hot-plug) so it works at once and can be rebound in Controls.

## Installer (build_setup.cmd -> dist\WildWaters PC Setup.exe, host/installer/setup.c)
- One exe: program files + VC++ runtime DLLs embedded (tools/make_payload.py -> payload.bin). The player
  picks their ROM (.z64/.v64/.n64, byte order fixed), checked with XXH3-64 0x3EF9EA187C5BBEDE (same as the
  port); stored as wildwaters.n64.proto.19990615.z64 in a portable install (portable.txt): settings, saves,
  mods in the install folder. Default %USERPROFILE%\Games\WildWaters PC; shortcuts; HKCU uninstall entry.
- Silent: /S /rom <file> /dir <dir> [/nodesktop] [/nostartmenu]; Uninstall.exe /uninstall [/S] [/delsaves].
  /snapshot <bmp> renders the window off-screen (dev check). Tested: .v64 install, wrong ROM, uninstall.
- Icons: nothing from the game is shipped. Setup cuts the program icon from the player's ROM at install
  (WildWaters logo, asset 0x11332, RGBA32 144x62 at ROM 0x274490) into the exe's resources + an .ico for
  the shortcuts; an update without a new ROM uses the stored copy. Setup's own icon is an original
  drawing (host/res/setup.ico).
