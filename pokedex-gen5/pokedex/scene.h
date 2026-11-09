// Types that appear in function signatures.
//
// Not stylistic: the Arduino builder writes its generated function prototypes
// immediately before the first function definition in the .ino file. Anything a
// signature mentions has to already be visible at that point, and an include at
// the top always is. Putting these in the .ino below the first function gives
// "'Look' does not name a type" from a file that reads perfectly well.
#pragma once
#include <stdint.h>

// Per-entry accent colours, resolved once when an entry appears. The creature's
// own colours come from its sprite palette; these three drive the effects, the
// HUD rule and the type chips.
struct Look {
  uint16_t base, accent, glow;
};

// Palette for the current biome, resolved to RGB565 at entry.
struct SceneCol {
  uint16_t skyTop, skyMid, skyBot, far, mid, near, ground, groundTop;
};

// What a creature does while nothing is happening, per type. Here rather than in
// the .ino for the same reason as everything else in this header: idleFor()
// returns one, and a return type must be visible where the generated prototypes
// land.
struct Idle {
  float bobAmp, bobHz, swayAmp, jitter;
};

// How a signature move is staged. The ten families cover beams, thrown objects,
// claws, falling debris, ground hits, self-buffs and weather-sized sweeps; the
// EBDX texture chosen for each move does the rest.
enum AtkFam : uint8_t {
  AF_BEAM, AF_BREATH, AF_BOLT, AF_ORB, AF_SLASH, AF_RAIN, AF_QUAKE, AF_AURA,
  AF_WAVE, AF_STORM, AF_COUNT
};

// One row of the move vocabulary. `tex1` is the main effect texture, `tex2` the
// charge/impact texture, and `count` how many pieces the family throws.
// Nothing here is per-species: 649 entries share this table and a compact pool,
// with overrides only for the moves that are part of a creature's identity.
struct MoveDef {
  const char *name;
  uint8_t fam;
  uint8_t type;
  uint8_t tex1, tex2;
  uint8_t count;
};

// How an entry arrives on screen. Four motions cover every creature without
// pretending that a Gyarados and a Caterpie walk in the same way.
enum Arrive : uint8_t {
  A_POP,        // drops in and lands with a squash
  A_DESCEND,    // comes down out of the sky, no bounce: it flies
  A_RISE,       // surfaces from below the ground line: it swims
  A_FADE,       // grows into place: it was already there
  A_COUNT
};
