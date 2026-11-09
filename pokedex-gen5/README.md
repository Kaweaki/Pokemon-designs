# pokedex-gen5 — all 649, each with its signature move

A LilyGo T‑Display S3 (ESP32‑S3, 320×170 ST7789 on an 8‑bit parallel bus) showing
every Pokémon up to Genesect, **six seconds of action each**, in a painted Gen 5
battle scene chosen by its type. Every entry steps into the player position and
performs **one real signature move** — chosen from its actual Gen 5 level-up
list and fired across the field at an opposing Ditto (the twelve species whose
whole level-up list the sketch cannot draw get a type-pool stand-in). Charizard
breathes Flamethrower, Gyarados fires Hyper Beam, Pikachu throws Thunderbolt. Worlds
**switch** at the entry boundary; this BLE build uses a hard cut rather than the
dissolve so RainMaker can have the RAM. Ditto has **100,000 HP**, spent over
exactly 649 registered hits — each sized by what that move really does to a
level-50 Ditto, so Hyper Beam bites and Sing does nothing; on the final hit the
real Ditto sprite squashes flat and the next entry drops a fresh Ditto onto the
platform.

The Gen 5 sprites are the archive's own 96×96 art, unchanged in colour and shape.
Ditto is the actual archive sprite (#132), not a drawn blob. The battle
backgrounds and move effects are baked from the EBDX battle art in
`art/Battle&animations/EBDX.zip`. Nothing is painted by hand at runtime.

The shared method — geometry, the blit, the traps, the measured budget and how to
look at the panel without a camera — lives in [`../pokemon_skill.md`](../pokemon_skill.md).

## Flash the ready-made image

`releases/pokedex-gen5-full.bin` is everything (bootloader, partitions, app).
Write it at **`0x0`**:

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 921600 \
  write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 releases/pokedex-gen5-full.bin
```

See [`releases/README.md`](releases/README.md) for the checksum and board notes.

## Build from source

```bash
./build.sh upload                      # compile + flash (PORT=/dev/ttyACM1 to switch board)
python3 tools/verify.py                # sweep all 649, then print the frame budget
python3 tools/verify.py /dev/ttyACM0 --shots shots 6 25 130 649
./tools/make_release.sh                # rebuild the one-image release
```

`tools/arduino-cli` is the pinned CLI; the FQBN is fixed at
`esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=disabled,PartitionScheme=rainmaker_4MB`.
PSRAM is off on purpose: the two boards here have different PSRAM types and
`PSRAM=opi` hangs the quad one before the app runs. Nothing needs it.

## What is in the folder

| Path | What it is |
|---|---|
| `pokedex/pokedex.ino` | the sketch: scenes, motion, the signature-move engine, HUD, serial commands |
| `pokedex/scene.h` | `AtkFam`, `MoveDef` and the other attack/types |
| `pokedex/dex_data.h` | **generated** — 649 names and types |
| `pokedex/sprites.h` / `.S` | **generated** — 649 `SprInfo` records and 1,021 KB of Gen 5 pixels |
| `pokedex/bg_art.h` / `.S` | **generated** — 10 painted battle scenes (320×138, 862 KB) |
| `pokedex/atk_art.h` / `.S` | **generated** — 38 move textures as 4 bpp with palettes (24 KB) |
| `pokedex/signature_moves.h` | **generated** — per species: real move, type, power, Gen 5 damage, HP share |
| `pokedex/logo.h`, `panel.h` | Pokéball and ST7789/panel definitions |
| `art/gen5/0001..0649.png` | the source sprites, so the build is reproducible offline |
| `art/signature-moves.json` | **vendored** — species → real Gen 5 move, its numbers and base stats |
| `art/Battle&animations/EBDX.zip` | source battlebacks, scenery and move-effect sheets |
| `releases/pokedex-gen5-full.bin` | the flashable image |
| `tools/make_battle_art.py` | EBDX archive → `bg_art.h/.S` + `atk_art.h/.S`; `--preview` writes a sheet |
| `tools/fetch_signatures.py` | PokéAPI (Gen 5 level-up, move power, base stats) + hand-picked overrides → `art/signature-moves.json` |
| `tools/make_signature.py` | JSON → `pokedex/signature_moves.h`, validating every name against `MOVE_LIST` |
| `tools/make_sprites.py` | sprite folder → `sprites.h` + `sprites.S` |
| `tools/make_dex.py` | name/type JSON → `dex_data.h` |
| `tools/make_release.sh` | compile + merge bootloader/partitions/boot_app0/app at `0x0` |
| `tools/shot.py` | `SHOT` → PNG: what the panel is being sent |
| `tools/verify.py` | `SCAN` + `PERF` + frame grabs, exits non-zero if the sweep is short |

Regenerate after changing art or data:

```bash
python3 tools/make_battle_art.py            # needs art/Battle&animations/EBDX.zip
python3 tools/make_sprites.py art/gen5 pokedex/sprites.h
python3 tools/make_dex.py art/dex-1-649.json pokedex/dex_data.h
POKEAPI_CACHE=build/pokeapi-cache python3 tools/fetch_signatures.py  # network
python3 tools/make_signature.py
```

The signature JSON is committed, so a normal build never touches the network.
The fetch script reads each species' Gen 5 level-up list (Black 2/White 2 first,
then Black/White) and keeps the **last damaging move that species actually
learns** — the attack it would still be using at the end of the game. Nothing is
spread across the dex for variety: if forty species really end on Earthquake,
forty entries use Earthquake. A move the sketch cannot animate falls through to
the next real one, and a species with no usable attack (Ditto has only Transform)
falls back to its type pool. A handwritten override table still pins the
identities people know — Charizard/Flamethrower, Gyarados/Hyper Beam,
Pikachu/Thunderbolt, Groudon/Earthquake, Reshiram/Blue Flare, Zekrom/Bolt
Strike — but an override is only used when the species really learns the move by
level in Gen 5, which keeps Gen 6 moves (Dazzling Gleam, Moonblast) out of a
Gen 5 dex.

## The move engine

There are ten visual families, because those are the shapes that actually read
on a 320×170 panel:

| Family | Shape | Examples |
|---|---|---|
| `BEAM` | tapered procedural lance of light, hot core, moving sparks | Hyper Beam, Solar Beam, Ice Beam, Hydro Cannon |
| `BREATH` | dense native-aspect puffs plus a continuous hot core | Flamethrower, Blast Burn, Hydro Pump, Dragon Breath |
| `BOLT` | one jagged polyline with a soft halo and forks | Thunderbolt, Thunder, Bolt Strike |
| `ORB` | projectiles on an arc, with a hot centre and impact | Shadow Ball, Fire Blast, Psychic, Focus Blast, Energy Ball |
| `SLASH` | offset claw/streak passes that converge on the hit | Night Slash, X-Scissor, Dragon Claw |
| `RAIN` | debris falling across the target half | Rock Slide, Stone Edge, Blizzard, Icicle Spear |
| `QUAKE` | expanding ground rings, fissures and rock shards | Earthquake, Bulldoze |
| `AURA` | orbiting sparks and a light pillar around the creature | Roost, Meditate, Shadow Force |
| `WAVE` | pressure rings and native-aspect streaks | Gust, Surf, Heat Wave, Water Spout |
| `STORM` | projectiles spiralling out then converging | Magma Storm, Hurricane, Eruption |

Each family has its own body motion (wind-up, lunge, slam, float), floor glow,
muzzle star, impact starburst/rings/sparks and a white hit flash. Beams,
breaths, bolts and waves travel from the mouth to the opponent's chest instead
of running off the panel edge, and every impact adds a shaped core, expanding
rings and a reaction on Ditto. The lower-right bar shows the player's number,
name and types; the lower-left bar always names the signature move. The opponent
HP bar sits above Ditto.

**Move-specific sheets, not one stretched texture.** Close Combat uses the
archive's fists, Thunderbolt strikes down with the lightning columns, Vine Whip
is the green lash, Shadow Ball is the purple orb, Water Gun is the droplet
stream, Psychic is the ripple around Ditto, Aura Sphere is the blue spiral shot,
and Fire Blast is the five-pointed flame star. Slashes and claws draw the real
slash/claw sheets rotated into place instead of generic line work. The rest of
the families still use native-aspect EBDX puffs, tapered beams and drawn light
effects, never a texture stretched across the gap.

**Numbers from the game.** The move named on the HUD is the move the species
really learns, and the type and base power beside it are that move's own. Each
hit's bite on Ditto comes from Gen 5's damage formula at level 50
(`((2L/5+2)·power·A/D)/50+2`), with STAB and the type chart against Ditto's pure
Normal type, a Ditto with 48 base Def/SpD and no EVs, and no critical hit or
random roll — one entry has to look the same on every pass. That per-move damage
is scaled at build time so the 649 entries spend the 100,000 HP pool exactly:
Hyper Beam takes a visible bite, Tackle barely moves the bar, a status move takes
nothing at all, and Ditto still faints on #649. Both numbers live in
`signature_moves.h`, so the firmware does no battle maths at run time.

**Facing and target.** Gen 5 front sprites are drawn facing the opponent (left),
so the whole set is mirrored at draw time to face Ditto on the upper right. This
is a display transform only: the sprite pixels, palettes and Gen 5 art are
untouched. The player occupies the lower-left position (`CX=96`), Ditto the
upper-right platform (`TARGET_X=252`), and both have a soft contact shadow on
their own platform.

**Battle framing.** The painted EBDX battleback supplies the world, while the
two raised discs establish the classic battle perspective: the player's feet are
at `GROUND_Y=128`, Ditto's platform is higher at `TARGET_GROUND_Y=112`. The
reflection is still tinted from the scene's own floor colour. There is no
separate hand-painted stage art; the platform rings are drawn procedurally.

## Measured on the board

Flash **3,616,423 B** of 4,038,656 (89 %); static RAM 55,760 B; **167,364 B heap
free at boot**, before the BLE/Wi-Fi stack claims its share. The second 108 KB
framebuffer is not allocated in this build.

| Work | Cost |
|---|---|
| Frame push (`pushSprite`, 108 KB at 20 MHz parallel) | **5.54 ms** |
| Creature blit (4 bpp, run-merged) | 3.46 ms |
| Backdrop (direct flash read; no RAM cache in the BLE build) | 3.45 ms |
| World (ground light, reflection) | 0.27 ms |
| Moves + creature motion | 6.23 ms |
| HUD + Pokéball | 1.44 ms |
| **Total draw** | **17.16 ms**, about 58 fps (15.9–17.2 ms across runs) |

`SCAN` over all 649: **649/649 drawn**, 18.96 ms average, worst single frame
33.78 ms (#639 Terrakion, Stone Edge; the direct flash backdrop and BLE task
both contribute). The earlier non-BLE build was 11.33 ms with a cached backdrop;
the 108 KB cache is the RAM this build gives to RainMaker.

The push is the floor: at 20 MHz it is 5.5 ms of a 16.6 ms budget and cannot be
reduced without over-clocking the panel bus past the ST7789 write-cycle spec.
That is a deliberate refusal, not an oversight — `SHOT` reads the frame buffer,
so a too-fast bus would corrupt the panel invisibly to every check here.

## Rhythm

Six seconds of action an entry: **0.7 s arrival**, the signature move at
**0.95 s** for **2.85 s**, the hold until 6.0 s, then the next entry. A full pass
over 649 still takes about **74 minutes**. Ditto's 100,000 HP is the pass
counter: one registered hit per entry, each spending the share of the pool that
its move's real Gen 5 damage earns. On the 649th hit the HP reaches zero, the
real sprite squashes into a flattened puddle, and a fresh Ditto drops back onto
the platform at full HP. `GOTO`/`NEXT`/`PREV` spend the pool at a different rate,
so a new pass always starts with a fresh Ditto.

The BLE release deliberately runs single-buffered. The dissolve code is still
present for a build with `haveNext` true, but RainMaker's BLE provisioning and
Wi-Fi stack need the 108 KB that the frozen frame used to occupy. Without the
second buffer the entry changes directly at the six-second mark; the ball and
HUD still draw every frame.

## The frame buffer is in panel byte order

Every write through the sprite API is byte-swapped on the way in, so
`getBuffer()` does not contain RGB565 — it contains the same words with their
halves the other way round. `BG_DATA` is pre-swapped for this reason. Writes
straight into the buffer swap as they go; the optional melt path's buffer-to-
buffer copies need no swap. A `memcpy` of unswapped scene data produces correct shapes and
rainbow noise, which is what made the original bug look like corrupt data.

## Controls

Serial at 115200: `NEXT`, `PREV`, `GOTO <1-649>`, `PAUSE`, `INFO`, `PERF`,
`SHOT`, `SCAN`, `VERBOSE`, `OFF`.

`VERBOSE` turns per-entry logging on. `SHOT` dumps the frame buffer and
`tools/shot.py` decodes it. `OFF` is a real power-down: backlight zero, panel
asleep, frame buffer freed, then ESP32 deep sleep; only reset/EN wakes it.

## ESP RainMaker / Bluetooth

The release includes RainMaker BLE provisioning. On a fresh board:

1. Open the **ESP RainMaker** app and add a device.
2. Choose **PROV_POKEDEX**; the proof-of-possession is **`pokedex123`**.
3. Give it a 2.4 GHz Wi-Fi network. BLE is used for provisioning; after that the
   app controls the node through RainMaker.

The **Pokedex** node exposes two controls:

- **Index** — a 1–649 slider. Drag it left or right to move through the dex; the
  board jumps on the next frame and keeps the current play/pause state.
- **Paused** — a toggle that freezes or resumes the six-second cycle.

If the app cannot find the board, reset it and look for `PROV_POKEDEX`; the BLE
window stays open for five minutes. The BLE name and PoP live in `pokedex.ino`
(`RMAKER_SERVICE`, `RMAKER_POP`).

## Limits

- The archive also ships `shiny/`, `back/` and `female/` variants and form
  sprites. None are used: one form per entry, or the dex stops being 649.
- `SHOT` proves the pixels being sent to the panel, not the panel's gamma. If
  colours look wrong on the glass, adjust the palettes in the baker.
- `GOTO` and the signature table are baked at build time; there is no runtime
  PokéAPI dependency.
- Ditto is the only opponent and its 100,000 HP is the pass counter. Damage is
  real Gen 5 damage scaled into that pool, so a status move — or a Ghost move
  into a Normal Ditto — does zero and the bar does not move for that entry. The
  reference is a level-50 Ditto with no EVs; critical hits and the 0.85–1.0
  random roll are left out so an entry looks the same on every pass. The dex
  shows one player species at a time; Ditto is not part of the 649.- The whole sprite set is mirrored. A sprite whose archive art already faced
  right will face left here; that is the deliberate consequence of putting the
  opponent on the right. The pixels themselves are not redrawn.
- The BLE build trades the dissolve transition for RainMaker RAM. The steady
  frame is 15.9–17.2 ms; the per-entry SCAN is about 19 ms because the backdrop
  is read directly from flash. The non-BLE build kept a cached backdrop and the
  second framebuffer.
