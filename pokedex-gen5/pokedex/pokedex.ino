// pokedex — one entry at a time, six seconds of action plus the melt.
//
// The scene is the point. Every entry arrives in a painted Elite Battle DX
// environment, performs its signature move with EBDX's own effect textures, and
// holds while the world melts into the next creature underneath it.
//
// The Gen 5 sprites are untouched: 96x96 archive art, cropped and packed to 4bpp
// by tools/make_sprites.py. The battle scenes and move textures are baked from
// art/Battle&animations/EBDX.zip by tools/make_battle_art.py.
//
// Board: LilyGo T-Display S3, ESP32-S3, ST7789 170x320 on an 8-bit parallel bus
// (D0..D7 = 39,40,41,42,45,46,47,48; WR 8, RD 9, DC 7, CS 6, RST 5; BL 38;
// peripheral power enable GPIO 15).

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <math.h>
#include <string.h>
#include "RMaker.h"
#include "WiFi.h"
#include "WiFiProv.h"

// ---------------------------------------------------------------- the types ---

enum Type : int8_t {
  T_NORMAL, T_FIRE, T_WATER, T_ELECTRIC, T_GRASS, T_ICE, T_FIGHTING, T_POISON,
  T_GROUND, T_FLYING, T_PSYCHIC, T_BUG, T_ROCK, T_GHOST, T_DRAGON, T_DARK,
  T_STEEL, T_FAIRY, T_COUNT
};

#include "dex_data.h"
#include "signature_moves.h"
#include "scene.h"
#include "sprites.h"
// Ten painted 320x138 battle scenes and the 4bpp move effects, both from EBDX.
#include "bg_art.h"
#include "atk_art.h"

// ---------------------------------------------------------------- the panel ---

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_Parallel8 _bus;
  lgfx::Light_PWM _light;

 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.freq_write = 20000000;
      cfg.pin_wr = 8;
      cfg.pin_rd = 9;
      cfg.pin_rs = 7;
      cfg.pin_d0 = 39;
      cfg.pin_d1 = 40;
      cfg.pin_d2 = 41;
      cfg.pin_d3 = 42;
      cfg.pin_d4 = 45;
      cfg.pin_d5 = 46;
      cfg.pin_d6 = 47;
      cfg.pin_d7 = 48;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 6;
      cfg.pin_rst = 5;
      cfg.pin_busy = -1;
      cfg.panel_width = 170;
      cfg.panel_height = 320;
      cfg.offset_x = 35;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.invert = true;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 38;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

static LGFX lcd;
static LGFX_Sprite fb(&lcd);
// The outgoing frame, frozen. A melt needs two pictures; this is the second
// 108 KB, which is why the board runs with PSRAM off and still has ~100 KB of
// heap afterwards. Without it the only transition available is a wipe to black.
static LGFX_Sprite nb(&lcd);
static bool haveSprite = false;
static bool haveNext = false;
// When no melt is in flight, the second frame buffer doubles as a RAM cache of
// the current 88 KB backdrop. Without it every frame re-reads the scene from
// flash (~3.3 ms); with it the copy is RAM-to-RAM (~0.3 ms). The cache is thrown
// away at every entry and refilled on the first frame after the melt.
static bool bgCached = false;

static const int W = 320;
static const int H = 170;

static const int CX = 96;           // player-side creature, lower left
static const int GROUND_Y = 128;    // its feet
static const int HUD_Y = 136;       // battle bar height is 34 px
// The opposing Ditto, upper right on its own platform. Every move aims at its
// chest, so a beam, a thrown orb and a claw all visibly land on the opponent.
static const int TARGET_X = 252;
static const int TARGET_Y = 88;
static const int TARGET_GROUND_Y = 112;
// The real archive sprite, not a drawn substitute. Ditto is entry 132 in the
// same generated table as every player-side creature.
static const int DITTO_SPRITE = 131;
static const int DITTO_HP_MAX = 100000;
static const int DITTO_HITS = 649;

// -------------------------------------------------------------------- style ---

struct TypeStyle { uint32_t rgb; const char *label; };
static const TypeStyle TYPE_STYLE[T_COUNT] = {
  {0xC8C2B0, "NORMAL"},  {0xF0703C, "FIRE"},   {0x4F9FE0, "WATER"},
  {0xF5C928, "ELECTRIC"},{0x6CBE58, "GRASS"},  {0x8FD6E8, "ICE"},
  {0xC23A2E, "FIGHTING"},{0xA35CB8, "POISON"}, {0xD6A45A, "GROUND"},
  {0x9DB4F2, "FLYING"},  {0xF05C9C, "PSYCHIC"},{0x9BBE2C, "BUG"},
  {0xB49C38, "ROCK"},    {0x6A5FA6, "GHOST"},  {0x6A4FC4, "DRAGON"},
  {0x565063, "DARK"},    {0x8C98A4, "STEEL"},  {0xEFA6C6, "FAIRY"},
};

enum Biome : uint8_t { B_FOREST, B_CAVE, B_VOLCANO, B_OCEAN, B_SKY,
                       B_CITY, B_SNOW, B_STORM, B_VOID, B_DESERT, B_COUNT };
static_assert(BG_COUNT == B_COUNT, "bg_art.h scenes must match the Biome enum");

static const char *BIOME_NAME[B_COUNT] = {
  "FOREST", "CAVE", "VOLCANO", "OCEAN", "SKY", "CITY", "SNOW", "STORM", "VOID",
  "DESERT"
};

// One type's opinion about where it belongs. B_COUNT means "no opinion", so the
// caller can ask the secondary type before falling back to the city.
static Biome biomeOf(int8_t t) {
  switch (t) {
    case T_FIRE: return B_VOLCANO;
    case T_WATER: return B_OCEAN;
    case T_ELECTRIC: return B_STORM;
    case T_GRASS: case T_BUG: case T_FAIRY: return B_FOREST;
    case T_ICE: return B_SNOW;
    case T_GROUND: return B_DESERT;
    case T_ROCK: case T_POISON: return B_CAVE;
    case T_FLYING: return B_SKY;
    case T_PSYCHIC: case T_GHOST: case T_DRAGON: case T_DARK: return B_VOID;
    default: return B_COUNT;             // NORMAL, FIGHTING, STEEL
  }
}

static Biome biomeFor(int8_t t1, int8_t t2) {
  const Biome a = biomeOf(t1);
  if (a != B_COUNT) return a;
  if (t2 >= 0) {
    const Biome b = biomeOf(t2);
    if (b != B_COUNT) return b;
  }
  return B_CITY;
}

// The painted scene carries the ground and the layers now. This table is only
// the ambient palette: weather, the horizon glow and the fallback floor colour.
struct BiomeStyle {
  uint32_t skyTop, skyMid, skyBot;
  uint32_t far, mid, near, ground, groundTop;
  uint8_t weather;      // 0 none, 1 rain, 2 snow, 3 ember, 4 leaf, 5 bubble,
                        // 6 dust, 7 star, 8 spark
};
static const BiomeStyle BIOMES[B_COUNT] = {
  {0x1B3A2B, 0x2E5C3A, 0x7FA05A, 0x1A3B2C, 0x235239, 0x2D6B46, 0x3C6B3A, 0x568A44, 4}, // FOREST
  {0x0E0E16, 0x1C1A26, 0x39344A, 0x171522, 0x211E2E, 0x2C283C, 0x3A3348, 0x4E4560, 6}, // CAVE
  {0x2A0B0C, 0x59120F, 0xC24A18, 0x431410, 0x5A1810, 0x7A2412, 0x2E1413, 0x7C2A16, 3}, // VOLCANO
  {0x123A52, 0x1D5C7A, 0x4FA6B8, 0x14506E, 0x1E6C8C, 0x2C86A4, 0x1B4C5E, 0x63BFC8, 5}, // OCEAN
  {0x2B4E7A, 0x5A8FC0, 0xA9CEE4, 0x7FA6C8, 0xA8C6DC, 0xD2E4EE, 0xA9BFD2, 0xDCE8F2, 0}, // SKY
  {0x121026, 0x241F3E, 0x4A3F63, 0x1A1730, 0x272245, 0x35305A, 0x2A2438, 0x3E3752, 0}, // CITY
  {0x243A55, 0x557AA0, 0xC2D8E4, 0x93AFC6, 0xB8CDDC, 0xE2EDF2, 0xA8C2CE, 0xD8E8EE, 2}, // SNOW
  {0x0A1020, 0x18213A, 0x2E3A56, 0x121A2E, 0x1A2440, 0x243052, 0x1A2032, 0x2C3650, 1}, // STORM
  {0x07030F, 0x120A24, 0x241440, 0x150C28, 0x1D1236, 0x2A1A48, 0x160E26, 0x241838, 7}, // VOID
  {0x2C2A5E, 0x7A5B8E, 0xE0A578, 0x5E4340, 0x8E6647, 0xC79974, 0xB28258, 0xE2BC90, 6}, // DESERT
};

// ------------------------------------------------------------------ runtime ---

static int idx = 0;                 // index into DEX
static uint8_t biome = B_FOREST;
static uint32_t entryAt = 0;

enum Phase : uint8_t { P_IN, P_HOLD, P_MELT };
static Phase phase = P_IN;

// CYCLE_MS is the visible action time. The 820 ms melt runs after it, so an
// entry owns the screen for 6.0 s and the next one rises through it.
static const uint32_t CYCLE_MS = 6000;
static const uint32_t IN_MS = 700;
static const uint32_t OUT_MS = 820;
static const uint32_t HOLD_MS = CYCLE_MS - IN_MS;

// The signature move is a staged action now: a wind-up, the strike, the hit and
// a short recovery, 2.85 s in total. The cycle was opened from five seconds to
// six so the extra half second of action does not eat the hold or the melt.
static const float ATK_START = 0.95f;
static const float ATK_LEN = 2.85f;

static bool paused = false;
static volatile int pendingGoto = 0;       // 1-based, written by the RainMaker task
static volatile int pendingPause = -1;     // -1 unchanged, 0 play, 1 pause
static Device *pokedexDev = nullptr;
static int lastReportedIndex = -1;
static int lastReportedPause = -1;
static float ballSpin = 0.0f;
static bool verbose = false;
static bool reacted = false;
static bool quiet = false;
static float animT = 0.0f;

static Look look;
static SceneCol scene;
// Sampled at build time from the painted floor around the feet. The reflection
// and contact shadow borrow it so they sit on the actual ground, not on the
// biome's nominal palette.
static uint16_t floorTint = 0;

static const float TARGET_H = 116.0f;
static const float MIN_SCALE = 1.0f;
static const float MAX_SCALE = 2.8f;
static int gX, gY, gW, gH;
static bool gFlip = false;   // display mirror, so a left-facing sprite aims at the target
static int srcMap[104], rowMap[168];
// Per-species variation, so two creatures sharing a move do not produce an
// identical effect. Values are stable for an entry and never touch the sprite.
static float fxA = 0.5f, fxB = 0.5f, fxC = 0.5f;
// Decaying reaction on the opposing Ditto. atkImpact() and the once-per-entry
// hit registration raise it, the next frames flash and wobble the target, and
// it falls back to zero on its own.
static float targetHit = 0.0f;
static int dittoHp = DITTO_HP_MAX;
static uint16_t dittoHits = 0;
static bool dittoFaint = false;
static bool dittoHitCounted = false;
static uint32_t dittoFaintAt = 0;
static uint32_t dittoRespawnAt = 0;
static Idle idle = {1.2f, 1.1f, 0.0f, 0.0f};
static Arrive arrive = A_POP;
static float shake = 0.0f;

// Frame accounting for PERF.
static uint32_t frameUsSum = 0, frameCount = 0;
static uint32_t blitUs = 0, pushUs = 0;
static uint32_t bgUs = 0, worldUs = 0, fxUs = 0, hudUs = 0;
static uint32_t meltUs = 0, meltFrames = 0, meltWorst = 0;
static uint32_t freezeUs = 0, freezeWorst = 0, meltApplyUs = 0;
static uint32_t gapMeltWorst = 0, gapNormalWorst = 0;
#define TICK(v) do { const uint32_t _t = micros(); v += _t - _mark; _mark = _t; } while (0)

static uint16_t C(uint32_t rgb) {
  return fb.color565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}
static uint16_t lerp565(uint16_t a, uint16_t b, float t) {
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + (int)((br - ar) * t), g = ag + (int)((bg - ag) * t), bl = ab + (int)((bb - ab) * t);
  return (uint16_t)((r << 11) | (g << 5) | bl);
}
static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static float easeOutBack(float t) {
  const float c1 = 1.70158f, c3 = c1 + 1.0f;
  const float u = t - 1.0f;
  return 1.0f + c3 * u * u * u + c1 * u * u;
}
static float easeOutCubic(float t) { return 1 - powf(1 - t, 3); }
static uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}

// --------------------------------------------------------------- the worlds ---

// The whole background is one baked image, already in panel byte order. A row
// copy is all the drawing it needs; during a melt it stops at the front.
static void drawBackdrop(int yLimit) {
  uint16_t *dst = (uint16_t *)fb.getBuffer();
  const int rows = yLimit < BG_H ? yLimit : BG_H;
  if (!dst) {
    fb.fillRect(0, 0, W, rows, C(BIOMES[biome].skyTop));
    return;
  }
  const uint16_t *src;
  if (bgCached && haveNext) src = (const uint16_t *)nb.getBuffer();
  else src = &BG_DATA[BGS[biome].off];
  for (int y = 0; y < rows; y++) memcpy(dst + (size_t)y * W, src + (size_t)y * W, W * 2);
}

// One flash read per entry, into the melt buffer while no melt needs it. The
// first 138 rows are the painted scene; the rest of nb is untouched.
static void cacheBackdrop() {
  if (bgCached || !haveNext) return;
  uint16_t *cache = (uint16_t *)nb.getBuffer();
  if (!cache) return;
  const uint16_t *src = &BG_DATA[BGS[biome].off];
  for (int y = 0; y < BG_H; y++) memcpy(cache + (size_t)y * W, src + (size_t)y * W, W * 2);
  bgCached = true;
}

// Weather is a light field of dots, not a physics toy: 40 marks is enough to read
// as rain, snow, embers or fireflies without eating the frame budget.
static void drawWeather(float t, uint8_t kind) {
  const uint16_t c = C(0xDCE8F6);
  for (int i = 0; i < 40; i++) {
    const uint32_t h = hash32(i * 97 + 5);
    const float speed = 22.0f + (h & 0x3F);
    float x = (h >> 8) % W;
    float y = fmodf((h >> 16) % GROUND_Y + t * speed, (float)GROUND_Y);
    switch (kind) {
      case 1:                                              // rain
        fb.drawLine((int)x, (int)y, (int)x - 2, (int)y + 6, lerp565(c, scene.skyBot, 0.5f));
        break;
      case 2:                                              // snow
        x += sinf(t * 1.4f + i) * 5;
        fb.fillCircle((int)x, (int)y, 1 + (i % 2), C(0xFFFFFF));
        break;
      case 3:                                              // embers
        y = GROUND_Y - fmodf((h >> 16) % GROUND_Y + t * speed, (float)GROUND_Y);
        fb.drawPixel((int)(x + sinf(t + i) * 3), (int)y, lerp565(C(0xFF8A3C), C(0xFFE08A), (y / GROUND_Y)));
        break;
      case 4:                                              // leaves
        x = fmodf(x + t * 12, (float)W);
        y = fmodf(y + t * 9, (float)GROUND_Y);
        fb.drawPixel((int)x, (int)y, C(0x9ACB5A));
        fb.drawPixel((int)x + 1, (int)y, C(0x6E9E3A));
        break;
      case 5:                                              // bubbles
        y = GROUND_Y - fmodf(t * speed * 0.7f + (h >> 16) % GROUND_Y, (float)GROUND_Y);
        fb.drawCircle((int)x, (int)y, 2 + (i % 3), lerp565(scene.skyBot, c, 0.6f));
        break;
      case 6:                                              // dust
        x += sinf(t * 0.6f + i) * 8;
        fb.drawPixel((int)x, (int)y, C(0x8C8494));
        break;
      case 7:                                              // fireflies / stars
        fb.drawPixel((int)x, (int)(y * 0.7f), lerp565(scene.skyBot, C(0xBBA8FF),
                    0.4f + 0.6f * sinf(t * 3 + i)));
        break;
      case 8:                                              // sparks
        fb.drawPixel((int)x, (int)y, C(0xFFE58A));
        break;
      default: break;
    }
  }
}

// A Pokéball, drawn as one, turning a full revolution every cycle. The centre
// and radius never move: it is furniture, and a wandering ball would leave a
// ghost of itself in the frozen melt frame.
static void drawPokeball(int cx, int cy, int r, float spin, float pulse) {
  const uint16_t red = C(0xE23B2E), shell = C(0xF2EDE4), ink = C(0x141019);
  const float ca = cosf(spin), sa = sinf(spin);
  const bool sideOn = fabsf(ca) < 0.16f;
  for (int dx = -r; dx <= r; dx++) {
    const int half = (int)sqrtf((float)(r * r - dx * dx));
    const int top = cy - half, bot = cy + half;
    if (sideOn) {
      fb.fillRect(cx + dx, top, 1, bot - top + 1, (sa * dx) > 0.0f ? red : shell);
      continue;
    }
    int cut = (int)((float)cy + (float)dx * (sa / ca) + 0.5f);
    if (cut < top) cut = top;
    if (cut > bot) cut = bot;
    const bool redAbove = ca > 0.0f;
    if (cut > top) fb.fillRect(cx + dx, top, 1, cut - top, redAbove ? red : shell);
    if (bot > cut) fb.fillRect(cx + dx, cut, 1, bot - cut, redAbove ? shell : red);
  }
  const int ex = (int)(ca * (float)r), ey = (int)(sa * (float)r);
  const int ex2 = (int)(ca * (float)(r - 1)), ey2 = (int)(sa * (float)(r - 1));
  const int nx = (int)(sa * 1.4f + (sa >= 0.0f ? 0.5f : -0.5f));
  const int ny = (int)(-ca * 1.4f + (-ca >= 0.0f ? 0.5f : -0.5f));
  fb.drawLine(cx - ex, cy - ey, cx + ex, cy + ey, ink);
  fb.drawLine(cx - ex2 + nx, cy - ey2 + ny, cx + ex2 + nx, cy + ey2 + ny, ink);
  fb.drawCircle(cx, cy, r, ink);
  fb.fillCircle(cx, cy, r / 3 + 1, ink);
  fb.fillCircle(cx, cy, r / 3 - 1, shell);
  if (pulse > 0.55f) fb.fillCircle(cx, cy, r / 3 - 2, lerp565(shell, 0xFFFF, pulse * 0.6f));
}

// ------------------------------------------------------------- the creature ---

static Idle idleFor(int8_t t1) {
  switch (t1) {
    case T_FLYING: return {4.0f, 0.85f, 0.0f, 0.0f};
    case T_BUG: return {2.4f, 2.10f, 0.0f, 0.8f};
    case T_WATER: return {2.0f, 1.00f, 2.0f, 0.0f};
    case T_GHOST: return {3.0f, 0.65f, 1.6f, 0.0f};
    case T_ELECTRIC: return {0.8f, 3.00f, 0.0f, 0.0f};
    case T_FIRE: return {1.0f, 2.40f, 0.0f, 0.6f};
    case T_DRAGON: return {2.0f, 0.80f, 1.0f, 0.0f};
    case T_PSYCHIC: return {2.6f, 0.55f, 0.0f, 0.0f};
    case T_ICE: return {1.0f, 0.70f, 0.0f, 0.0f};
    default: return {1.2f, 1.10f, 0.0f, 0.0f};
  }
}

static Arrive arriveFor(int8_t t1, int8_t t2) {
  const int8_t t = t2 >= 0 ? t2 : t1;
  switch (t1) {
    case T_FLYING: case T_DRAGON: return A_DESCEND;
    case T_WATER: return A_RISE;
    case T_GHOST: case T_PSYCHIC: case T_DARK: case T_ICE: return A_FADE;
    default: break;
  }
  switch (t) {
    case T_FLYING: return A_DESCEND;
    case T_WATER: return A_RISE;
    default: return A_POP;
  }
}

static void deriveLook(int8_t t1, int8_t t2) {
  const uint32_t acc = t2 >= 0 ? TYPE_STYLE[t2].rgb : TYPE_STYLE[t1].rgb;
  look.base = C(TYPE_STYLE[t1].rgb);
  look.accent = C(acc);
  look.glow = C(lerp565(C(acc), 0xFFFF, 0.55f));
  idle = idleFor(t1);
  arrive = arriveFor(t1, t2);
}

// Front sprites in the archive are not all drawn facing the same way. The player
// side is mirrored in the games; mirroring these makes their mouth the right-hand
// edge, which is where every effect launches from.
static bool mirrorFor(int n) {
  // The Gen 5 front archive is drawn facing the opponent (to the left). Every
  // scene now puts the opposing Ditto on the right, so the whole set is flipped
  // at draw time. This is a display transform only; the sprite pixels and
  // palette stay exactly as archived.
  (void)n;
  return true;
}

static void layoutCreature(float k, float squash, float bob, int drop, int sway) {
  const SprInfo &s = SPRITES[idx];
  const float sx = 1.0f + (1.0f - squash) * 0.7f;
  const float maxW = 186.0f, maxH = (float)(GROUND_Y - 10);
  float sc = TARGET_H / (float)s.h;
  const float fitW = maxW / (float)s.w;
  const float fitH = maxH / (float)s.h;
  if (sc > fitW) sc = fitW;
  if (sc > fitH) sc = fitH;
  if (sc > MAX_SCALE) sc = MAX_SCALE;
  if (sc < MIN_SCALE) sc = MIN_SCALE;
  gW = (int)(s.w * sc * k * sx);
  gH = (int)(s.h * sc * k * squash);
  if (gW < 8) gW = 8;
  if (gH < 8) gH = 8;
  gX = CX + sway - gW / 2;
  gY = GROUND_Y - gH - drop - (int)bob;
}

// One source row, emitted as runs. White touching the sprite border was keyed by
// tools/make_sprites.py, and the outline is baked as nibble 15, so this only
// moves pixels.
static void emitRow(const uint8_t *row, int py, float mix, uint16_t tint, float flash) {
  const SprInfo &s = SPRITES[idx];
  const uint16_t edge = mix > 0.0f ? lerp565(C(0x0A0810), tint, mix) : C(0x0A0810);
  int runStart = 0;
  uint8_t runVal = 0xFF;
  for (int sx = 0; sx <= s.w; sx++) {
    uint8_t v = 0;
    if (sx < s.w) {
      const uint8_t pack = row[sx >> 1];
      v = (sx & 1) ? (pack & 0x0F) : (pack >> 4);
    }
    if (v == runVal) continue;
    if (runVal != 0xFF && runVal != 0 && sx > runStart) {
      int x0 = srcMap[runStart];
      int x1 = sx < s.w ? srcMap[sx] : gW;
      if (gFlip) {
        const int a = gW - x1, b = gW - x0;
        x0 = a;
        x1 = b;
      }
      if (x1 > x0) {
        uint16_t c;
        if (runVal == SPR_OUTLINE) {
          c = edge;
        } else {
          c = s.pal[runVal - 1];
          if (mix > 0.0f) c = lerp565(c, tint, mix);
          if (flash > 0.01f) c = lerp565(c, 0xFFFF, flash);
        }
        fb.fillRect(gX + x0, py, x1 - x0, 1, c);
      }
    }
    runStart = sx;
    runVal = v;
  }
}

static void blitCreature(float flash, float reveal) {
  const SprInfo &s = SPRITES[idx];
  const int stride = (s.w + 1) / 2;
  const uint8_t *data = &SPR_DATA[s.off];
  const int revealY = (int)(reveal * gH);

  for (int i = 0; i <= s.w; i++) srcMap[i] = i * gW / s.w;
  for (int i = 0; i < gH; i++) rowMap[i] = i * s.h / gH;

  for (int yy = 0; yy < gH; yy++) {
    if (reveal < 1.0f && yy > revealY) break;
    const int py = gY + yy;
    if (py < -2 || py >= HUD_Y) continue;
    emitRow(data + rowMap[yy] * stride, py, 0.0f, 0, flash);
  }
}

// A mirrored copy under the ground line. The eye takes contact from the mirror
// more than from the shadow, and eight source rows cost almost nothing.
static void drawReflection(float strength) {
  if (strength <= 0.02f) return;
  const SprInfo &s = SPRITES[idx];
  const int stride = (s.w + 1) / 2;
  const uint8_t *data = &SPR_DATA[s.off];
  const int rows = 8;
  for (int r = 0; r < rows; r++) {
    const int py = GROUND_Y + 2 + r;
    if (py >= HUD_Y) break;
    const int sy = gH - 1 - r;
    if (sy < 0) break;
    const float f = (float)r / (float)rows;
    emitRow(data + rowMap[sy] * stride, py, 0.22f + 0.44f * f + (1.0f - strength) * 0.34f,
            floorTint, 0.0f);
  }
}

// ------------------------------------------------------------ move vocabulary ---
//
// One table for the whole dex. Every move is a real move name; the family is how
// it is staged, and the two textures are the main effect and its charge/impact.
// Pools below give each type a handful of moves; the override switch names the
// ones that are part of a creature's identity.

#define MOVE_LIST(X) \
  X(TACKLE,      AF_SLASH, T_NORMAL,   ATK_BURST,    ATK_SMOKE,    2) \
  X(SCRATCH,     AF_SLASH, T_NORMAL,   ATK_CLAW,     ATK_SLASH,    3) \
  X(FURYSWIPES,  AF_SLASH, T_NORMAL,   ATK_SLASH,    ATK_CLAW,     4) \
  X(BODYSLAM,    AF_SLASH, T_NORMAL,   ATK_BURST,    ATK_SMOKE,    2) \
  X(HYPERBEAM,   AF_BEAM,  T_NORMAL,   ATK_JET,      ATK_BURST,    3) \
  X(HYPERVOICE,  AF_WAVE,  T_NORMAL,   ATK_RIPPLE,   ATK_NOVA,     3) \
  X(SWIFT,       AF_ORB,   T_NORMAL,   ATK_SPARKLE,  ATK_SPARKLE,  5) \
  X(SING,        AF_AURA,  T_NORMAL,   ATK_SPARKLE,  ATK_SPARKLE,  6) \
  X(EMBER,       AF_ORB,   T_FIRE,     ATK_FLAME2,   ATK_FLAME,    3) \
  X(FLAMETHROWER,AF_BREATH,T_FIRE,     ATK_FLAME,    ATK_FLAME2,   9) \
  X(FLAREBLITZ,  AF_SLASH, T_FIRE,     ATK_FLAME,    ATK_NOVA,     3) \
  X(WILLOWISP,   AF_ORB,   T_FIRE,     ATK_FLAME2,   ATK_FLAME,    3) \
  X(FIREPUNCH,   AF_ORB,   T_FIRE,     ATK_FLAME,    ATK_SUN,      2) \
  X(HEATWAVE,    AF_WAVE,  T_FIRE,     ATK_FLAME,    ATK_RIPPLE,   3) \
  X(WATERGUN,    AF_ORB,   T_WATER,    ATK_WATER,    ATK_RIPPLE,   3) \
  X(BUBBLE,      AF_WAVE,  T_WATER,    ATK_WATER,    ATK_RIPPLE,   4) \
  X(BUBBLEBEAM,  AF_BREATH,T_WATER,    ATK_WATER,    ATK_RIPPLE,   9) \
  X(HYDROPUMP,   AF_BREATH,T_WATER,    ATK_WATER,    ATK_JET,      9) \
  X(AQUARING,    AF_AURA,  T_WATER,    ATK_RIPPLE,   ATK_WATER,    5) \
  X(SURF,        AF_WAVE,  T_WATER,    ATK_WAVE,     ATK_WATER,    3) \
  X(CRABHAMMER,  AF_SLASH, T_WATER,    ATK_CLAW,     ATK_WATER,    3) \
  X(THUNDERSHOCK,AF_ORB,   T_ELECTRIC, ATK_SPARK,    ATK_BOLT,     3) \
  X(THUNDERBOLT, AF_BOLT,  T_ELECTRIC, ATK_BOLT,     ATK_SPARK,    3) \
  X(THUNDERWAVE, AF_WAVE,  T_ELECTRIC, ATK_BOLT,     ATK_SPARK,    4) \
  X(THUNDERPUNCH,AF_ORB,   T_ELECTRIC, ATK_LIGHT,    ATK_SPARK,    3) \
  X(DISCHARGE,   AF_WAVE,  T_ELECTRIC, ATK_SPARK,    ATK_BOLT,     4) \
  X(VINEWHIP,    AF_SLASH, T_GRASS,    ATK_WHIP,     ATK_GREEN,    3) \
  X(RAZORLEAF,   AF_RAIN,  T_GRASS,    ATK_GREEN,    ATK_SWIRL,    5) \
  X(LEAFSTORM,   AF_STORM, T_GRASS,    ATK_GREEN,    ATK_SWIRL,    8) \
  X(INGRAIN,     AF_AURA,  T_GRASS,    ATK_GREEN,    ATK_SPARKLE,  6) \
  X(BULLETSEED,  AF_RAIN,  T_GRASS,    ATK_GREEN,    ATK_SPARKLE,  7) \
  X(SOLARBEAM,   AF_BEAM,  T_GRASS,    ATK_SUN,      ATK_GREEN,    3) \
  X(MEGADRAIN,   AF_ORB,   T_GRASS,    ATK_GREEN,    ATK_SPARKLE,  3) \
  X(SPORE,       AF_AURA,  T_GRASS,    ATK_SPARKLE,  ATK_SMOKE,    6) \
  X(ICESHARD,    AF_RAIN,  T_ICE,      ATK_CRYSTAL,  ATK_ICESTAR,  4) \
  X(ICEBEAM,     AF_BEAM,  T_ICE,      ATK_ICEBEAM,  ATK_ICESTAR,  3) \
  X(ICICLECRASH, AF_RAIN,  T_ICE,      ATK_CRYSTAL,  ATK_ICESTAR,  5) \
  X(ICYWIND,     AF_WAVE,  T_ICE,      ATK_ICEBEAM,  ATK_ICESTAR,  3) \
  X(ICEPUNCH,    AF_ORB,   T_ICE,      ATK_ICESTAR,  ATK_CRYSTAL,  3) \
  X(MIST,        AF_AURA,  T_ICE,      ATK_ICEBEAM,  ATK_SPARKLE,  5) \
  X(CLOSECOMBAT, AF_SLASH, T_FIGHTING, ATK_CLAW,     ATK_NOVA,     4) \
  X(AURASPHERE,  AF_ORB,   T_FIGHTING, ATK_AURA,     ATK_AURASHOT, 2) \
  X(BULKUP,      AF_AURA,  T_FIGHTING, ATK_SUN,      ATK_SPARKLE,  6) \
  X(POISONJAB,   AF_SLASH, T_POISON,   ATK_BLOB,     ATK_SLASH,    3) \
  X(SLUDGEBOMB,  AF_ORB,   T_POISON,   ATK_POISON,   ATK_BLOB,     2) \
  X(TOXIC,       AF_ORB,   T_POISON,   ATK_BLOB,     ATK_PSYRING,  2) \
  X(POISONGAS,   AF_WAVE,  T_POISON,   ATK_SMOKE,    ATK_BLOB,     3) \
  X(ACID,        AF_ORB,   T_POISON,   ATK_BLOB,     ATK_SMOKE,    3) \
  X(EARTHQUAKE,  AF_QUAKE, T_GROUND,   ATK_ROCK,     ATK_SMOKE,    3) \
  X(EARTHPOWER,  AF_QUAKE, T_GROUND,   ATK_ROCK,     ATK_SUN,      2) \
  X(BONERUSH,    AF_RAIN,  T_GROUND,   ATK_ROCK,     ATK_SPARKLE,  5) \
  X(MUDSPORT,    AF_QUAKE, T_GROUND,   ATK_ROCK,     ATK_SMOKE,    3) \
  X(GUST,        AF_WAVE,  T_FLYING,   ATK_STREAK,   ATK_RIPPLE,   3) \
  X(WINGATTACK,  AF_SLASH, T_FLYING,   ATK_SLASH,    ATK_STREAK,   3) \
  X(AIRSLASH,    AF_SLASH, T_FLYING,   ATK_STREAK,   ATK_SLASH,    3) \
  X(AIRCUTTER,   AF_WAVE,  T_FLYING,   ATK_STREAK,   ATK_RIPPLE,   3) \
  X(ROOST,       AF_AURA,  T_FLYING,   ATK_SPARKLE,  ATK_SUN,      5) \
  X(DRAGONASCENT,AF_BEAM,  T_FLYING,   ATK_STRIKE,   ATK_BURST,    3) \
  X(CONFUSION,   AF_ORB,   T_PSYCHIC,  ATK_PSYRING,  ATK_SPARKLE,  2) \
  X(PSYCHIC,     AF_ORB,   T_PSYCHIC,  ATK_PSYRIPPLE,ATK_NOVA,     2) \
  X(PSYCHOCUT,   AF_SLASH, T_PSYCHIC,  ATK_SLASH,    ATK_PSYRING,  3) \
  X(MEDITATE,    AF_AURA,  T_PSYCHIC,  ATK_SUN,      ATK_SPARKLE,  5) \
  X(SYNCHRONOISE,AF_WAVE,  T_PSYCHIC,  ATK_PSYRING,  ATK_RIPPLE,   3) \
  X(STRINGSHOT,  AF_WAVE,  T_BUG,      ATK_SWIRL,    ATK_RIPPLE,   3) \
  X(BUGBITE,     AF_SLASH, T_BUG,      ATK_CLAW,     ATK_SWIRL,    3) \
  X(STRUGGLEBUG, AF_WAVE,  T_BUG,      ATK_SWIRL,    ATK_RIPPLE,   3) \
  X(PINMISSILE,  AF_RAIN,  T_BUG,      ATK_SPARKLE,  ATK_CRYSTAL,  6) \
  X(ROCKTHROW,   AF_RAIN,  T_ROCK,     ATK_ROCK,     ATK_SMOKE,    4) \
  X(ROCKSLIDE,   AF_RAIN,  T_ROCK,     ATK_ROCK,     ATK_SMOKE,    5) \
  X(ANCIENTPOWER,AF_AURA,  T_ROCK,     ATK_ROCK,     ATK_BURST,    5) \
  X(ROCKPOLISH,  AF_AURA,  T_ROCK,     ATK_SUN,      ATK_SPARKLE,  5) \
  X(LICK,        AF_SLASH, T_GHOST,    ATK_SHADOW,   ATK_BLOB,     3) \
  X(SHADOWCLAW,  AF_SLASH, T_GHOST,    ATK_SLASH,    ATK_DARKCLAW, 3) \
  X(SHADOWBALL,  AF_ORB,   T_GHOST,    ATK_SHADOWBALL,ATK_PSYRING, 2) \
  X(HEX,         AF_ORB,   T_GHOST,    ATK_PSYRING,  ATK_SHADOW,   3) \
  X(DRAGONCLAW,  AF_SLASH, T_DRAGON,   ATK_SLASH,    ATK_DARKCLAW, 3) \
  X(DRAGONBREATH,AF_BREATH,T_DRAGON,   ATK_STRIKE,   ATK_BOLT,     8) \
  X(DRAGONDANCE, AF_AURA,  T_DRAGON,   ATK_SUN,      ATK_SPARKLE,  6) \
  X(OUTRAGE,     AF_STORM, T_DRAGON,   ATK_CLAW,     ATK_SLASH,    8) \
  X(BITE,        AF_SLASH, T_DARK,     ATK_CLAW,     ATK_DARKCLAW, 3) \
  X(NIGHTSLASH,  AF_SLASH, T_DARK,     ATK_SLASH,    ATK_DARKCLAW, 3) \
  X(DARKPULSE,   AF_WAVE,  T_DARK,     ATK_DARK,     ATK_PSYRING,  3) \
  X(NASTYPLOT,   AF_AURA,  T_DARK,     ATK_DARK,     ATK_SPARKLE,  5) \
  X(DARKVOID,    AF_AURA,  T_DARK,     ATK_SHADOW,   ATK_DARK,     6) \
  X(METALCLAW,   AF_SLASH, T_STEEL,    ATK_CLAW,     ATK_GEAR,     3) \
  X(IRONHEAD,    AF_SLASH, T_STEEL,    ATK_GEAR,     ATK_NOVA,     3) \
  X(MAGNETBOMB,  AF_ORB,   T_STEEL,    ATK_GEAR,     ATK_SPARKLE,  3) \
  X(GEARGRIND,   AF_ORB,   T_STEEL,    ATK_GEAR,     ATK_GEAR,     4) \
  X(AUTOTOMIZE,  AF_AURA,  T_STEEL,    ATK_GEAR,     ATK_SUN,      5) \
  X(FLASHCANNON, AF_BEAM,  T_STEEL,    ATK_STRIKE,   ATK_GEAR,     3) \
  X(MOONBLAST,   AF_ORB,   T_FAIRY,    ATK_PSYRING,  ATK_NOVA,     2) \
  X(DAZZLINGGLEAM,AF_WAVE, T_FAIRY,    ATK_SPARKLE,  ATK_NOVA,     4) \
  X(BABYDOLLEYES,AF_AURA,  T_FAIRY,    ATK_SPARKLE,  ATK_SUN,      5) \
  X(DRAININGKISS,AF_ORB,   T_FAIRY,    ATK_SPARKLE,  ATK_NOVA,     3) \
  X(JUDGMENT,    AF_BEAM,  T_NORMAL,   ATK_STRIKE,   ATK_NOVA,     3) \
  X(AEROBLAST,   AF_WAVE,  T_FLYING,   ATK_STREAK,   ATK_NOVA,     3) \
  X(SACREDFIRE,  AF_BREATH,T_FIRE,     ATK_FLAME,    ATK_FLAME2,   9) \
  X(FIREBLAST,   AF_ORB,   T_FIRE,     ATK_FLAME,    ATK_BURST,    2) \
  X(BLASTBURN,   AF_BREATH,T_FIRE,     ATK_FLAME,    ATK_FLAME2,   10) \
  X(HYDROCANNON, AF_BEAM,  T_WATER,    ATK_JET,      ATK_WATER,    3) \
  X(FRENZYPLANT, AF_BEAM,  T_GRASS,    ATK_GREEN,    ATK_SWIRL,    4) \
  X(VOLTTACKLE,  AF_SLASH, T_ELECTRIC, ATK_SPARK,    ATK_STRIKE,   3) \
  X(THUNDER,     AF_BOLT,  T_ELECTRIC, ATK_BOLT,     ATK_SPARK,    3) \
  X(DRAGONRUSH,  AF_SLASH, T_DRAGON,   ATK_DARKCLAW, ATK_NOVA,     3) \
  X(DRACOMETEOR, AF_RAIN,  T_DRAGON,   ATK_SPARKLE,  ATK_NOVA,     6) \
  X(ROAROFTIME,  AF_BEAM,  T_DRAGON,   ATK_STRIKE,   ATK_PSYRING,  3) \
  X(SPACIALREND, AF_SLASH, T_DRAGON,   ATK_SLASH,    ATK_NOVA,     3) \
  X(SHADOWFORCE, AF_AURA,  T_GHOST,    ATK_SHADOW,   ATK_DARK,     6) \
  X(MAGMASTORM,  AF_STORM, T_FIRE,     ATK_FLAME,    ATK_ROCK,     7) \
  X(CRUSHGRIP,   AF_SLASH, T_NORMAL,   ATK_CLAW,     ATK_NOVA,     3) \
  X(SEEDFLARE,   AF_ORB,   T_GRASS,    ATK_GREEN,    ATK_SUN,      2) \
  X(SEARINGSHOT, AF_ORB,   T_FIRE,     ATK_FLAME,    ATK_NOVA,     3) \
  X(VCREATE,     AF_SLASH, T_FIRE,     ATK_FLAME,    ATK_NOVA,     3) \
  X(BOLTSTRIKE,  AF_BOLT,  T_ELECTRIC, ATK_BOLT,     ATK_SPARK,    3) \
  X(BLUEFLARE,   AF_BREATH,T_FIRE,     ATK_FLAME,    ATK_JET,      10) \
  X(FUSIONFLARE, AF_ORB,   T_FIRE,     ATK_FLAME,    ATK_NOVA,     3) \
  X(FUSIONBOLT,  AF_BOLT,  T_ELECTRIC, ATK_BOLT,     ATK_SPARK,    3) \
  X(ICEBURN,     AF_BEAM,  T_ICE,      ATK_ICEBEAM,  ATK_ICESTAR,  3) \
  X(FREEZESHOCK, AF_BEAM,  T_ICE,      ATK_ICEBEAM,  ATK_CRYSTAL,  3) \
  X(TECHNOBLAST, AF_BEAM,  T_NORMAL,   ATK_STRIKE,   ATK_GEAR,     3) \
  X(RELICSONG,   AF_WAVE,  T_NORMAL,   ATK_RIPPLE,   ATK_NOVA,     3) \
  X(SECRETSWORD, AF_SLASH, T_FIGHTING, ATK_SWORD,    ATK_NOVA,     3) \
  X(PSYSTRIKE,   AF_ORB,   T_PSYCHIC,  ATK_PSYRING,  ATK_NOVA,     2) \
  X(GIGAIMPACT,  AF_BEAM,  T_NORMAL,   ATK_JET,      ATK_BURST,    3) \
  X(METEORMASH,  AF_SLASH, T_STEEL,    ATK_GEAR,     ATK_NOVA,     3) \
  X(WATERSPOUT,  AF_WAVE,  T_WATER,    ATK_WAVE,     ATK_WATER,    4) \
  X(ERUPTION,    AF_STORM, T_FIRE,     ATK_FLAME,    ATK_ROCK,     8) \
  X(STONEEDGE,   AF_RAIN,  T_ROCK,     ATK_ROCK,     ATK_SPARKLE,  5) \
  X(HURRICANE,   AF_STORM, T_FLYING,   ATK_STREAK,   ATK_RIPPLE,   8) \
  X(BLIZZARD,    AF_STORM, T_ICE,      ATK_ICEBEAM,  ATK_ICESTAR,  8) \
  X(DRAGONPULSE, AF_WAVE,  T_DRAGON,   ATK_PSYRING,  ATK_NOVA,     3) \
  X(XSCISSOR,    AF_SLASH, T_BUG,      ATK_CLAW,     ATK_SLASH,    3) \
  X(IRONTAIL,    AF_SLASH, T_STEEL,    ATK_GEAR,     ATK_CLAW,     3) \
  X(ICEFANG,     AF_SLASH, T_ICE,      ATK_ICESTAR,  ATK_CRYSTAL,  3) \
  X(CRUNCH,      AF_SLASH, T_DARK,     ATK_DARKCLAW, ATK_CLAW,     3) \
  X(AQUATAIL,    AF_SLASH, T_WATER,    ATK_WATER,    ATK_NOVA,     3) \
  X(FLY,         AF_SLASH, T_FLYING,   ATK_STREAK,   ATK_NOVA,     3) \
  X(BRAVEBIRD,   AF_SLASH, T_FLYING,   ATK_STREAK,   ATK_NOVA,     3) \
  X(SUPERPOWER,  AF_SLASH, T_FIGHTING, ATK_CLAW,     ATK_NOVA,     3) \
  X(FOCUSBLAST,  AF_ORB,   T_FIGHTING, ATK_PSYRING,  ATK_NOVA,     2) \
  X(ENERGYBALL,  AF_ORB,   T_GRASS,    ATK_GREEN,    ATK_SPARKLE,  2) \
  X(LEAFBLADE,   AF_SLASH, T_GRASS,    ATK_SLASH,    ATK_GREEN,    3) \
  X(SEEDBOMB,    AF_ORB,   T_GRASS,    ATK_GREEN,    ATK_BURST,    2) \
  X(SLUDGEWAVE,  AF_WAVE,  T_POISON,   ATK_SMOKE,    ATK_BLOB,     3) \
  X(GUNKSHOT,    AF_ORB,   T_POISON,   ATK_BLOB,     ATK_SMOKE,    2) \
  X(BUGBUZZ,     AF_WAVE,  T_BUG,      ATK_SWIRL,    ATK_RIPPLE,   3) \
  X(ZENHEADBUTT, AF_ORB,   T_PSYCHIC,  ATK_PSYRING,  ATK_NOVA,     2) \
  X(BULLDOZE,    AF_QUAKE, T_GROUND,   ATK_ROCK,     ATK_SMOKE,    3) \
  X(ICICLESPEAR, AF_RAIN,  T_ICE,      ATK_ICESTAR,  ATK_CRYSTAL,  6) \
  X(AVALANCHE,   AF_RAIN,  T_ICE,      ATK_CRYSTAL,  ATK_ICESTAR,  5) \
  X(HORNLEECH,   AF_SLASH, T_GRASS,    ATK_GREEN,    ATK_NOVA,     3) \
  X(FLAMEWHEEL,  AF_SLASH, T_FIRE,     ATK_FLAME,    ATK_NOVA,     3) \
  X(HEADCHARGE,  AF_SLASH, T_NORMAL,   ATK_BURST,    ATK_SMOKE,    2) \
  X(WILDCHARGE,  AF_SLASH,  T_ELECTRIC, ATK_SPARK,    ATK_BOLT,     3) \
  X(STRENGTH,    AF_SLASH,  T_NORMAL,   ATK_BURST,    ATK_SMOKE,    2) \
  X(TAKEDOWN,    AF_SLASH,  T_NORMAL,   ATK_STRIKE,   ATK_SMOKE,    2) \
  X(DOUBLEEDGE,  AF_SLASH,  T_NORMAL,   ATK_BURST,    ATK_NOVA,     3) \
  X(RETALIATE,   AF_SLASH,  T_NORMAL,   ATK_CLAW,     ATK_BURST,    3) \
  X(OVERHEAT,    AF_BREATH, T_FIRE,     ATK_FLAME,    ATK_NOVA,    10) \
  X(LAVAPLUME,   AF_WAVE,   T_FIRE,     ATK_FLAME,    ATK_SMOKE,    4) \
  X(FIREFANG,    AF_SLASH,  T_FIRE,     ATK_FLAME,    ATK_BURST,    3) \
  X(INCINERATE,  AF_ORB,    T_FIRE,     ATK_FLAME2,   ATK_FLAME,    3) \
  X(AQUAJET,     AF_SLASH,  T_WATER,    ATK_WATER,    ATK_STREAK,   3) \
  X(WATERFALL,   AF_SLASH,  T_WATER,    ATK_WATER,    ATK_NOVA,     3) \
  X(SCALD,       AF_BREATH, T_WATER,    ATK_WATER,    ATK_SMOKE,    8) \
  X(BRINE,       AF_ORB,    T_WATER,    ATK_WATER,    ATK_RIPPLE,   3) \
  X(WHIRLPOOL,   AF_WAVE,   T_WATER,    ATK_WAVE,     ATK_WATER,    4) \
  X(RAINDANCE,   AF_AURA,   T_WATER,    ATK_RIPPLE,   ATK_WATER,    6) \
  X(SPARK,       AF_SLASH,  T_ELECTRIC, ATK_SPARK,    ATK_BURST,    3) \
  X(CHARGEBEAM,  AF_BEAM,   T_ELECTRIC, ATK_LIGHT,    ATK_SPARK,    3) \
  X(ELECTROBALL, AF_ORB,    T_ELECTRIC, ATK_SPARK,    ATK_BOLT,     3) \
  X(VOLTSWITCH,  AF_SLASH,  T_ELECTRIC, ATK_SPARK,    ATK_STREAK,   3) \
  X(GIGADRAIN,   AF_ORB,    T_GRASS,    ATK_GREEN,    ATK_SPARKLE,  3) \
  X(PETALDANCE,  AF_STORM,  T_GRASS,    ATK_GREEN,    ATK_SWIRL,    8) \
  X(WOODHAMMER,  AF_SLASH,  T_GRASS,    ATK_GREEN,    ATK_BURST,    3) \
  X(GRASSKNOT,   AF_SLASH,  T_GRASS,    ATK_GREEN,    ATK_SWIRL,    3) \
  X(AURORABEAM,  AF_BEAM,   T_ICE,      ATK_ICEBEAM,  ATK_ICESTAR,  3) \
  X(POWDERSNOW,  AF_BREATH, T_ICE,      ATK_CRYSTAL,  ATK_ICESTAR,  8) \
  X(FROSTBREATH, AF_BREATH, T_ICE,      ATK_ICEBEAM,  ATK_CRYSTAL,  8) \
  X(KARATECHOP,  AF_SLASH,  T_FIGHTING, ATK_CLAW,     ATK_BURST,    3) \
  X(CROSSCHOP,   AF_SLASH,  T_FIGHTING, ATK_CLAW,     ATK_NOVA,     3) \
  X(LOWSWEEP,    AF_SLASH,  T_FIGHTING, ATK_CLAW,     ATK_SMOKE,    3) \
  X(DYNAMICPUNCH,AF_ORB,    T_FIGHTING, ATK_CLAW,     ATK_NOVA,     2) \
  X(VITALTHROW,  AF_SLASH,  T_FIGHTING, ATK_CLAW,     ATK_BURST,    3) \
  X(SLUDGE,      AF_ORB,    T_POISON,   ATK_POISON,   ATK_BLOB,     2) \
  X(VENOSHOCK,   AF_ORB,    T_POISON,   ATK_BLOB,     ATK_POISON,   3) \
  X(CROSSPOISON, AF_SLASH,  T_POISON,   ATK_BLOB,     ATK_SLASH,    3) \
  X(POISONTAIL,  AF_SLASH,  T_POISON,   ATK_BLOB,     ATK_SLASH,    3) \
  X(DIG,         AF_SLASH,  T_GROUND,   ATK_ROCK,     ATK_SMOKE,    3) \
  X(MUDSHOT,     AF_ORB,    T_GROUND,   ATK_ROCK,     ATK_BLOB,     3) \
  X(SANDTOMB,    AF_WAVE,   T_GROUND,   ATK_ROCK,     ATK_SMOKE,    4) \
  X(DRILLRUN,    AF_SLASH,  T_GROUND,   ATK_ROCK,     ATK_BURST,    3) \
  X(PECK,        AF_SLASH,  T_FLYING,   ATK_STREAK,   ATK_SLASH,    3) \
  X(DRILLPECK,   AF_SLASH,  T_FLYING,   ATK_STREAK,   ATK_BURST,    3) \
  X(SKYATTACK,   AF_BEAM,   T_FLYING,   ATK_STREAK,   ATK_BURST,    3) \
  X(ACROBATICS,  AF_SLASH,  T_FLYING,   ATK_STREAK,   ATK_NOVA,     3) \
  X(PSYBEAM,     AF_BEAM,   T_PSYCHIC,  ATK_PSYRING,  ATK_NOVA,     3) \
  X(EXTRASENSORY,AF_ORB,    T_PSYCHIC,  ATK_PSYRING,  ATK_SPARKLE,  2) \
  X(FUTURESIGHT, AF_ORB,    T_PSYCHIC,  ATK_PSYRIPPLE,ATK_NOVA,     2) \
  X(DREAMEATER,  AF_ORB,    T_PSYCHIC,  ATK_PSYRIPPLE,ATK_SHADOW,   3) \
  X(STOREDPOWER, AF_AURA,   T_PSYCHIC,  ATK_PSYRING,  ATK_SPARKLE,  5) \
  X(MIRRORCOAT,  AF_WAVE,   T_PSYCHIC,  ATK_PSYRIPPLE,ATK_NOVA,     4) \
  X(SIGNALBEAM,  AF_BEAM,   T_BUG,      ATK_SWIRL,    ATK_RIPPLE,   3) \
  X(SILVERWIND,  AF_WAVE,   T_BUG,      ATK_SWIRL,    ATK_SPARKLE,  4) \
  X(UTURN,       AF_SLASH,  T_BUG,      ATK_CLAW,     ATK_STREAK,   3) \
  X(FURYCUTTER,  AF_SLASH,  T_BUG,      ATK_CLAW,     ATK_SLASH,    4) \
  X(LEECHLIFE,   AF_ORB,    T_BUG,      ATK_GREEN,    ATK_SWIRL,    3) \
  X(SMACKDOWN,   AF_RAIN,   T_ROCK,     ATK_ROCK,     ATK_SMOKE,    4) \
  X(HEADSMASH,   AF_SLASH,  T_ROCK,     ATK_ROCK,     ATK_BURST,    3) \
  X(ROCKBLAST,   AF_RAIN,   T_ROCK,     ATK_ROCK,     ATK_SPARKLE,  5) \
  X(SHADOWSNEAK, AF_SLASH,  T_GHOST,    ATK_SHADOW,   ATK_SLASH,    3) \
  X(OMINOUSWIND, AF_WAVE,   T_GHOST,    ATK_SHADOW,   ATK_PSYRING,  4) \
  X(NIGHTMARE,   AF_AURA,   T_GHOST,    ATK_SHADOW,   ATK_DARK,     6) \
  X(DRAGONTAIL,  AF_SLASH,  T_DRAGON,   ATK_STRIKE,   ATK_NOVA,     3) \
  X(TWISTER,     AF_WAVE,   T_DRAGON,   ATK_STRIKE,   ATK_RIPPLE,   4) \
  X(PAYBACK,     AF_SLASH,  T_DARK,     ATK_DARKCLAW, ATK_BURST,    3) \
  X(FOULPLAY,    AF_SLASH,  T_DARK,     ATK_DARKCLAW, ATK_NOVA,     3) \
  X(SUCKERPUNCH, AF_SLASH,  T_DARK,     ATK_DARKCLAW, ATK_STRIKE,   3) \
  X(BEATUP,      AF_RAIN,   T_DARK,     ATK_DARKCLAW, ATK_SMOKE,    5) \
  X(GYROBALL,    AF_ORB,    T_STEEL,    ATK_GEAR,     ATK_NOVA,     3) \
  X(BULLETPUNCH, AF_SLASH,  T_STEEL,    ATK_GEAR,     ATK_STRIKE,   3) \
  X(HEAVYSLAM,   AF_SLASH,  T_STEEL,    ATK_GEAR,     ATK_SMOKE,    2) \
  X(STEELWING,   AF_SLASH,  T_STEEL,    ATK_GEAR,     ATK_STREAK,   3) \
  X(DISARMINGVOICE,AF_WAVE, T_FAIRY,    ATK_SPARKLE,  ATK_PSYRING,  4) \
  X(PLAYROUGH,   AF_SLASH,  T_FAIRY,    ATK_SPARKLE,  ATK_NOVA,     3) \
  X(SWEETKISS,   AF_AURA,   T_FAIRY,    ATK_SPARKLE,  ATK_PSYRING,  5)


enum MoveId : uint8_t {
#define X(n, f, t, a, b, c) M_##n,
  MOVE_LIST(X)
#undef X
  M_COUNT
};

static const MoveDef MOVES[M_COUNT] = {
#define X(n, f, t, a, b, c) {#n, f, t, a, b, c},
  MOVE_LIST(X)
#undef X
};

static const char *FAM_NAME[AF_COUNT] = {
  "BEAM", "BREATH", "BOLT", "ORB", "SLASH", "RAIN", "QUAKE", "AURA", "WAVE",
  "STORM"
};

static const uint8_t POOL_NORMAL[]   = {M_TACKLE, M_SCRATCH, M_FURYSWIPES, M_BODYSLAM, M_HYPERBEAM, M_HYPERVOICE, M_SWIFT, 0xFF};
static const uint8_t POOL_FIRE[]     = {M_EMBER, M_FLAMETHROWER, M_FLAREBLITZ, M_WILLOWISP, M_FIREPUNCH, M_HEATWAVE, 0xFF};
static const uint8_t POOL_WATER[]    = {M_WATERGUN, M_BUBBLE, M_BUBBLEBEAM, M_HYDROPUMP, M_AQUARING, M_SURF, M_CRABHAMMER, 0xFF};
static const uint8_t POOL_ELECTRIC[] = {M_THUNDERSHOCK, M_THUNDERBOLT, M_THUNDERWAVE, M_THUNDERPUNCH, M_DISCHARGE, 0xFF};
static const uint8_t POOL_GRASS[]    = {M_VINEWHIP, M_RAZORLEAF, M_LEAFSTORM, M_INGRAIN, M_BULLETSEED, M_SOLARBEAM, M_MEGADRAIN, M_SPORE, 0xFF};
static const uint8_t POOL_ICE[]      = {M_ICESHARD, M_ICEBEAM, M_ICICLECRASH, M_ICYWIND, M_ICEPUNCH, M_MIST, 0xFF};
static const uint8_t POOL_FIGHTING[] = {M_CLOSECOMBAT, M_AURASPHERE, M_BULKUP, 0xFF};
static const uint8_t POOL_POISON[]   = {M_POISONJAB, M_SLUDGEBOMB, M_TOXIC, M_POISONGAS, M_ACID, 0xFF};
static const uint8_t POOL_GROUND[]   = {M_EARTHQUAKE, M_EARTHPOWER, M_BONERUSH, M_MUDSPORT, 0xFF};
static const uint8_t POOL_FLYING[]   = {M_GUST, M_WINGATTACK, M_AIRSLASH, M_AIRCUTTER, M_ROOST, M_DRAGONASCENT, 0xFF};
static const uint8_t POOL_PSYCHIC[]  = {M_CONFUSION, M_PSYCHIC, M_PSYCHOCUT, M_MEDITATE, M_SYNCHRONOISE, 0xFF};
static const uint8_t POOL_BUG[]      = {M_STRINGSHOT, M_BUGBITE, M_STRUGGLEBUG, M_PINMISSILE, 0xFF};
static const uint8_t POOL_ROCK[]     = {M_ROCKTHROW, M_ROCKSLIDE, M_ANCIENTPOWER, M_ROCKPOLISH, 0xFF};
static const uint8_t POOL_GHOST[]    = {M_LICK, M_SHADOWCLAW, M_SHADOWBALL, M_HEX, 0xFF};
static const uint8_t POOL_DRAGON[]   = {M_DRAGONCLAW, M_DRAGONBREATH, M_DRAGONDANCE, M_OUTRAGE, 0xFF};
static const uint8_t POOL_DARK[]     = {M_BITE, M_NIGHTSLASH, M_DARKPULSE, M_NASTYPLOT, M_DARKVOID, 0xFF};
static const uint8_t POOL_STEEL[]    = {M_METALCLAW, M_IRONHEAD, M_MAGNETBOMB, M_GEARGRIND, M_AUTOTOMIZE, M_FLASHCANNON, 0xFF};
static const uint8_t POOL_FAIRY[]    = {M_MOONBLAST, M_DAZZLINGGLEAM, M_BABYDOLLEYES, M_DRAININGKISS, 0xFF};

static const uint8_t *const MOVE_POOL[T_COUNT] = {
  POOL_NORMAL, POOL_FIRE, POOL_WATER, POOL_ELECTRIC, POOL_GRASS, POOL_ICE,
  POOL_FIGHTING, POOL_POISON, POOL_GROUND, POOL_FLYING, POOL_PSYCHIC,
  POOL_BUG, POOL_ROCK, POOL_GHOST, POOL_DRAGON, POOL_DARK, POOL_STEEL,
  POOL_FAIRY
};

static uint8_t moveIdx = 0;
static bool atkActive = false;
static bool atkLabel = false;
static float atkP = 0.0f;
static float atkFlash = 0.0f;

static uint8_t pickMove(int n, int8_t t1, int8_t t2) {
  // The generated table is one real move per species, not a pool draw. The
  // string lookup is once per entry and costs less than a frame's worth of time
  // across the whole 649-entry pass.
  if (n >= 1 && n <= DEX_COUNT) {
    const char *want = SIG[n - 1].move;
    if (want && want[0]) {
      for (uint8_t i = 0; i < M_COUNT; i++)
        if (strcmp(MOVES[i].name, want) == 0) return i;
    }
  }
  // A species with no matching effect still gets a type-appropriate move.
  int8_t t = t1;
  if (t < 0 || t >= T_COUNT) t = T_NORMAL;
  if (t == T_NORMAL && t2 >= 0 && t2 < T_COUNT) t = t2;
  const uint8_t *p = MOVE_POOL[t];
  int k = 0;
  while (p[k] != 0xFF) k++;
  return p[hash32((uint32_t)n * 2654435761u + (uint32_t)t * 97u) % (uint32_t)k];
}

// ------------------------------------------------------------ the move engine ---

// Origin at the creature's mouth, clamped so a Wailord-sized sprite does not put
// the launch point past the target.
// After the global mirror, the mouth/front is the right-hand edge of the box.
// The small per-species offset stops every creature from launching at exactly
// the same point, without putting the origin behind the silhouette.
static int atkOriginX() {
  int x = gX + (int)(gW * (0.72f + 0.06f * fxA));
  if (x < 120) x = 120;
  if (x > TARGET_X - 18) x = TARGET_X - 18;
  return x;
}
static int atkOriginY() {
  int y = gY + (int)(gH * (0.27f + 0.06f * fxB));
  if (y < 22) y = 22;
  if (y > 104) y = 104;
  return y;
}
// Beams, breaths and bolts launch from the middle of the head, which is lower
// than the old 28%-from-the-top point on long-necked sprites like Gyarados.
static int atkBeamY() {
  int y = gY + (int)(gH * (0.38f + 0.06f * fxC));
  if (y < 30) y = 30;
  if (y > 104) y = 104;
  return y;
}

static int atkTargetX() { return TARGET_X; }
static int atkTargetY() { return TARGET_Y; }

// The one blitter. Source rows and columns map onto the destination box, equal
// nibbles become one fillRect, and the palette is shifted toward `tint`, then
// toward white for a flash, then dimmed. All four are needed: tint ties an
// effect to the move's type, flash is the hit, dim is the fade.
static void drawAtk(uint8_t id, int x, int y, int w, int h,
                    uint16_t tint, float mix, float flash, float bright) {
  if (id >= ATK_COUNT || w <= 0 || h <= 0) return;
  const AtkTex &a = ATK_TEX[id];
  if (a.w == 0 || a.h == 0) return;
  const int stride = (a.w + 1) / 2;
  const uint8_t *data = &ATK_DATA[a.off];
  for (int dy = 0; dy < h; dy++) {
    const int py = y + dy;
    if (py < 0 || py >= HUD_Y) continue;
    const int sy = (int)((int32_t)dy * a.h / h);
    if (sy >= a.h) continue;
    const uint8_t *row = data + (size_t)sy * stride;
    int runStart = 0;
    uint8_t runVal = 0xFF;
    for (int sx = 0; sx <= a.w; sx++) {
      uint8_t v = 0;
      if (sx < a.w) {
        const uint8_t pack = row[sx >> 1];
        v = (sx & 1) ? (pack & 0x0F) : (pack >> 4);
      }
      if (v == runVal) continue;
      if (runVal != 0xFF && runVal != 0 && sx > runStart) {
        const int x0 = x + (int)((int32_t)runStart * w / a.w);
        const int x1 = x + (int)((int32_t)sx * w / a.w);
        if (x1 > x0) {
          int cx0 = x0 < 0 ? 0 : x0;
          int cx1 = x1 > W ? W : x1;
          if (cx1 > cx0) {
            uint16_t c = a.pal[runVal - 1];
            if (mix > 0.0f) c = lerp565(c, tint, mix);
            if (bright < 0.999f) c = lerp565(0x0000, c, bright);
            if (flash > 0.001f) c = lerp565(c, 0xFFFF, flash);
            fb.fillRect(cx0, py, cx1 - cx0, 1, c);
          }
        }
      }
      runStart = sx;
      runVal = v;
    }
  }
}

static void drawAtkCenter(uint8_t id, int cx, int cy, float scale,
                          uint16_t tint, float mix, float flash, float bright) {
  const AtkTex &a = ATK_TEX[id];
  int w = (int)(a.w * scale), h = (int)(a.h * scale);
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  drawAtk(id, cx - w / 2, cy - h / 2, w, h, tint, mix, flash, bright);
}

// Rotated copy of one of the archived effect sheets. Slashes and the Vine Whip
// lash are directional art; drawing them upright loses the shape the move is
// supposed to have. This is nearest-neighbour, like the rest of the renderer.
static void drawAtkRot(uint8_t id, int cx, int cy, int w, int h, float ang,
                       uint16_t tint, float mix, float flash, float bright) {
  if (id >= ATK_COUNT || w < 1 || h < 1) return;
  const AtkTex &a = ATK_TEX[id];
  if (a.w == 0 || a.h == 0) return;
  const int x = cx - w / 2, y = cy - h / 2;
  const int stride = (a.w + 1) / 2;
  const float ca = cosf(ang), sa = sinf(ang);
  for (int dy = 0; dy < h; dy++) {
    const int py = y + dy;
    if (py < 0 || py >= HUD_Y) continue;
    for (int dx = 0; dx < w; dx++) {
      const int px = x + dx;
      if (px < 0 || px >= W) continue;
      const float u = (dx - w * 0.5f) * a.w / (float)w;
      const float v = (dy - h * 0.5f) * a.h / (float)h;
      const int sx = (int)(u * ca + v * sa + a.w * 0.5f);
      const int sy = (int)(-u * sa + v * ca + a.h * 0.5f);
      if (sx < 0 || sx >= a.w || sy < 0 || sy >= a.h) continue;
      const uint8_t pack = ATK_DATA[a.off + (size_t)sy * stride + (sx >> 1)];
      const uint8_t pv = (sx & 1) ? (pack & 0x0F) : (pack >> 4);
      if (pv == 0) continue;
      uint16_t c = a.pal[pv - 1];
      if (mix > 0.0f) c = lerp565(c, tint, mix);
      if (bright < 0.999f) c = lerp565(0x0000, c, bright);
      if (flash > 0.001f) c = lerp565(c, 0xFFFF, flash);
      fb.drawPixel(px, py, c);
    }
  }
}

// Concentric outlines are the cheapest glow this panel has: no fill, no alpha,
// no read-back, just two ellipses per ring. The dim outer ring is what makes a
// charge look like it is lighting the air rather than sitting in it.
static void glowRing(int cx, int cy, int rx, int ry, uint16_t colour, float bright) {
  if (rx < 2 || ry < 2 || bright <= 0.01f) return;
  // Never fade a ring all the way to black. On a bright impact or a pale
  // backdrop a 10%-bright ring became a dark cage, which is the opposite of
  // glow. Keep a floor of 38% of the hue so the dimmest ring still reads as
  // coloured light rather than a black outline.
  const float b = bright < 0.38f ? 0.38f : (bright > 1.0f ? 1.0f : bright);
  fb.drawEllipse(cx, cy, rx, ry, lerp565(0x0000, colour, b));
  if (rx > 5 && ry > 4) {
    const float b2 = b * 0.65f < 0.28f ? 0.28f : b * 0.65f;
    fb.drawEllipse(cx, cy, rx - 2, ry - 1, lerp565(0x0000, colour, b2));
  }
}

// Light pooling on the ground under the creature: three flattened rings, cheap
// enough to draw every attack frame and strong enough to anchor the move.
static void glowFloor(uint16_t colour, float strength) {
  const int cx = gX + gW / 2;
  const uint16_t light = lerp565(colour, 0xFFFF, 0.22f);
  for (int i = 0; i < 3; i++) {
    glowRing(cx, GROUND_Y + 2, (int)(gW * 0.38f + 30 * i), 4 + i * 3,
             light, (0.34f - 0.07f * i) * strength);
  }
  glowRing(cx, GROUND_Y + 4, 20, 5, light, 0.30f * strength);
}

// The flash at the mouth or hand at the instant the move leaves. Small, but it
// is what turns a texture appearing at a point into something being fired.
static void muzzleFlash(int x, int y, uint16_t tint, float power) {
  glowStar(x, y, (int)(7 + 18 * power), tint, 0.92f);
  glowRing(x, y, (int)(9 + 24 * power), (int)(9 + 24 * power), tint, 0.55f * power);
  glowRing(x, y, (int)(15 + 34 * power), (int)(15 + 34 * power), tint, 0.22f * power);
}

// A layered halo around a point. Three rings, each wider and dimmer than the
// last, give the effects a soft bloom without any alpha blending: the panel
// simply draws one-pixel outlines, and the eye fills the gap.
static void glowHalo(int cx, int cy, int r, uint16_t colour, float bright) {
  // Two rings are enough for a halo. A third, dimmer ring looked like an
  // outline once several effects overlapped at the target.
  glowRing(cx, cy, r, r, colour, bright);
  glowRing(cx, cy, r + 4, r + 4, colour, bright * 0.58f);
}

// A cheap shaded ball. The offset dark disc is the shadowed lower-right rim,
// the body sits on top, and the upper-left cap plus one specular dot give the
// eye a light direction. Four fills read as a sphere; a flat disc does not.
static void shadeDisc(int cx, int cy, int r, uint16_t base, float spec) {
  if (r < 3) {
    fb.fillCircle(cx, cy, r, base);
    return;
  }
  const uint16_t lo = lerp565(base, 0x0000, 0.40f);
  const uint16_t hi = lerp565(base, 0xFFFF, 0.34f);
  fb.fillCircle(cx + 1, cy + 1, r, lo);
  fb.fillCircle(cx, cy, r - 1, base);
  fb.fillCircle(cx - r / 3, cy - r / 3, (r * 3) / 5, hi);
  if (spec > 0.01f) {
    const int s = r / 4 < 1 ? 1 : r / 4;
    fb.fillCircle(cx - r / 3, cy - r / 3, s, lerp565(hi, 0xFFFF, spec));
  }
}

// The battle floor rises from the player's feet to the opponent's platform, so
// a projectile's shadow follows that perspective line instead of sitting on the
// player's floor all the way across the screen.
static int battleGroundAt(int x) {
  if (x <= CX) return GROUND_Y;
  if (x >= TARGET_X) return TARGET_GROUND_Y;
  return GROUND_Y - (int)((x - CX) * (GROUND_Y - TARGET_GROUND_Y) / (TARGET_X - CX));
}

// A contact shadow on the floor under a flying effect. This is the cheapest
// depth cue in the whole renderer: if the projectile has a shadow on the ground
// below it, the eye reads it as being above the ground, not painted on it.
static void castShadow(int x, int y, int w) {
  const int gy = battleGroundAt(x);
  const int h = gy - y;
  if (h < 8) return;
  int sw = w + h / 10;
  if (sw < 3) sw = 3;
  if (sw > 34) sw = 34;
  fb.fillEllipse(x + 2, gy + 1, sw, sw / 4 + 1,
                 lerp565(floorTint, 0x000000, 0.34f));
}

// A drawn star instead of a scaled impact sheet. The sheets carry a baked dark
// outline, and at impact scale that outline became a black cage around the
// opponent. Four long rays, four short ones and a white core read as light and
// cost only a handful of lines.
static void glowStar(int cx, int cy, int r, uint16_t tint, float bright) {
  if (r < 3 || bright <= 0.01f) return;
  glowHalo(cx, cy, r, tint, bright * 0.55f);
  const uint16_t hot = lerp565(tint, 0xFFFF, bright);
  for (int k = 0; k < 8; k++) {
    const float a = k * 0.7854f;
    const int rr = (k & 1) ? (r * 3) / 4 : (int)(r * 1.35f);
    fb.drawLine(cx, cy, cx + (int)(cosf(a) * rr), cy + (int)(sinf(a) * rr), hot);
  }
  const int core = r / 3 < 2 ? 2 : r / 3;
  fb.fillCircle(cx, cy, core, 0xFFFF);
}

// Draw the archived Ditto sprite. No face, arms or body are invented here: the
// only transforms are scale, the faint squash and the white hit flash. Ditto's
// front sprite faces left, which is already the correct direction on the right
// side of the field.
static void blitDitto(int cx, int bottom, float squash, float flash) {
  const SprInfo &s = SPRITES[DITTO_SPRITE];
  if (s.w == 0 || s.h == 0) return;
  const int w = (int)(s.w * 1.75f * (squash < 0.55f ? 1.20f : 1.0f));
  int h = (int)(s.h * 1.75f * squash);
  if (h < 8) h = 8;
  const int x = cx - w / 2;
  const int y = bottom - h;
  const int stride = (s.w + 1) / 2;
  const uint8_t *data = &SPR_DATA[s.off];

  for (int i = 0; i <= s.w; i++) srcMap[i] = i * w / s.w;
  for (int i = 0; i < h; i++) rowMap[i] = i * s.h / h;

  for (int yy = 0; yy < h; yy++) {
    const int py = y + yy;
    if (py < 0 || py >= HUD_Y) continue;
    const uint8_t *row = data + (size_t)rowMap[yy] * stride;
    int runStart = 0;
    uint8_t runVal = 0xFF;
    for (int sx = 0; sx <= s.w; sx++) {
      uint8_t v = 0;
      if (sx < s.w) {
        const uint8_t pack = row[sx >> 1];
        v = (sx & 1) ? (pack & 0x0F) : (pack >> 4);
      }
      if (v == runVal) continue;
      if (runVal != 0xFF && runVal != 0 && sx > runStart) {
        const int x0 = x + srcMap[runStart];
        const int x1 = sx < s.w ? x + srcMap[sx] : x + w;
        if (x1 > x0) {
          uint16_t c = runVal == SPR_OUTLINE ? C(0x0A0810) : s.pal[runVal - 1];
          if (flash > 0.001f) c = lerp565(c, 0xFFFF, flash);
          fb.fillRect(x0, py, x1 - x0, 1, c);
        }
      }
      runStart = sx;
      runVal = v;
    }
  }
}

// The opponent: the actual Ditto sprite on its raised platform. The hit flash
// and faint squash are transforms on the real artwork, not a replacement sketch.
static void drawTarget() {
  const uint32_t now = millis();
  const int wob = (int)(sinf(animT * 61.0f) * (0.7f + 3.8f * targetHit));
  const int x = TARGET_X + wob;
  const bool fainted = dittoFaint && dittoHp <= 0;

  // The opponent platform is higher than the player's floor, which is the
  // simplest way to keep the classic battle perspective on a small panel.
  fb.fillEllipse(TARGET_X, TARGET_GROUND_Y + 2, 35, 7,
                 lerp565(scene.ground, 0x000000, 0.34f));
  fb.drawEllipse(TARGET_X, TARGET_GROUND_Y + 2, 35, 7,
                 lerp565(scene.ground, look.accent, 0.34f));

  // A fainted Ditto is the same sprite squashed onto the platform. The artwork
  // stays recognisable; only its height and width change.
  if (fainted) {
    const float f = clamp01((now - dittoFaintAt) / 620.0f);
    blitDitto(TARGET_X + (int)(8.0f * f), TARGET_GROUND_Y + 1,
              0.28f - 0.10f * f, 0.0f);
    return;
  }

  // A fresh Ditto drops onto the platform after the previous one faints. The
  // short fall is enough to read as a replacement without a full Poké Ball
  // animation on a 6-second cycle.
  float respawn = 1.0f;
  if (dittoRespawnAt != 0 && now - dittoRespawnAt < 520) {
    const float q = clamp01((now - dittoRespawnAt) / 520.0f);
    respawn = easeOutCubic(q);
  }
  const int drop = (int)((1.0f - respawn) * 34.0f);
  const int bob = (int)(sinf(animT * 2.2f) * 0.8f);

  // Rings and sparks sit behind the sprite. They are impact light, not a cage
  // drawn over the opponent's face.
  if (atkActive) {
    const uint16_t tint = C(TYPE_STYLE[MOVES[moveIdx].type].rgb);
    if (targetHit < 0.15f)
      glowRing(x, TARGET_Y, 18, 18, tint, 0.11f + 0.06f * sinf(animT * 18.0f));
    glowRing(TARGET_X, TARGET_GROUND_Y - 2, 28, 6, tint, 0.18f);
  }
  if (targetHit > 0.01f) {
    glowRing(x, TARGET_Y, 13 + (int)(10.0f * targetHit),
             13 + (int)(10.0f * targetHit), 0xFFFF, 0.55f * targetHit);
    for (int i = 0; i < 7; i++) {
      const float a = i * 0.8976f + animT * 9.0f;
      const int rr = 11 + (int)(19.0f * targetHit);
      fb.fillCircle(x + (int)(cosf(a) * rr), TARGET_Y + (int)(sinf(a) * rr * 0.7f),
                    1 + (i & 1), lerp565(C(0x9A5CFF), 0xFFFF, 0.7f));
    }
  }

  blitDitto(x, TARGET_GROUND_Y - 2 - drop + bob, 1.0f, 0.42f * targetHit);
}

// Opponent HP bar, above Ditto. It is deliberately small and out of the way so
// the battle scene still reads before the HUD does.
static void drawDittoHp() {
  const int px = 216, py = 38, pw = 100, ph = 20;
  fb.fillRoundRect(px, py, pw, ph, 3, C(0x0B0910));
  fb.drawRoundRect(px, py, pw, ph, 3, lerp565(C(0x2A2438), look.accent, 0.55f));
  fb.setFont(&fonts::Font0);
  fb.setTextColor(C(0xF4F0FA));
  fb.drawString("DITTO", px + 5, py + 2);
  char hp[12];
  if (dittoHp == 0) snprintf(hp, sizeof(hp), "FAINTED");
  else snprintf(hp, sizeof(hp), "%d", dittoHp);
  fb.setTextColor(dittoHp == 0 ? C(0xFF6A5A) : C(0x8A82A0));
  fb.drawString(hp, px + pw - 5 - fb.textWidth(hp), py + 2);
  const int bx = px + 5, by = py + 12, bw = pw - 10, bh = 5;
  fb.fillRect(bx, by, bw, bh, C(0x241C32));
  const int fill = (bw * dittoHp) / DITTO_HP_MAX;
  const uint16_t col = dittoHp > DITTO_HP_MAX / 2 ? C(0x66D86A)
                       : (dittoHp > DITTO_HP_MAX / 5 ? C(0xE6C84A) : C(0xE0564A));
  if (fill > 0) fb.fillRect(bx, by, fill, bh, col);
  fb.drawRect(bx, by, bw, bh, C(0x3A3050));
}

// Foreground player platform. The painted battleback supplies the ground; this
// adds the raised disc that makes the two Pokémon read as opposing sides.
static void drawMainPlatform() {
  const int cy = GROUND_Y + 2;
  fb.fillEllipse(CX, cy, 56, 8, lerp565(scene.ground, 0x000000, 0.28f));
  fb.drawEllipse(CX, cy, 56, 8, lerp565(scene.ground, look.accent, 0.30f));
  fb.drawEllipse(CX, cy, 48, 6, lerp565(scene.ground, look.accent, 0.16f));
}

// The shared hit: a drawn starburst, a hot core, expanding rings and sparks.
// Every family lands on this, which is what makes a beam, a shadow ball and a
// rock slide feel like they hit the same world.
static void atkImpact(const MoveDef &m, int x, int y, float q) {
  q = clamp01(q);
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  targetHit = fmaxf(targetHit, 0.88f * (1.0f - 0.55f * q));
  // Contact moves already put fists or claws on top of Ditto. A filled type-
  // coloured disc would hide the sprite, so slash/fist hits get perimeter
  // sparks only and leave the real Ditto artwork readable.
  if (m.fam == AF_SLASH) {
    for (int i = 0; i < 10; i++) {
      const uint32_t h = hash32((uint32_t)i * 37u + (uint32_t)(q * 211.0f));
      const float a = (h & 0xFF) / 255.0f * 6.2832f;
      const int rr = (int)(15 + 34 * q);
      fb.fillCircle(x + (int)(cosf(a) * rr), y + (int)(sinf(a) * rr * 0.55f),
                    1 + (int)((h >> 8) & 1), lerp565(tint, 0xFFFF, 0.82f));
    }
    atkFlash = fmaxf(atkFlash, 0.45f * (1.0f - q));
    return;
  }
  // A drawn burst, not the impact sheet: its black outline used to cage the
  // opponent on every beam, bolt and orb hit.
  const int rr = 8 + (int)(22.0f * q);
  // A deeper, offset copy under the white core gives the burst a front face and
  // a receding rim. It is a dark tint, not black, so no cage appears.
  const uint16_t deep = lerp565(tint, 0x0000, 0.22f);
  fb.fillCircle(x + 3, y + 3, rr, deep);
  // Two rings around the core, then short outward spikes. The old star drew
  // eight long rays through the same space as the rings; where they crossed
  // they made a black spiderweb over Ditto. Spikes outside the core keep
  // the burst readable without a lattice.
  // Slashes already carry their own curved marks; adding the full ring stack
  // over a small opponent turned the burst into a wireframe. Give that family
  // the core and spikes only.
  if (m.fam != AF_SLASH)
    fb.drawEllipse(x, y, rr + 4, rr + 4, lerp565(tint, 0xFFFF, 0.72f));
  shadeDisc(x, y, rr, lerp565(tint, 0xFFFF, 0.38f), 0.95f);
  // The spikes begin outside the outermost ring. Starting them at the core
  // made them cross both rings, and that grid was the black web over Ditto.
  const uint16_t ray = lerp565(tint, 0xFFFF, 0.80f);
  for (int k = 0; k < 8; k++) {
    const float a = k * 0.7854f + 0.3927f;
    const int r0 = rr + 10;
    const int r1 = rr + 15 + (k & 1 ? 4 : 0);
    fb.drawLine(x + (int)(cosf(a) * r0), y + (int)(sinf(a) * r0),
                x + (int)(cosf(a) * r1), y + (int)(sinf(a) * r1), ray);
  }
  // One ground shock ring gives the blast a floor contact; the shaded core and
  // short spikes do the volume. Extra vertical rings turned the target into a
  // spiderweb when a slash crossed them.
  if (m.fam != AF_SLASH)
    fb.drawEllipse(x, y, (int)(8 + 52 * q), (int)(3 + 18 * q),
                   lerp565(tint, 0xFFFF, 0.42f * (1.0f - q)));
  // The shockwave reaches the floor a moment after the hit. That vertical gap
  // is what makes the opponent look like it is standing above the ground plane.
  if (q > 0.18f) {
    const int sq = (int)(14 + 48 * q);
    glowRing(x, TARGET_GROUND_Y, sq, 4 + sq / 8, tint, 0.26f * (1.0f - q));
  }
  for (int i = 0; i < 14; i++) {
    const uint32_t h = hash32((uint32_t)i * 37u + (uint32_t)(q * 211.0f));
    const float a = (h & 0xFF) / 255.0f * 6.2832f;
    const int rr = (int)(8 + 52 * q);
    fb.fillCircle(x + (int)(cosf(a) * rr), y + (int)(sinf(a) * rr * 0.6f),
                  1 + (int)((h >> 8) & 1), lerp565(tint, 0xFFFF, 0.65f));
  }
  atkFlash = fmaxf(atkFlash, 0.65f * (1.0f - q));
}

// A tapered, three-layer lance of light. The old beam was a move texture stretched
// across the gap, which is exactly why it read as a rectangular bar: no amount of
// palette work hides an outline that was painted for a 56-pixel sprite. Building
// the beam from tapered spans gives it a hot white core, a coloured body and an
// air-glow edge whose width follows the shot instead of a texture's bounding box.
static void beamLance(int x0, int y0, int x1, int y1, float base,
                      uint16_t tint, float t) {
  const int len = x1 - x0;
  if (len < 4) return;
  for (int i = 0; i <= len; i += 2) {
    const float q = (float)i / (float)len;
    const float taper = 0.12f + 0.88f * powf(sinf(3.14159f * q), 0.70f);
    const float wob = 1.0f + 0.16f * sinf(q * 13.0f - t * 42.0f);
    int half = (int)(base * taper * wob);
    if (half < 2) half = 2;
    const int y = y0 + (int)((y1 - y0) * q);
    // Three visible layers plus an outer bloom edge. Not a black outline: on a
    // bright sky that reads as a brown border, so the widest pass stays close
    // to the move colour and only the thin edge is pushed toward white. The hot
    // core sits one pixel above centre and the lower edge is a deeper tint, so
    // the beam reads as a lit cylinder instead of a symmetric flat stripe.
    const int coreY = y - 1;
    const uint16_t shade = lerp565(tint, 0x0000, 0.22f);
    fb.fillRect(x0 + i, y - half - 3, 2, half * 2 + 6, lerp565(tint, 0xFFFF, 0.12f));
    fb.fillRect(x0 + i, y - half - 2, 2, half * 2 + 4, lerp565(tint, 0xFFFF, 0.24f));
    fb.fillRect(x0 + i, y - half, 2, half * 2, lerp565(tint, 0xFFFF, 0.38f));
    fb.fillRect(x0 + i, y - (int)(half * 0.55f), 2, (int)(half * 1.1f),
                lerp565(tint, 0xFFFF, 0.58f));
    fb.fillRect(x0 + i, y + half, 2, 1, shade);
    const int core = 2;
    fb.fillRect(x0 + i, coreY - core, 2, core * 2 + 1,
                lerp565(tint, 0xFFFF, 0.97f));
    fb.fillRect(x0 + i, y - half - 1, 2, 1, lerp565(tint, 0xFFFF, 0.72f));
  }
}

// --- BEAM: a charge at the mouth, a tapered lance, moving rings, a hit
static void drawBeam(const MoveDef &m, float p, float t) {
  const int ox = atkOriginX(), oy = atkBeamY();
  const int tx = atkTargetX(), ty = atkTargetY();
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  // Hyper Beam is the one normal-type beam with a warm body; JET is its marker.
  const bool hot = (m.tex1 == ATK_JET);
  // C() takes 24-bit RGB, so this has to be a full 0xRRGGBB orange. A bare
  // 0xFD20 reads as dark green in the 5-6-5 conversion.
  const uint16_t glow = hot ? C(0xFF7A18) : tint;

  if (p < 0.42f) {
    const float q = clamp01(p / 0.30f);
    glowHalo(ox, oy, (int)(10 + 16 * q), glow, 0.45f * q);
    glowStar(ox, oy, (int)(10 + 18 * q), glow, 0.90f);
    glowRing(ox, oy, (int)(12 + 28 * q), (int)(12 + 28 * q), glow, 0.55f);
    for (int i = 0; i < 8; i++) {
      const float a = t * 5.0f + i * 0.7854f;
      const int rad = (int)((1.0f - q) * 34.0f + 7.0f);
      const int px = ox + (int)(cosf(a) * rad), py = oy + (int)(sinf(a) * rad);
      const uint16_t mot = lerp565(glow, 0xFFFF, 0.70f);
      fb.fillCircle(px, py, 2, mot);
      fb.drawFastHLine(px - 2, py, 5, mot);
      fb.drawFastVLine(px, py - 2, 5, mot);
    }
  }

  const float fire = clamp01((p - 0.16f) / 0.24f);
  if (fire > 0.02f && fire < 0.80f) muzzleFlash(ox, oy, glow, 1.0f - fire);
  if (fire > 0.0f) {
    // The lance now ends on Ditto's chest, so the shot visibly crosses the
    // gap and lands, instead of running off the edge of the panel.
    const int x1 = ox + (int)((tx + 7 - ox) * easeOutCubic(fire));
    const int yEnd = oy + (int)((ty - oy) * easeOutCubic(fire));
    const int len = x1 - ox;
    if (len > 12) {
      beamLance(ox + 6, oy, x1, yEnd, (hot ? 14.0f : 9.0f) * (0.90f + 0.20f * fxA),
                glow, t);
      // No full ellipses across the body: they read as the bands on a sausage.
      // Sparks travelling down the core sell the speed instead, and a few soft
      // rings along the path make the beam feel like it is lighting the air.
      for (int i = 0; i < 7; i++) {
        const int px = ox + 6 + (int)fmodf(t * 480.0f + i * (len / 7.0f), (float)len);
        const int py = oy + (int)((ty - oy) * (float)(px - ox) / (float)len)
                       + (int)(sinf(t * 24.0f + i * 2.1f) * 7.0f);
        const uint16_t mot = lerp565(glow, 0xFFFF, 0.78f);
        fb.fillCircle(px, py, 2, mot);
        fb.drawFastHLine(px - 2, py, 5, mot);
        fb.drawFastVLine(px, py - 2, 5, mot);
      }
      // No small rings strung along the lance: at nine pixels across they read
      // as orange beads with dark gaps, not as a continuous beam. The motes
      // above carry the motion; the target star carries the landing.
      // Three shaded pulses travel inside the beam. They are balls, not rings,
      // so they read as energy moving through a volume rather than beads on a
      // string.
      for (int i = 0; i < 3; i++) {
        const float q = fmodf(t * 0.85f + i * 0.333f, 1.0f);
        const int px = ox + 6 + (int)(len * q);
        const int py = oy + (int)((ty - oy) * q);
        shadeDisc(px, py, 3 + (i & 1), lerp565(glow, 0xFFFF, 0.52f), 0.95f);
      }
      if (fire > 0.82f) glowStar(tx, ty, 11, glow, 0.95f);
    }
  }

  if (p > 0.52f && p < 0.88f) atkImpact(m, tx, ty, (p - 0.52f) / 0.30f);
}

// --- BREATH: a moving stream of native-aspect puffs, not a stretched bar.
// Flamethrower is flame puffs, Hydro Pump and Bubble Beam are water puffs, Dragon
// Breath is energy puffs. The stream grows from the mouth to the target and then
// keeps flowing while the hit lands.
static void drawBreath(const MoveDef &m, float p, float t) {
  const int ox = atkOriginX(), oy = atkBeamY();
  const int tx = atkTargetX(), ty = atkTargetY();
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);

  if (p < 0.42f) {
    const float q = clamp01(p / 0.30f);
    glowHalo(ox, oy, (int)(10 + 15 * q), tint, 0.42f * q);
    glowStar(ox, oy, (int)(10 + 17 * q), tint, 0.88f);
    glowRing(ox, oy, (int)(12 + 26 * q), (int)(12 + 26 * q), tint, 0.55f);
    for (int i = 0; i < 8; i++) {
      const float a = t * 5.0f + i * 0.7854f;
      const int rad = (int)((1.0f - q) * 32.0f + 8.0f);
      const int px = ox + (int)(cosf(a) * rad), py = oy + (int)(sinf(a) * rad);
      const uint16_t mot = lerp565(tint, 0xFFFF, 0.72f);
      fb.fillCircle(px, py, 2, mot);
      fb.drawFastHLine(px - 2, py, 5, mot);
      fb.drawFastVLine(px, py - 2, 5, mot);
    }
  }

  const float fire = clamp01((p - 0.14f) / 0.24f);
  if (fire <= 0.0f) return;
  const int x1 = ox + (int)((tx + 5 - ox) * easeOutCubic(fire));
  const int yEnd = oy + (int)((ty - oy) * easeOutCubic(fire));
  const int len = x1 - ox;
  if (len > 8) {
    const bool water = (m.type == T_WATER);
    const bool jet = strcmp(m.name, "HYDROPUMP") == 0 ||
                     strcmp(m.name, "HYDROCANNON") == 0;
    // A soft, tapered haze is the body of the stream. It follows the line from
    // the mouth to the target chest, so a tall creature does not fire over the
    // opponent. Deliberately dim and covered by puffs.
    for (int i = 0; i <= len; i += 3) {
      const float q = (float)i / (float)len;
      const float taper = 0.30f + 0.70f * sinf(3.14159f * q);
      const int half = (int)((water ? 11.0f : 10.0f) * taper * (0.92f + 0.16f * fxA));
      const int yy = oy + (int)((ty - oy) * q);
      fb.fillRect(ox + i, yy - half - 3, 3, half * 2 + 6,
                  lerp565(tint, 0xFFFF, water ? 0.18f : 0.14f));
      fb.fillRect(ox + i, yy - half, 3, half * 2,
                  lerp565(tint, 0xFFFF, water ? 0.30f : 0.24f));
      fb.fillRect(ox + i, yy - (int)(half * 0.45f), 3, (int)(half * 0.9f),
                  lerp565(tint, 0xFFFF, water ? 0.52f : 0.58f));
    }
    // A continuous hot core under the puffs. Without it the flame row reads as
    // separate fireballs with dark gaps, not one long breath.
    if (!water) {
      for (int i = 0; i <= len; i += 3) {
        const float q = (float)i / (float)len;
        const int half = 2 + (int)(3.0f * sinf(3.14159f * q));
        const int yy = oy + (int)((ty - oy) * q);
        fb.fillRect(ox + i, yy - half, 3, half * 2, lerp565(tint, 0xFFFF, 0.45f));
        fb.fillRect(ox + i, yy - 1, 3, 2, lerp565(tint, 0xFFFF, 0.72f));
        fb.drawFastHLine(ox + i, yy + half, 3, lerp565(tint, 0x0000, 0.22f));
      }
    }
    // Hydro Pump is a jet, not a bubble stream: give it a solid lance under the
    // droplets so it reads as pressurised water instead of a chain of blobs.
    if (jet) beamLance(ox + 5, oy, x1, yEnd, 6.0f, tint, t);
    // More puffs than slots, moving slowly enough to overlap. The stream is
    // dense at the mouth (as in the games) and thins a little as it travels.
    int n = jet ? 12 : m.count * 2;
    if (n < 14) n = 14;
    if (n > 24) n = 24;
    for (int i = 0; i < n; i++) {
      const float q = fmodf(t * 1.25f + (float)i / (float)n, 1.0f);
      if (q > fire) continue;
      const int x = ox + (int)((x1 - ox) * q);
      const int yy = oy + (int)((ty - oy) * q);
      const int y = yy + (int)(sinf(q * 5.0f + t * 4.0f + i * 1.7f) * (2.0f + 5.0f * q));
      const float s = jet ? 0.16f + 0.16f * q
                          : (water ? 0.24f + 0.28f * q : 0.30f + 0.32f * q);
      const float ss = s * (0.94f + 0.12f * fxB);
      // A dimmer, larger copy behind each puff is the bloom. The old single
      // copy left hard-edged orange blobs; this keeps the interior readable
      // while the outside melts into the background. Every other puff also gets
      // a darker copy offset down-right: that one-pixel crescent is enough to
      // make the stream look rounded instead of pasted flat.
      const uint8_t tex = i & 1 ? m.tex2 : m.tex1;
      if ((i & 1) == 0)
        drawAtkCenter(tex, x + 2, y + 2, ss * 1.30f, lerp565(tint, 0x0000, 0.28f),
                      0.0f, 0.0f, 0.42f);
      drawAtkCenter(tex, x, y, ss * 1.38f, tint, 0.16f, 0.0f, 0.20f);
      drawAtkCenter(tex, x - 1, y - 1, ss, tint, 0.05f, 0.04f, 1.0f);
      const uint16_t lit = lerp565(tint, 0xFFFF, 0.86f);
      fb.fillCircle(x - 2 - (int)(3.0f * q), y - 2 - (int)(2.0f * q),
                    1 + (i & 1), lit);
      if (!water && (i % 4) == 0) {
        const int tx = x + 5 + (int)(4.0f * sinf(t * 7.0f + i));
        drawAtkCenter(ATK_FLAME2, tx, y + (int)(sinf(t * 9.0f + i) * 4.0f),
                      ss * 0.62f, tint, 0.18f, 0.20f, 0.92f);
      }
      if ((i & 3) == 0) {
        const uint16_t mot = lerp565(tint, 0xFFFF, 0.82f);
        fb.fillCircle(x, y, 2, mot);
        fb.drawFastHLine(x - 2, y, 5, mot);
      }
    }
    // A second, smaller row fills the gaps between the first row's puffs.
    for (int i = 0; !jet && i < n; i += 2) {
      const float q = fmodf(t * 1.25f + (float)i / (float)n + 0.5f / (float)n, 1.0f);
      if (q > fire) continue;
      const int x = ox + (int)((x1 - ox) * q);
      const int yy = oy + (int)((ty - oy) * q);
      const int y = yy + (int)(sinf(q * 5.0f + t * 4.0f + i) * (2.0f + 4.0f * q));
      drawAtkCenter(m.tex1, x, y, water ? 0.16f : 0.24f, tint, 0.04f, 0.02f, 0.95f);
    }
    glowHalo(x1, yEnd, 9, tint, 0.32f);
  }
  if (p > 0.55f && p < 0.86f) atkImpact(m, tx, ty, (p - 0.55f) / 0.28f);
}

// --- BOLT: two animated jagged arcs. This is the shape Thunderbolt has in the
// games; a straight bar with a spark texture on it never read as lightning.
static void drawBolt(const MoveDef &m, float p, float t) {
  const int ox = atkOriginX(), oy = atkBeamY();
  const int tx = atkTargetX(), ty = atkTargetY();
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);

  if (p < 0.42f) {
    const float q = clamp01(p / 0.30f);
    glowHalo(ox, oy, (int)(10 + 15 * q), tint, 0.45f * q);
    glowStar(ox, oy, (int)(10 + 17 * q), tint, 0.90f);
    glowRing(ox, oy, (int)(12 + 26 * q), (int)(12 + 26 * q), tint, 0.55f);
    for (int i = 0; i < 6; i++) {
      const float a = t * 6.0f + i * 1.0472f;
      const int rad = (int)((1.0f - q) * 30.0f + 8.0f);
      const int px = ox + (int)(cosf(a) * rad), py = oy + (int)(sinf(a) * rad);
      const uint16_t mot = lerp565(tint, 0xFFFF, 0.75f);
      fb.fillCircle(px, py, 2, mot);
      fb.drawFastHLine(px - 2, py, 5, mot);
      fb.drawFastVLine(px, py - 2, 5, mot);
    }
  }

  const float fire = clamp01((p - 0.15f) / 0.24f);
  if (fire <= 0.0f) return;
  const int x1 = ox + (int)((tx + 5 - ox) * easeOutCubic(fire));
  const int segs = 6;
  const uint16_t halo = lerp565(0x0000, tint, 0.42f);
  // One main path with alternating, large offsets. Two parallel arcs with small
  // offsets made a braid; a single path with sharp vertices and a softer halo is
  // what actually reads as lightning.
  int px = ox, py = oy;
  for (int k = 1; k <= segs; k++) {
    const float q = (float)k / (float)segs;
    const int x = ox + (int)((x1 - ox) * q);
    const uint32_t h = hash32((uint32_t)(k * 97u + (uint32_t)(t * 38.0f)));
    const float amp = 16.0f * sinf(3.14159f * q);
    const int dir = (k & 1) ? 1 : -1;
    const int baseY = oy + (int)((ty - oy) * q);
    const int y = (k == segs) ? ty : baseY + dir * (int)(amp * 0.55f + (h % 8u));
    // A deep-tinted stroke two pixels down-right gives the arc thickness. The
    // bright stroke stays on top, so the bolt has a shadow side rather than a
    // symmetrical outline.
    fb.drawLine(px + 2, py + 2, x + 2, y + 2, lerp565(tint, 0x0000, 0.30f));
    fb.drawLine(px, py - 4, x, y - 4, lerp565(tint, 0xFFFF, 0.22f));
    fb.drawLine(px, py + 4, x, y + 4, lerp565(tint, 0xFFFF, 0.22f));
    fb.drawLine(px, py - 3, x, y - 3, lerp565(tint, 0xFFFF, 0.40f));
    fb.drawLine(px, py + 3, x, y + 3, lerp565(tint, 0xFFFF, 0.40f));
    fb.drawLine(px, py - 2, x, y - 2, lerp565(tint, 0xFFFF, 0.58f));
    fb.drawLine(px, py + 2, x, y + 2, lerp565(tint, 0xFFFF, 0.58f));
    fb.drawLine(px, py - 1, x, y - 1, halo);
    fb.drawLine(px, py + 1, x, y + 1, halo);
    fb.drawLine(px, py - 1, x, y - 1, 0xFFFF);
    fb.drawLine(px, py, x, y, 0xFFFF);
    fb.drawLine(px, py + 1, x, y + 1, 0xFFFF);
    px = x;
    py = y;
  }
  // One long fork and three short ones: the branches are what make it look like
  // it is striking the target rather than being a decorative line.
  {
    const float fq = 0.42f;
    const int fx = ox + (int)((x1 - ox) * fq);
    const int fy = oy + (int)((ty - oy) * fq) + (int)(sinf(3.14159f * fq) * 10.0f);
    fb.drawLine(fx + 2, fy + 2, fx + 16, fy - 22, lerp565(tint, 0x0000, 0.28f));
    fb.drawLine(fx, fy, fx + 14, fy - 24, 0xFFFF);
    fb.drawLine(fx, fy + 1, fx + 14, fy - 23, halo);
  }
  for (int b = 0; b < 3; b++) {
    const float q = 0.28f + b * 0.22f;
    const int x = ox + (int)((x1 - ox) * q);
    const int y = oy + (int)((ty - oy) * q)
                  + (int)(sinf(3.14159f * q) * (b & 1 ? 12.0f : -12.0f));
    const uint32_t h = hash32((uint32_t)(b * 131u + (uint32_t)(t * 40.0f)));
    fb.drawLine(x, y, x + (int)(h % 19u) - 9, y + (int)((h >> 8) % 25u) - 12,
                lerp565(tint, 0xFFFF, 0.65f));
  }
  for (int i = 0; i < 3; i++) {
    const float q = 0.3f + i * 0.3f;
    const int x = ox + (int)((x1 - ox) * q);
    const int y = oy + (int)((ty - oy) * q)
                  + (int)(sinf(3.14159f * q) * (i & 1 ? 10.0f : -10.0f));
    const uint16_t mot = lerp565(tint, 0xFFFF, 0.56f);
    shadeDisc(x, y, 3, mot, 0.95f);
    const uint16_t spark = lerp565(mot, 0xFFFF, 0.80f);
    fb.drawFastHLine(x - 3, y, 7, spark);
    fb.drawFastVLine(x, y - 3, 7, spark);
  }
  if (p < 0.34f) muzzleFlash(ox, oy, tint, 1.0f - p / 0.34f);
  glowHalo(x1, ty, 10, tint, 0.38f);
  glowRing(ox, oy, 18, 18, tint, 0.28f);
  if (p > 0.55f && p < 0.86f) atkImpact(m, tx, ty, (p - 0.55f) / 0.28f);
}

// --- ORB: staggered projectiles on an arc, with trails and one shared hit
static void drawOrb(const MoveDef &m, float p, float t) {
  const int ox = atkOriginX(), oy = atkOriginY();
  const int tx = atkTargetX(), ty = atkTargetY();
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  const float wind = clamp01(p / 0.30f);
  if (p < 0.42f) {
    glowStar(ox, oy, (int)(9 + 16 * wind), tint, 0.90f);
    // A flattened ring is an orbit seen in perspective, so the charge now sits
    // in the scene rather than facing the camera as a flat circle.
    const int cr = (int)(14 + 28 * wind);
    fb.drawEllipse(ox, oy, cr, cr / 2, lerp565(tint, 0xFFFF, 0.30f));
    fb.drawEllipse(ox, oy, cr + 4, cr / 2 + 2, lerp565(tint, 0x0000, 0.28f));
    for (int i = 0; i < 6; i++) {
      const float a = t * 4.5f + i * 1.0472f;
      const int rad = (int)((1.0f - wind) * 30.0f + 16.0f);
      fb.fillCircle(ox + (int)(cosf(a) * rad), oy + (int)(sinf(a) * rad * 0.7f),
                    2, lerp565(tint, 0xFFFF, 0.70f));
    }
  }
  if (p < 0.28f) muzzleFlash(ox, oy, tint, 1.0f - p / 0.28f);

  int count = m.count;
  if (count > 5) count = 5;
  if (count < 1) count = 1;
  for (int i = 0; i < count; i++) {
    const float st = 0.06f + i * 0.13f;
    const float q = clamp01((p - st) / 0.50f);
    if (q <= 0.0f || q >= 1.0f) continue;
    const float arc = 16.0f + fxA * 12.0f +
                      (float)(hash32((uint32_t)i * 71u + 7u) % 18u);
    const int x = (int)(ox + (tx - ox) * q);
    const int y = (int)(oy + (ty - oy) * q - sinf(3.14159f * q) * arc);
    for (int k = 1; k <= 2; k++) {
      const float qq = q - k * 0.06f;
      if (qq <= 0.0f) continue;
      const int tr = 4 - k;
      fb.fillCircle((int)(ox + (tx - ox) * qq),
                    (int)(oy + (ty - oy) * qq - sinf(3.14159f * qq) * arc),
                    tr, lerp565(tint, 0xFFFF, 0.42f - 0.1f * k));
    }
    const float ss = 0.86f + 0.28f * sinf(3.14159f * q);
    castShadow(x, y, 7 + (int)(4 * ss));
    const bool ring = (m.tex1 == ATK_PSYRING || m.tex1 == ATK_PSYRIPPLE ||
                       m.tex1 == ATK_SHADOW || m.tex1 == ATK_SHADOWORB ||
                       m.tex1 == ATK_ORB);
    // The generic orb is a fire-coloured ball; a Ghost move has to override its
    // hue rather than merely mix toward the grey-purple type colour.
    const uint16_t orbTint = m.type == T_GHOST ? C(0x9A5CFF)
                             : (m.type == T_DARK ? C(0x6A4FA0) : tint);
    // Do not enlarge the texture and dim it: that puts its baked outline around
    // the projectile as a black ring on bright scenes. The colored halo below is
    // the glow instead.
    const bool waterOrb = (m.tex1 == ATK_WATER);
    const float orbMix = (m.type == T_GHOST || m.type == T_DARK) ? 0.88f
                                                                  : (ring ? 0.42f : 0.08f);
    if (waterOrb || ring) {
      // Procedural spheres for water and for the ring-style projectiles. The
      // archived sheets are outline-heavy; at orb scale their dark border read
      // as a black square. Layer a rim, a body and a moving highlight instead.
      const int R = (int)((waterOrb ? 9.0f : 10.0f) * ss);
      shadeDisc(x, y, R, orbTint, 0.95f);
      if (waterOrb) {
        fb.fillCircle(x + R - 1, y - R / 2, 2, lerp565(tint, 0xFFFF, 0.72f));
        fb.fillCircle(x - R + 2, y + R / 3, 1, lerp565(tint, 0xFFFF, 0.55f));
      } else {
        glowRing(x, y, R + 5, R + 3, orbTint, 0.62f);
        // A small crescent of rim light on the shadow side reads as a round
        // object catching a second light, not a flat sprite circle.
        fb.drawCircle(x + 1, y + 1, R - 1, lerp565(orbTint, 0xFFFF, 0.34f));
      }
    } else {
      drawAtkCenter(m.tex1, x, y, ss, orbTint, orbMix, 0.12f, 1.0f);
      // A drawn hot centre rather than the generic orb sheet: that sheet is a
      // square with a dark border, which is exactly what a round projectile
      // must not look like.
      const int R = (int)(7.0f * ss);
      shadeDisc(x, y, R, orbTint, 0.98f);
    }
    glowHalo(x, y, (int)(11 + 6 * ss), tint, 0.34f);
    const int sx = x - (int)(cosf(t * 12.0f + i) * 7.0f);
    const int sy = y - (int)(sinf(t * 10.0f + i) * 6.0f);
    const uint16_t spark = lerp565(tint, 0xFFFF, 0.85f);
    fb.fillCircle(sx, sy, 2, spark);
    fb.drawFastHLine(sx - 3, sy, 7, spark);
    fb.drawFastVLine(sx, sy - 3, 7, spark);
  }
  if (p > 0.55f && p < 0.88f) atkImpact(m, tx, ty, (p - 0.55f) / 0.28f);
}

// --- SLASH: claws at the target, a quick lunge (in the motion code) and a hit
// One tapered claw as a chain of short segments: a deep shadow offset down,
// a coloured body, and a bright upper edge. No texture means no baked outline.
static void drawSlashStroke(int x0, int y0, int x1, int y1, float bend,
                            uint16_t tint, float bright) {
  const uint16_t deep = lerp565(tint, 0x0000, 0.36f);
  const uint16_t body = lerp565(tint, 0xFFFF, 0.24f);
  const uint16_t hot = lerp565(tint, 0xFFFF, 0.50f + 0.42f * bright);
  int px = x0, py = y0;
  for (int s = 1; s <= 7; s++) {
    const float q = s / 7.0f;
    const float arc = sinf(3.14159f * q) * bend;
    const int x = x0 + (int)((x1 - x0) * q + arc);
    const int y = y0 + (int)((y1 - y0) * q - arc * 0.55f);
    fb.drawLine(px + 2, py + 2, x + 2, y + 2, deep);
    fb.drawLine(px, py, x, y, body);
    fb.drawLine(px, py + 1, x, y + 1, body);
    fb.drawLine(px, py - 1, x, y - 1, hot);
    px = x;
    py = y;
  }
  shadeDisc(px, py, 2, hot, 0.95f);
}

static void drawSlash(const MoveDef &m, float p, float t) {
  const int tx = atkTargetX(), ty = atkTargetY();
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  const int ox = atkOriginX(), oy = atkOriginY();
  if (p < 0.32f) {
    const float q = clamp01(p / 0.32f);
    glowHalo(ox, oy, (int)(9 + 9 * q), tint, 0.34f * q);
    glowStar(ox, oy, (int)(9 + 13 * q), tint, 0.82f);
    glowRing(ox, oy, (int)(12 + 14 * q), (int)(12 + 14 * q), tint, 0.36f);
  }
  // Vine Whip is the one slash that should read as a reach, not three claws in
  // mid-air: a drawn, curved vine runs from the mouth to the hit. The old
  // texture chain carried a dark baked outline, so it is now line work too.
  if (m.tex1 == ATK_SWIRL) {
    const int ox = atkOriginX(), oy = atkOriginY();
    const float grow = clamp01((p - 0.04f) / 0.40f);
    int px = ox, py = oy;
    const uint16_t deep = lerp565(tint, 0x0000, 0.34f);
    const uint16_t body = lerp565(tint, 0xFFFF, 0.20f);
    const uint16_t hot = lerp565(tint, 0xFFFF, 0.62f);
    for (int k = 1; k <= 8; k++) {
      const float f = (float)k / 8.0f * grow;
      const int x = (int)(ox + (tx - ox) * f + sinf(t * 4.0f + f * 6.0f) * 9.0f * f);
      const int y = (int)(oy + (ty - oy) * f + cosf(t * 3.5f + f * 5.0f) * 7.0f * f);
      fb.drawLine(px + 2, py + 2, x + 2, y + 2, deep);
      fb.drawLine(px, py, x, y, body);
      fb.drawLine(px, py + 1, x, y + 1, body);
      fb.drawLine(px, py - 1, x, y - 1, hot);
      px = x;
      py = y;
    }
    if (grow > 0.92f) shadeDisc(px, py, 3, hot, 0.95f);
  }
  int count = m.count;
  if (count < 2) count = 2;
  if (count > 3) count = 3;
  for (int i = 0; i < count; i++) {
    const float q = clamp01((p - 0.05f - i * 0.10f) / 0.55f);
    if (q <= 0.0f || q >= 1.0f) continue;
    const uint32_t h = hash32((uint32_t)i * 31u + 3u);
    const int x = tx - 18 + (int)(h % 32u) + (int)((fxA - 0.5f) * 8.0f);
    const int y = ty - 24 + (int)((h >> 7) % 48u) + (int)((fxB - 0.5f) * 8.0f);
    const float s = 0.68f + 0.45f * sinf(3.14159f * q);
    const int len = (int)(18 + 18 * s);
    const int slope = (i & 1) ? -1 : 1;
    // The EBDX slash/claw sheets are the move's actual silhouette. Draw them
    // rotated instead of replacing them with generic strokes. Textures without
    // a slash shape still use the shaded stroke.
    const bool realSlash = (m.tex1 == ATK_SLASH || m.tex1 == ATK_CLAW ||
                            m.tex1 == ATK_DARKCLAW || m.tex1 == ATK_SWORD);
    if (realSlash) {
      drawAtkRot(m.tex1, x, y, (int)(len * 1.25f), (int)(13 + 8 * s),
                 slope * 0.52f, tint, 0.12f, 0.10f, 0.92f + 0.08f * s);
    } else {
      drawSlashStroke(x - len / 2, y + slope * 7,
                      x + len / 2, y - slope * 9,
                      (i & 1) ? 5.0f : -5.0f, tint, s);
    }
    if (i == 0) glowRing(x, y, (int)(9 + 7 * sinf(3.14159f * q)),
                         (int)(9 + 7 * sinf(3.14159f * q)), tint, 0.22f);
  }
  if (p < 0.30f) {
    // Start at the silhouette's edge, not the centre: a speed line through the
    // creature reads as a laser skewering it.
    const int sx = gX + gW - 3;
    if (tx > sx)
      for (int i = 0; i < 4; i++)
        fb.drawFastHLine(sx, ty + i * 4 - 6, tx - sx,
                         lerp565(tint, 0xFFFF, 0.62f - i * 0.1f));
  }
  if (p > 0.55f && p < 0.90f) atkImpact(m, tx, ty, (p - 0.55f) / 0.30f);
}

// --- RAIN: debris falls across the target half of the field
static void drawRain(const MoveDef &m, float p, float t) {
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  int count = m.count;
  if (count < 3) count = 3;
  if (count > 8) count = 8;
  const float mix = (m.tex1 == ATK_ROCK) ? 0.0f : 0.12f;
  if (p < 0.58f) {
    const float q = clamp01(p / 0.40f);
    if (m.tex1 == ATK_ROCK) {
      // A rock slide is dust from the opponent's platform, not a cloud in the
      // sky.
      drawAtkCenter(ATK_SMOKE, 240, TARGET_GROUND_Y - 8, 1.0f + 0.45f * q, tint,
                    0.45f, 0.0f, 0.30f + 0.15f * q);
      glowRing(240, TARGET_GROUND_Y - 4, (int)(26 + 30 * q), (int)(8 + 12 * q), tint, 0.30f);
    } else if (m.tex1 == ATK_CRYSTAL) {
      drawAtkCenter(ATK_SMOKE, 248, 24, 1.15f + 0.55f * q, tint, 0.45f, 0.0f,
                    0.28f + 0.16f * q);
      glowRing(248, 24, (int)(22 + 34 * q), (int)(10 + 18 * q), tint, 0.30f);
    } else {
      glowRing(248, 24, (int)(20 + 30 * q), (int)(9 + 16 * q), tint, 0.28f);
    }
  }
  glowRing(250, TARGET_GROUND_Y - 2, 56, 9, tint, 0.24f);
  for (int wave = 0; wave < 2; wave++) {
    for (int i = 0; i < count; i++) {
      const float q = clamp01((p - wave * 0.28f - i * 0.045f) / 0.55f);
      if (q <= 0.0f || q >= 1.0f) continue;
      const uint32_t h = hash32((uint32_t)(i + wave * 11) * 53u + 11u);
      const int x = 165 + (int)(h % 145u);
      const int y = -24 + (int)(q * (TARGET_GROUND_Y + 34));
      const float s = 0.60f + 0.25f * sinf(3.14159f * q);
      drawAtkCenter(m.tex1, x, y - 16, s * 0.70f, tint, mix, 0.0f, 0.35f);
      drawAtkCenter(m.tex1, x + 2, y + 2, s * 1.08f,
                    lerp565(tint, 0x0000, 0.26f), mix, 0.0f, 0.46f);
      drawAtkCenter(m.tex1, x - 1, y - 1, s, tint, mix, 0.08f, 1.0f);
      if (y > TARGET_GROUND_Y - 30) {
        glowRing(x, TARGET_GROUND_Y, 14, 4, tint, 0.45f);
        drawAtkCenter(ATK_SMOKE, x, TARGET_GROUND_Y - 5, 0.55f, tint, 0.30f, 0.0f, 0.55f);
      }
    }
  }
  if (p < 0.65f) {
    for (int i = 0; i < 4; i++)
      fb.drawFastHLine(190 + i * 28, TARGET_GROUND_Y - 2, 16, lerp565(scene.ground, tint, 0.35f));
  }
}

// --- QUAKE: rings behind, boulders and dust in front
static void drawQuakeBack(const MoveDef &m, float p, float t) {
  const int cx = gX + gW / 2;
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  glowFloor(tint, 1.0f);
  for (int r = 0; r < 4; r++) {
    const float q = clamp01((p - 0.05f - r * 0.11f) / 0.55f);
    if (q <= 0.0f) continue;
    fb.drawEllipse(cx, GROUND_Y, (int)(20 + 104 * q), (int)(5 + 28 * q),
                   lerp565(tint, 0xFFFF, 0.22f * q));
  }
  for (int i = 0; i < 2; i++) {
    drawAtkCenter(ATK_SMOKE, cx - 54 + i * 108, GROUND_Y - 8, 0.9f,
                  tint, 0.40f, 0.0f, 0.40f);
  }
  // Fissures, not smooth circles: filled circles on sand read as potatoes.
  for (int i = 0; i < 5; i++) {
    const int x0 = cx - 84 + i * 42 + (int)(sinf(t * 3.0f + i) * 3.0f);
    const int y0 = GROUND_Y + 1;
    const int x1 = x0 + 12, y1 = y0 + 7;
    const int x2 = x1 - 5, y2 = y1 + 7;
    const uint16_t crack = lerp565(scene.ground, 0x000000, 0.62f);
    const uint16_t lip = lerp565(tint, 0xFFFF, 0.58f);
    fb.drawLine(x0, y0, x1, y1, crack);
    fb.drawLine(x1, y1, x2, y2, crack);
    fb.drawLine(x0, y0 + 1, x1, y1 + 1, lip);
    fb.drawLine(x1, y1 + 1, x2, y2 + 1, lip);
  }
  for (int i = 0; i < 4; i++)
    fb.fillCircle(cx - 66 + i * 44, GROUND_Y - 2, 2, lerp565(scene.ground, tint, 0.35f));
  for (int i = 0; i < 5; i++) {
    const float a = 0.6f + i * 0.45f;
    fb.drawLine(cx, GROUND_Y + 2, cx + (int)(cosf(a) * 46), GROUND_Y + (int)(sinf(a) * 8),
                lerp565(scene.ground, 0x000000, 0.55f));
  }
}

static void drawQuakeFront(const MoveDef &m, float p, float t) {
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  const int cx = gX + gW / 2;
  for (int i = 0; i < 8; i++) {
    const float q = clamp01((p - 0.06f - i * 0.045f) / 0.52f);
    if (q <= 0.0f || q >= 1.0f) continue;
    const uint32_t h = hash32((uint32_t)i * 83u + 5u);
    const int x = 54 + (int)(h % 232u);
    const int hh = 10 + (int)((h >> 12) % 14) + (int)(q * 18.0f);
    const int lift = (int)(sinf(3.14159f * q) * 7.0f);
    const int baseY = GROUND_Y - lift;
    const int tipX = x + (int)((h >> 15) % 7u) - 3;
    const int tipY = baseY - hh;
    const int midX = x + (int)(h % 5u) - 2;
    const uint16_t rock = lerp565(scene.ground, 0x000000, 0.46f);
    const uint16_t rockLo = lerp565(rock, 0x000000, 0.34f);
    const uint16_t edge = lerp565(tint, 0xFFFF, 0.62f);
    // Jagged shards, not filled circles: the rounded rock texture looked like
    // potatoes scattered on the sand. Each shard now has a lit face and a
    // shadowed face split along a ridge, plus a ground shadow. That is enough
    // to read as a chipped 3D stone instead of a brown paper triangle.
    fb.fillEllipse(x + 3, GROUND_Y + 1, 9, 3,
                   lerp565(scene.ground, 0x000000, 0.58f));
    fb.fillTriangle(midX, baseY, tipX, tipY, x + 7, baseY, rockLo);
    fb.fillTriangle(x - 6, baseY, tipX, tipY, midX, baseY, rock);
    fb.drawLine(x - 6, baseY, tipX, tipY, edge);
    fb.drawLine(tipX, tipY, midX, baseY, lerp565(rock, 0xFFFF, 0.26f));
    fb.drawLine(tipX, tipY, x + 7, baseY, lerp565(tint, 0x0000, 0.42f));
    fb.fillCircle(tipX, tipY, 1, lerp565(tint, 0xFFFF, 0.85f));
    fb.fillCircle(x, GROUND_Y - 1, 2, lerp565(tint, 0xFFFF, 0.48f));
  }
  if (p > 0.18f && p < 0.62f) {
    const float q = sinf(clamp01((p - 0.18f) / 0.44f) * 3.14159f);
    glowStar(cx, GROUND_Y - 4, (int)(18 + 12 * q), tint, 0.80f);
    muzzleFlash(cx, GROUND_Y - 3, tint, q * 0.8f);
  }
  if (m.tex2 == ATK_SUN && p > 0.35f) {
    const float q = clamp01((p - 0.35f) / 0.35f);
    glowStar(cx, GROUND_Y - 4, (int)(18 + 12 * q), tint, 0.78f);
    for (int i = 0; i < 7; i++) {
      const int x = cx - 70 + i * 23;
      fb.drawLine(x, GROUND_Y, x + (int)(sinf(t * 5 + i) * 6), 20,
                  lerp565(tint, scene.skyTop, 0.45f));
    }
  }
}

// --- AURA: rings and an orbiting halo around the creature, no target
static void drawAuraBack(const MoveDef &m, float p, float t) {
  const int cx = gX + gW / 2;
  const int cy = gY + gH / 2;
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  const float q = clamp01(p / 0.65f);
  glowFloor(tint, 0.9f);
  for (int i = 0; i < 9; i++) {
    const int x = cx - 80 + i * 20 + (int)(sinf(t * 3.0f + i) * 3.0f);
    fb.drawLine(x, gY + 10, x, GROUND_Y - 2, lerp565(scene.skyTop, tint, 0.28f + 0.12f * q));
  }
  // Three bright rings, not a stack of dim ones. Ten near-black outlines around
  // a small creature read as a cage; four tinted rings read as a halo.
  for (int r = 0; r < 2; r++) {
    glowRing(cx, cy, (int)(22 + 18 * r + 16 * q),
             (int)(16 + 12 * r + 12 * q), tint,
             0.42f - 0.08f * r);
  }
  for (int r = 0; r < 3; r++) {
    const float rp = clamp01(p * 1.1f - r * 0.18f);
    if (rp <= 0.0f) continue;
    glowRing(cx, GROUND_Y, (int)(20 + 70 * rp), (int)(5 + 20 * rp), tint, 0.38f * (1.0f - rp * 0.5f));
  }
  // The far half of the orbit passes behind the creature. Splitting the
  // orbiters across the back/front passes is what makes the aura an orbit in
  // depth rather than a ring of stickers on the glass.
  for (int i = 0; i < 8; i++) {
    const float a = t * 2.4f + i * 6.2832f / 8.0f;
    if (sinf(a) >= 0.0f) continue;
    const int rr = gW / 2 + 12 + (int)(sinf(t * 3.0f + i) * 4.0f);
    const int px = cx + (int)(cosf(a) * rr);
    const int py = cy + (int)(sinf(a) * rr * 0.60f);
    shadeDisc(px, py, 2, lerp565(tint, 0x0000, 0.30f), 0.55f);
  }
}

static void drawAuraFront(const MoveDef &m, float p, float t) {
  const int cx = gX + gW / 2;
  const int cy = gY + gH / 2;
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  const int n = 8;
  for (int i = 0; i < n; i++) {
    const float a = t * 2.4f + i * 6.2832f / (float)n;
    const float depth = sinf(a);
    if (depth < 0.0f) continue;          // the back half was drawn behind the creature
    const float ds = 0.72f + 0.28f * depth;
    const int rr = gW / 2 + 12 + (int)(sinf(t * 3.0f + i) * 4.0f);
    const int px = cx + (int)(cosf(a) * rr);
    const int py = cy + (int)(sinf(a) * rr * 0.60f);
    // Front orbiters are larger, brighter and rounder; side ones are smaller
    // and deeper-tinted. Procedural sparks, not scaled sheets: even small copies
    // of the sparkle texture chained into a black outline around a creature.
    const uint16_t spark = lerp565(tint, 0xFFFF, 0.55f + 0.25f * depth);
    shadeDisc(px, py, 2 + (depth > 0.72f), spark, 0.95f);
    const int arm = 2 + (int)(2.0f * ds);
    fb.drawFastHLine(px - arm, py, arm * 2 + 1, lerp565(spark, 0xFFFF, 0.55f));
    fb.drawFastVLine(px, py - arm, arm * 2 + 1, lerp565(spark, 0xFFFF, 0.55f));
  }
  // Sing, Spore and Dark Void are status moves, but they should still visibly
  // leave the creature and reach Ditto rather than pulsing in place.
  if (strcmp(m.name, "SING") == 0 || strcmp(m.name, "SPORE") == 0 ||
      strcmp(m.name, "DARKVOID") == 0) {
    const float q = clamp01((p - 0.35f) / 0.52f);
    if (q > 0.0f && q < 1.0f) {
      const int px = cx + (int)((TARGET_X - cx) * q);
      const int py = cy + (int)((TARGET_Y - cy) * q);
      // A drawn pulse, not ATK_SUN: that sheet is large and carries a dark
      // outline, so scaling it here made a black cage around the creature.
      castShadow(px, py, 5);
      shadeDisc(px, py, 6, lerp565(tint, 0xFFFF, 0.38f), 0.98f);
      glowHalo(px, py, 10, tint, 0.30f);
      for (int k = 0; k < 3; k++)
        fb.fillCircle(px + (int)(sinf(t * 4 + k * 2.1f) * 10.0f),
                      py + (int)(cosf(t * 3 + k * 1.7f) * 8.0f),
                      2, lerp565(tint, 0xFFFF, 0.70f));
      if (q > 0.88f) atkImpact(m, TARGET_X, TARGET_Y, (q - 0.88f) / 0.12f);
    }
  }
  if (p > 0.7f) {
    const float q = (p - 0.7f) / 0.3f;
    fb.drawEllipse(cx, GROUND_Y - 2, (int)(30 + 40 * q), (int)(7 + 8 * q),
                   lerp565(tint, 0xFFFF, 0.5f * (1.0f - q)));
  }
}

// --- WAVE: expanding rings travelling at the target, plus native-aspect streaks.
// The old version stretched one texture across the whole panel, which turned a
// gust into horizontal stripes. Rings read as a pressure wave for wind, water and
// sound alike, and the streaks give it a direction.
static void drawWave(const MoveDef &m, float p, float t) {
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  const int cx = gX + gW / 2;
  const int oy = atkOriginY();
  const int tx = atkTargetX(), ty = atkTargetY();
  glowFloor(tint, 0.8f);
  if (p < 0.35f) {
    const float q = clamp01(p / 0.35f);
    glowHalo(cx, oy, (int)(11 + 13 * q), tint, 0.34f * q);
    glowStar(cx, oy, (int)(10 + 15 * q), tint, 0.88f);
    glowRing(cx, oy, (int)(18 + 26 * q), (int)(12 + 18 * q), tint, 0.35f);
  }
  for (int i = 0; i < 4; i++) {
    const float q = clamp01((p - 0.05f - i * 0.07f) / 0.58f);
    if (q <= 0.0f || q >= 1.0f) continue;
    const int x = cx + (int)((tx - cx) * 0.85f * q);
    const int y = oy + (int)((ty - oy) * 0.85f * q);
    const int rx = (int)(20 + 84 * q);
    const int ry = (int)(7 + 26 * q);
    // A deeper copy offset down-right gives the wave a back wall; the front
    // rings stay bright. That asymmetry is what turns a flat ellipse into a
    // pressure front with thickness.
    fb.drawEllipse(x + 2, y + 3, rx, ry, lerp565(tint, 0x0000, 0.30f * (1.0f - q)));
    fb.drawEllipse(x, y, rx, ry, lerp565(0x0000, tint, 0.46f * (1.0f - q)));
    fb.drawEllipse(x, y, rx - 3, ry - 2, lerp565(tint, 0xFFFF, 0.34f * (1.0f - q)));
    fb.drawEllipse(x, y, rx + 3, ry + 2, lerp565(0x0000, tint, 0.16f * (1.0f - q)));
    if ((i & 1) == 0)
      fb.drawEllipse(x, GROUND_Y + 1, rx / 2, 3 + q * 4,
                     lerp565(floorTint, 0x000000, 0.33f));
  }
  for (int i = 0; i < 20; i++) {
    const float q = clamp01((p - 0.10f) / 0.62f);
    if (q <= 0.0f) continue;
    const int x = 34 + (int)((W - 74) * q) + (int)(sinf(t * 4.0f + i * 1.9f) * 7.0f);
    const int yy = oy + (int)((ty - oy) * q);
    const int y = yy + (int)(sinf(i * 2.4f + t * 3.4f) * 38.0f);
    drawAtkCenter(ATK_STREAK, x, y, 0.54f, tint, 0.08f, 0.16f, 0.96f);
    if ((i & 1) == 0) {
      const uint16_t mot = lerp565(tint, 0xFFFF, 0.80f);
      fb.fillCircle(x, y + 8, 2, mot);
      fb.drawFastHLine(x - 2, y + 8, 5, mot);
    }
  }
  if (m.type == T_FLYING || m.type == T_NORMAL) {
    for (int i = 0; i < 6; i++) {
      const float q = clamp01((p - 0.12f - i * 0.05f) / 0.58f);
      if (q <= 0.0f || q >= 1.0f) continue;
      const int x = gX + gW + (int)((tx - gX - gW) * q);
      const int yy = oy + (int)((ty - oy) * q);
      const int y = yy + (int)(sinf(t * 5.0f + i * 1.3f) * (12.0f + 10.0f * q));
      drawAtkCenter(ATK_SWIRL, x, y, 0.48f, tint, 0.82f, 0.10f, 0.92f);
    }
  }
  for (int i = 0; i < 8; i++) {
    const uint32_t h = hash32((uint32_t)i * 41u + 9u);
    const float q = clamp01((p - 0.12f) / 0.7f);
    const int x = (int)(h % W);
    const int y = GROUND_Y - (int)(q * (30 + (h >> 9) % 60));
    fb.fillCircle(x, y + (int)(sinf(t * 6 + i) * 3), 1 + (i & 1),
                  lerp565(tint, 0xFFFF, 0.45f));
  }
}

// --- STORM: projectiles spiral out of the creature and converge on the target
static void drawStorm(const MoveDef &m, float p, float t) {
  const int cx = gX + gW / 2, cy = gY + gH / 2;
  const int tx = atkTargetX(), ty = atkTargetY();
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  glowFloor(tint, 0.9f);
  glowRing(cx, cy, (int)(gW / 2 + 22), (int)(gH / 2 + 14), tint, 0.28f);
  if (p < 0.42f) {
    const float q = clamp01(p / 0.35f);
    glowHalo(cx, cy, (int)(18 + 16 * q), tint, 0.30f * q);
    glowStar(cx, cy, (int)(14 + 12 * q), tint, 0.70f);
  }
  int count = m.count;
  if (count < 5) count = 5;
  if (count > 9) count = 9;
  for (int i = 0; i < count; i++) {
    const float q = clamp01((p - i * 0.055f) / 0.65f);
    if (q <= 0.0f || q >= 1.0f) continue;
    const float a = t * 3.6f + i * 6.2832f / (float)count;
    const float depth = sinf(a);
    const float ds = 0.72f + 0.28f * (depth + 1.0f) * 0.5f;
    const uint16_t bodyTint = depth < 0.0f ? lerp565(tint, 0x0000, 0.20f) : tint;
    const float orbit = 1.0f - q;
    const int x = cx + (int)(cosf(a) * (24.0f + 12.0f * sinf(t * 2.1f + i)) * orbit
                             + (tx - cx) * q);
    const int y = cy + (int)(sinf(a) * 18.0f * orbit + (ty - cy) * q
                             - sinf(3.14159f * q) * 10.0f);
    const float tq = clamp01(q - 0.10f);
    const int px = cx + (int)(cosf(a) * (24.0f + 12.0f * sinf(t * 2.1f + i)) * (1.0f - tq)
                             + (tx - cx) * tq);
    const int py = cy + (int)(sinf(a) * 18.0f * (1.0f - tq) + (ty - cy) * tq
                             - sinf(3.14159f * tq) * 10.0f);
    drawAtkCenter(i & 1 ? m.tex2 : m.tex1, px, py, 0.48f * ds, bodyTint, 0.10f, 0.0f, 0.32f);
    drawAtkCenter(i & 1 ? m.tex2 : m.tex1, x, y, 0.55f * ds, bodyTint, 0.12f, 0.12f, 1.0f);
    if ((i & 1) == 0) castShadow(x, y, 4 + (int)(2 * ds));
    glowHalo(x, y, 8 + (int)(2 * ds), bodyTint, 0.20f + 0.10f * depth);
    const uint16_t spark = lerp565(bodyTint, 0xFFFF, 0.62f);
    shadeDisc(x, y, 2 + (depth > 0.45f), spark, 0.95f);
    fb.drawFastHLine(x - 3, y, 7, lerp565(spark, 0xFFFF, 0.55f));
    fb.drawFastVLine(x, y - 3, 7, lerp565(spark, 0xFFFF, 0.55f));
  }
  if (p > 0.62f && p < 0.92f) atkImpact(m, tx, ty, (p - 0.62f) / 0.28f);
}

// ---------------------------------------------------- real move silhouettes ---
// These are the moves whose in-game animation is not a generic beam/orb shape.
// They use the actual EBDX sheets the move scripts call: fists for Close Combat,
// lightning columns for Thunderbolt, the green lash for Vine Whip and the purple
// ball for Shadow Ball.

static bool isFistMove(const MoveDef &m) {
  return strcmp(m.name, "CLOSECOMBAT") == 0 || strcmp(m.name, "SUPERPOWER") == 0 ||
         strcmp(m.name, "CROSSCHOP") == 0 || strcmp(m.name, "KARATECHOP") == 0 ||
         strcmp(m.name, "LOWSWEEP") == 0 || strcmp(m.name, "VITALTHROW") == 0 ||
         strcmp(m.name, "DYNAMICPUNCH") == 0 || strcmp(m.name, "METEORMASH") == 0 ||
         strcmp(m.name, "FIREPUNCH") == 0 || strcmp(m.name, "ICEPUNCH") == 0 ||
         strcmp(m.name, "THUNDERPUNCH") == 0 || strcmp(m.name, "BULLETPUNCH") == 0;
}

static bool isSkyBoltMove(const MoveDef &m) {
  return strcmp(m.name, "THUNDERBOLT") == 0 || strcmp(m.name, "THUNDER") == 0 ||
         strcmp(m.name, "THUNDERSHOCK") == 0 || strcmp(m.name, "BOLTSTRIKE") == 0 ||
         strcmp(m.name, "FUSIONBOLT") == 0 || strcmp(m.name, "WILDCHARGE") == 0;
}

// Close Combat in Gen 5 is a burst of punches around the defender, not three
// claws. The fist and the orange hit fleck are separate sheets from the script.
static void drawFistMove(const MoveDef &m, float p, float t) {
  const int tx = atkTargetX(), ty = atkTargetY();
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  if (p < 0.30f) {
    const float q = clamp01(p / 0.30f);
    glowHalo(atkOriginX(), atkOriginY(), (int)(9 + 10 * q), tint, 0.34f * q);
    glowStar(atkOriginX(), atkOriginY(), (int)(8 + 12 * q), tint, 0.72f);
  }
  const int n = 8;
  for (int i = 0; i < n; i++) {
    const float q = clamp01((p - 0.10f - i * 0.065f) / 0.52f);
    if (q <= 0.0f || q >= 1.0f) continue;
    const uint32_t h = hash32((uint32_t)i * 67u + 13u);
    const int x = tx - 26 + (int)(h % 53u) + (int)((fxA - 0.5f) * 8.0f);
    const int y = ty - 24 + (int)((h >> 9) % 49u) + (int)((fxB - 0.5f) * 8.0f);
    const float s = 0.48f + 0.22f * sinf(3.14159f * q);
    // Actual fist art, kept upright: it is already drawn at the angle a punch
    // reads at this size. The second, smaller copy is the orange contact fleck.
    drawAtkCenter(ATK_FIST, x, y, s, 0xFFFF, 0.0f, 0.16f * targetHit, 1.0f);
    drawAtkCenter(ATK_FISTSPARK, x + 9, y - 7, 0.70f, 0xFFFF, 0.0f, 0.0f, 0.95f);
    drawAtkCenter(ATK_FISTSPARK, x - 8, y + 6, 0.48f, tint, 0.10f, 0.0f, 0.85f);
    castShadow(x, y, 6);
  }
  if (p > 0.52f && p < 0.92f) atkImpact(m, tx, ty, (p - 0.52f) / 0.34f);
}

// Thunderbolt/Thunder strike down from above. The old renderer drew a zig-zag
// beam out of the attacker's mouth, which is a different move.
static void drawSkyBoltMove(const MoveDef &m, float p, float t) {
  const int tx = atkTargetX(), ty = atkTargetY();
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  if (p < 0.28f) {
    const float q = clamp01(p / 0.28f);
    glowHalo(atkOriginX(), atkOriginY(), (int)(10 + 12 * q), tint, 0.40f * q);
    glowStar(atkOriginX(), atkOriginY(), (int)(9 + 14 * q), tint, 0.80f);
  }
  const int n = 4;
  for (int i = 0; i < n; i++) {
    const float q = clamp01((p - 0.18f - i * 0.07f) / 0.48f);
    if (q <= 0.0f || q >= 1.0f) continue;
    const uint32_t h = hash32((uint32_t)i * 47u + 5u);
    const int x = tx - 22 + (int)(h % 43u);
    const int len = (int)((ty + 26) * easeOutCubic(q));
    if (len < 3) continue;
    drawAtk(ATK_LIGHTNING, x, -10, 12, len, tint, 0.12f, 0.10f, 1.0f);
    drawAtk(ATK_LIGHTNING, x + 5, -10, 8, (len * 4) / 5,
            lerp565(tint, 0xFFFF, 0.65f), 0.0f, 0.0f, 1.0f);
    if (i == 0 || i == 3) {
      drawAtkCenter(ATK_SPARK, x + 2, ty - 18 + (int)(h % 35u), 0.45f,
                    tint, 0.12f, 0.0f, 0.90f);
    }
  }
  if (p > 0.52f && p < 0.92f) atkImpact(m, tx, ty, (p - 0.52f) / 0.36f);
}

// Vine Whip is the green crescent lash from the script, repeated along the arc
// from the user's mouth to the defender. No straight speed-line.
static void drawWhipMove(const MoveDef &m, float p, float t) {
  const int ox = atkOriginX(), oy = atkOriginY();
  const int tx = atkTargetX(), ty = atkTargetY();
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  const float grow = clamp01((p - 0.04f) / 0.44f);
  if (p < 0.20f) {
    glowHalo(ox, oy, (int)(8 + 10 * grow), tint, 0.30f);
    glowStar(ox, oy, (int)(7 + 10 * grow), tint, 0.62f);
  }
  const int n = 5;
  for (int i = 0; i < n; i++) {
    const float f = ((float)(i + 1) / n) * grow;
    if (f <= 0.02f) continue;
    const int x = (int)(ox + (tx - ox) * f + sinf(t * 4.0f + f * 6.0f) * 9.0f * f);
    const int y = (int)(oy + (ty - oy) * f + cosf(t * 3.5f + f * 5.0f) * 7.0f * f);
    const float ang = atan2f((float)(ty - oy), (float)(tx - ox)) + (i & 1 ? 0.55f : -0.35f);
    drawAtkRot(ATK_WHIP, x, y, 40, 12, ang, tint, 0.08f, 0.0f, 0.95f);
  }
  if (grow > 0.82f) shadeDisc(tx - 2, ty, 3, lerp565(tint, 0xFFFF, 0.55f), 0.95f);
  if (p > 0.48f && p < 0.90f) atkImpact(m, tx, ty, (p - 0.48f) / 0.38f);
}

// Shadow Ball is a charged purple ball, not a generic fire-coloured sphere.
static void drawShadowBallMove(const MoveDef &m, float p, float t) {
  const int ox = atkOriginX(), oy = atkOriginY();
  const int tx = atkTargetX(), ty = atkTargetY();
  const uint16_t tint = C(0x9A5CFF);
  const float charge = clamp01(p / 0.32f);
  if (p < 0.34f) {
    const float s = 0.35f + 0.55f * charge;
    drawAtkCenter(ATK_SHADOWBALL, ox, oy, s, tint, 0.05f, 0.0f, 0.85f);
    glowRing(ox, oy, (int)(14 + 22 * charge), (int)(10 + 16 * charge), tint, 0.35f);
    glowStar(ox, oy, (int)(7 + 13 * charge), tint, 0.62f);
  }
  const float q = clamp01((p - 0.26f) / 0.52f);
  if (q > 0.0f && q < 1.0f) {
    const int x = (int)(ox + (tx - ox) * q);
    const int y = (int)(oy + (ty - oy) * q - sinf(3.14159f * q) * 16.0f);
    castShadow(x, y, 9);
    drawAtkCenter(ATK_SHADOWBALL, x, y, 1.0f, tint, 0.06f, 0.10f, 1.0f);
    glowHalo(x, y, 15, tint, 0.36f);
    for (int k = 0; k < 4; k++) {
      const float a = t * 6.0f + k * 1.5708f;
      fb.fillCircle(x + (int)(cosf(a) * 13), y + (int)(sinf(a) * 10),
                    1 + (k & 1), lerp565(tint, 0xFFFF, 0.72f));
    }
  }
  if (p > 0.55f && p < 0.90f) atkImpact(m, tx, ty, (p - 0.55f) / 0.34f);
}

// Water Gun is a pressurised stream with droplets, not a slow orb. The blue
// streak sheet is the one the Water Gun script uses.
static void drawWaterGunMove(const MoveDef &m, float p, float t) {
  const int ox = atkOriginX(), oy = atkOriginY();
  const int tx = atkTargetX(), ty = atkTargetY();
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  if (p < 0.24f) {
    const float q = clamp01(p / 0.24f);
    glowHalo(ox, oy, (int)(8 + 10 * q), tint, 0.34f * q);
    glowStar(ox, oy, (int)(7 + 10 * q), tint, 0.72f);
  }
  const float fire = clamp01((p - 0.12f) / 0.30f);
  const int x1 = ox + (int)((tx - ox) * fire);
  const int y1 = oy + (int)((ty - oy) * fire);
  const float ang = atan2f((float)(y1 - oy), (float)(x1 - ox));
  for (int i = 0; i < 9; i++) {
    const float q = fmodf(t * 1.6f + i / 9.0f, 1.0f);
    if (q > fire) continue;
    const int x = (int)(ox + (x1 - ox) * q);
    const int y = (int)(oy + (y1 - oy) * q + sinf(t * 5.0f + i * 2.1f) * 3.0f);
    drawAtkRot(ATK_STRIKE, x, y, 28, 9, ang, tint, 0.04f, 0.0f, 0.90f);
    drawAtkCenter(ATK_WATER, x + 3, y - 4 + (int)(sinf(t * 4 + i) * 3), 0.22f,
                  tint, 0.05f, 0.0f, 0.95f);
  }
  if (p > 0.50f && p < 0.88f) atkImpact(m, tx, ty, (p - 0.50f) / 0.34f);
}

// Fire Blast is the five-pointed fire star, not a small orange ball. The
// individual points reuse the archive's flame cells; the layout is the move's
// own silhouette.
static void drawFireBlastMove(const MoveDef &m, float p, float t) {
  const int ox = atkOriginX(), oy = atkOriginY();
  const int tx = atkTargetX(), ty = atkTargetY();
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  if (p < 0.34f) {
    const float q = clamp01(p / 0.34f);
    glowHalo(ox, oy, (int)(10 + 14 * q), tint, 0.36f * q);
    drawAtkCenter(ATK_FLAME, ox, oy, 0.55f + 0.55f * q, tint, 0.10f, 0.0f, 0.95f);
    glowStar(ox, oy, (int)(7 + 12 * q), tint, 0.70f);
  }
  const float q = clamp01((p - 0.24f) / 0.34f);
  if (q > 0.0f && q < 1.0f) {
    const int x = (int)(ox + (tx - ox) * q);
    const int y = (int)(oy + (ty - oy) * q - sinf(3.14159f * q) * 14.0f);
    castShadow(x, y, 8);
    drawAtkCenter(ATK_FLAME, x, y, 0.85f, tint, 0.04f, 0.08f, 1.0f);
    glowHalo(x, y, 12, tint, 0.32f);
  }
  const float star = clamp01((p - 0.38f) / 0.28f);
  if (star > 0.0f) {
    // The points sit outside the defender's body; inside that radius the
    // sprite would hide the whole star again.
    const int r = (int)(34 + 14 * star);
    for (int k = 0; k < 5; k++) {
      const float a = k * 1.2566f - 1.5708f + t * 0.35f;
      const int x = tx + (int)(cosf(a) * r);
      const int y = ty + (int)(sinf(a) * r * 0.82f);
      drawAtkRot(ATK_FLAME, x, y, 18, (int)(24 + 10 * star), a + 1.5708f,
                 tint, 0.04f, 0.10f, 1.0f);
      drawAtkCenter(ATK_FLAME2, x + 3, y - 4, 0.60f, tint, 0.0f, 0.10f, 0.95f);
    }
    glowStar(tx, ty, (int)(10 + 14 * star), tint, 0.82f * star);
  }
  if (p > 0.54f && p < 0.92f) atkImpact(m, tx, ty, (p - 0.54f) / 0.38f);
}

// Aura Sphere is the blue spiral charge and its white-core shot from the
// archive, not a recoloured generic orb.
static void drawAuraSphereMove(const MoveDef &m, float p, float t) {
  const int ox = atkOriginX(), oy = atkOriginY();
  const int tx = atkTargetX(), ty = atkTargetY();
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  const float charge = clamp01(p / 0.34f);
  if (p < 0.38f) {
    const float s = 0.45f + 0.55f * charge;
    drawAtkRot(ATK_AURA, ox, oy, (int)(42 * s), (int)(42 * s), t * 1.8f,
               tint, 0.08f, 0.0f, 0.95f);
    glowRing(ox, oy, (int)(14 + 20 * charge), (int)(10 + 14 * charge), tint, 0.32f);
  }
  const float q = clamp01((p - 0.28f) / 0.48f);
  if (q > 0.0f && q < 1.0f) {
    const int x = (int)(ox + (tx - ox) * q);
    const int y = (int)(oy + (ty - oy) * q - sinf(3.14159f * q) * 12.0f);
    const float ang = atan2f((float)(ty - oy), (float)(tx - ox));
    castShadow(x, y, 8);
    drawAtkRot(ATK_AURASHOT, x, y, 34, 13, ang, tint, 0.05f, 0.10f, 1.0f);
    glowHalo(x, y, 13, tint, 0.32f);
  }
  if (p > 0.54f && p < 0.90f) atkImpact(m, tx, ty, (p - 0.54f) / 0.36f);
}

// Psychic is a pressure wave around the defender, not a thrown ball. The
// archive's psychic ripple is drawn as concentric sheets at the target.
static void drawPsychicMove(const MoveDef &m, float p, float t) {
  const int ox = atkOriginX(), oy = atkOriginY();
  const int tx = atkTargetX(), ty = atkTargetY();
  const uint16_t tint = C(0xE05CC8);
  if (p < 0.30f) {
    const float q = clamp01(p / 0.30f);
    drawAtkCenter(ATK_PSYRING, ox, oy, 0.35f + 0.45f * q, tint, 0.18f, 0.0f, 0.72f);
    glowStar(ox, oy, (int)(7 + 12 * q), tint, 0.62f);
  }
  const float q = clamp01((p - 0.20f) / 0.56f);
  if (q > 0.0f) {
    for (int k = 0; k < 3; k++) {
      const float qq = clamp01(q - k * 0.13f);
      if (qq <= 0.0f) continue;
      drawAtkCenter(ATK_PSYRIPPLE, tx, ty, 0.55f + 0.85f * qq, tint,
                    0.16f, 0.0f, 0.60f * (1.0f - qq * 0.55f));
    }
    targetHit = fmaxf(targetHit, 0.34f * q);
    glowRing(tx, ty, (int)(24 + 36 * q), (int)(18 + 26 * q), tint, 0.28f * (1.0f - q));
  }
  if (p > 0.52f && p < 0.90f) atkImpact(m, tx, ty, (p - 0.52f) / 0.36f);
}

// The attack state is resolved before layout so the quake shake moves the
// creature in the same frame the ground does.
static void updateAttack(float t) {
  const float at = t - ATK_START;
  atkActive = (phase != P_MELT) && at >= 0.0f && at < ATK_LEN;
  atkFlash = 0.0f;
  shake = 0.0f;
  if (!atkActive) {
    atkP = 0.0f;
    atkLabel = false;
    return;
  }
  atkP = clamp01(at / ATK_LEN);
  atkLabel = at < ATK_LEN + 0.22f;
  const MoveDef &m = MOVES[moveIdx];
  // One registered hit per entry. The 100,000 HP pool is spent across exactly
  // 649 attacks; rounding is carried in dittoHits so the final hit reaches zero.
  if (!dittoHitCounted && !dittoFaint && atkP > 0.72f) {
    dittoHitCounted = true;
    if (dittoHits < DITTO_HITS) dittoHits++;
    // The bar loses what this move really takes off a level-50 Ditto, sized at
    // build time so one full pass spends the 100,000 pool exactly. A status
    // move, or a Ghost move into a Normal Ditto, does no damage at all -- the
    // same answer the games give.
    dittoHp -= SIG[idx].hp;
    if (dittoHp < 0) dittoHp = 0;
    targetHit = fmaxf(targetHit, 0.72f);
    if (dittoHp == 0) {
      dittoFaint = true;
      dittoFaintAt = millis();
    }
  }
  if (m.fam == AF_QUAKE) {
    shake = atkP > 0.04f ? 2.5f * (1.0f - atkP) : 0.0f;
  } else if (m.fam == AF_SLASH) {
    if (atkP > 0.32f) shake = 1.3f * (1.0f - atkP);
  } else if (m.fam == AF_BEAM || m.fam == AF_BREATH || m.fam == AF_BOLT ||
             m.fam == AF_STORM) {
    if (atkP > 0.55f) shake = 1.5f * (1.0f - atkP);
  }
}

static void drawAttackBack(const MoveDef &m, float p, float t) {
  const uint16_t tint = C(TYPE_STYLE[m.type].rgb);
  if (m.fam == AF_BEAM || m.fam == AF_BREATH || m.fam == AF_BOLT ||
      m.fam == AF_ORB || m.fam == AF_SLASH || m.fam == AF_RAIN)
    glowFloor(tint, 0.65f);
  if (m.fam == AF_BEAM || m.fam == AF_BREATH || m.fam == AF_BOLT || m.fam == AF_ORB) {
    const int ox = atkOriginX(), oy = atkOriginY();
    glowStar(ox, oy, (int)(16 + 7 * p), tint, 0.58f);
    glowRing(ox, oy, 30, 30, tint, 0.30f);
    // Motes circling the charge point. They are cheap, but they turn a round
    // glow into something that is visibly gathering power.
    for (int i = 0; i < 7; i++) {
      const float a = t * 4.2f + i * 0.8976f;
      const int rr = (int)((1.0f - p * 0.45f) * (24.0f + 10.0f * fxA));
      const int px = ox + (int)(cosf(a) * rr), py = oy + (int)(sinf(a) * rr * 0.7f);
      const uint16_t mot = lerp565(tint, 0xFFFF, 0.75f);
      fb.fillCircle(px, py, 2, mot);
      fb.drawFastHLine(px - 2, py, 5, mot);
    }
  }
  if (m.fam == AF_QUAKE) drawQuakeBack(m, p, t);
  else if (m.fam == AF_AURA) drawAuraBack(m, p, t);
}

static void drawAttackFront(const MoveDef &m, float p, float t) {
  // Recognisable, move-specific silhouettes come before the family fallback.
  if (isFistMove(m)) { drawFistMove(m, p, t); return; }
  if (isSkyBoltMove(m)) { drawSkyBoltMove(m, p, t); return; }
  if (strcmp(m.name, "VINEWHIP") == 0) { drawWhipMove(m, p, t); return; }
  if (m.tex1 == ATK_SHADOWBALL) { drawShadowBallMove(m, p, t); return; }
  if (strcmp(m.name, "WATERGUN") == 0) { drawWaterGunMove(m, p, t); return; }
  if (strcmp(m.name, "AURASPHERE") == 0) { drawAuraSphereMove(m, p, t); return; }
  if (strcmp(m.name, "PSYCHIC") == 0) { drawPsychicMove(m, p, t); return; }
  if (strcmp(m.name, "FIREBLAST") == 0) { drawFireBlastMove(m, p, t); return; }

  switch (m.fam) {
    case AF_BEAM: drawBeam(m, p, t); break;
    case AF_BREATH: drawBreath(m, p, t); break;
    case AF_BOLT: drawBolt(m, p, t); break;
    case AF_ORB: drawOrb(m, p, t); break;
    case AF_SLASH: drawSlash(m, p, t); break;
    case AF_RAIN: drawRain(m, p, t); break;
    case AF_QUAKE: drawQuakeFront(m, p, t); break;
    case AF_AURA: drawAuraFront(m, p, t); break;
    case AF_WAVE: drawWave(m, p, t); break;
    case AF_STORM: drawStorm(m, p, t); break;
    default: break;
  }
}

// ---------------------------------------------------------------- the HUD -----

static void drawHud(const DexEntry &e, float hold) {
  fb.fillRect(0, HUD_Y, W, H - HUD_Y, C(0x0B0910));
  fb.drawFastHLine(0, HUD_Y, W, lerp565(C(0x2A2438), look.accent, 0.55f));

  // Left side is the battle log: the current signature move. It used to share
  // the line with the name and type; separating them gives both room to read.
  fb.setFont(&fonts::Font0);
  fb.setTextColor(C(0x8A82A0));
  fb.drawString("MOVE", 8, HUD_Y + 3);
  // The move's real type and base power, the way the games list them. A status
  // move has no power number, and a type-pool stand-in has no data, so both say
  // what they are instead of printing a zero.
  const SigData &sg = SIG[idx];
  const uint8_t mt = (uint8_t)MOVES[moveIdx].type;
  char meta[20];
  if (!sg.move[0]) snprintf(meta, sizeof(meta), "%s", TYPE_STYLE[mt].label);
  else if (sg.power > 0)
    snprintf(meta, sizeof(meta), "%s %u", TYPE_STYLE[mt].label, (unsigned)sg.power);
  else snprintf(meta, sizeof(meta), "%s STATUS", TYPE_STYLE[mt].label);
  fb.setTextColor(C(TYPE_STYLE[mt].rgb));
  fb.drawString(meta, 40, HUD_Y + 3);
  fb.setFont(&fonts::FreeSansBold9pt7b);
  fb.setTextColor(look.glow);
  fb.drawString(MOVES[moveIdx].name, 8, HUD_Y + 12);

  // Player bar to the right of the main Pokémon: number, name and type chips.
  const int px = 176, py = HUD_Y + 1, pw = W - px - 4, ph = H - HUD_Y - 3;
  fb.fillRoundRect(px, py, pw, ph, 4, C(0x120E20));
  fb.drawRoundRect(px, py, pw, ph, 4, lerp565(C(0x2A2438), look.accent, 0.55f));

  char num[12];
  snprintf(num, sizeof(num), "No.%03d", idx + 1);
  fb.setFont(&fonts::Font0);
  fb.setTextColor(C(0x8A82A0));
  fb.drawString(num, px + 6, py + 7);

  fb.setFont(&fonts::FreeSansBold9pt7b);
  fb.setTextColor(C(0xF4F0FA));
  fb.drawString(e.name, px + 44, py + 1);

  fb.setFont(&fonts::Font0);
  int chipX = px + 6;
  for (int k = 0; k < 2; k++) {
    const int8_t t = k == 0 ? e.t1 : e.t2;
    if (t < 0) continue;
    const char *lab = TYPE_STYLE[t].label;
    const int cw = fb.textWidth(lab) + 8;
    fb.fillRoundRect(chipX, py + 16, cw, 12, 6, C(TYPE_STYLE[t].rgb));
    fb.setTextColor(C(0x0B0910));
    fb.drawString(lab, chipX + 4, py + 18);
    chipX += cw + 4;
  }

  const int barW = (int)(W * hold);
  fb.fillRect(0, H - 2, W, 2, C(0x1A1626));
  fb.fillRect(0, H - 2, barW, 2, look.accent);
}

// ---------------------------------------------------------------- the scene ---

static void enterEntry(int i) {
  const int next = ((i % DEX_COUNT) + DEX_COUNT) % DEX_COUNT;
  // A pass starts with a fresh Ditto. In an undisturbed 649-entry run the pool
  // is already empty at the end; this also covers NEXT/PREV and GOTO jumps,
  // which spend the pool at a different rate than the sum assumes.
  const bool newPass = next == 0 && idx == DEX_COUNT - 1;
  idx = next;
  const DexEntry &e = DEX[idx];
  biome = biomeFor(e.t1, e.t2);
  const BiomeStyle &b = BIOMES[biome];
  scene.skyTop = C(b.skyTop);
  scene.skyMid = C(b.skyMid);
  scene.skyBot = C(b.skyBot);
  scene.far = C(b.far);
  scene.mid = C(b.mid);
  scene.near = C(b.near);
  scene.ground = C(b.ground);
  scene.groundTop = C(b.groundTop);

  deriveLook(e.t1, e.t2);
  {
    const uint32_t fh = hash32((uint32_t)(idx + 1) * 2654435761u + 17u);
    fxA = (fh & 0xFF) / 255.0f;
    fxB = ((fh >> 8) & 0xFF) / 255.0f;
    fxC = ((fh >> 16) & 0xFF) / 255.0f;
  }
  gFlip = mirrorFor(idx + 1);
  // The painted scene's own floor at the feet, sampled by the baker. A little of
  // the creature's colour is mixed in so the reflection belongs to both.
  floorTint = lerp565(BGS[biome].floor, look.base, 0.08f);
  moveIdx = pickMove(idx + 1, e.t1, e.t2);
  entryAt = millis();
  phase = P_IN;
  animT = 0.0f;
  reacted = false;
  atkActive = false;
  atkLabel = false;
  atkP = 0.0f;
  atkFlash = 0.0f;
  shake = 0.0f;
  targetHit = 0.0f;
  dittoHitCounted = false;
  if (newPass || dittoHp <= 0) {
    dittoHits = 0;
    dittoHp = DITTO_HP_MAX;
    dittoFaint = false;
    dittoRespawnAt = millis();
  }
  bgCached = false;

  if (quiet || !verbose) return;
  Serial.printf("show %03d %-11s biome=%-7s type=%s%s%s move=%s (%s)\n", idx + 1, e.name,
                BIOME_NAME[biome], TYPE_STYLE[e.t1].label,
                e.t2 >= 0 ? "/" : "", e.t2 >= 0 ? TYPE_STYLE[e.t2].label : "",
                MOVES[moveIdx].name, FAM_NAME[MOVES[moveIdx].fam]);
}

static float ballTilt() {
  const float s = animT < 0.55f
      ? sinf(animT * 47.0f) * 0.45f * (1.0f - animT / 0.55f) : 0.0f;
  return 0.28f * sinf(animT * 0.85f) + s;
}
static float ballPulse() { return 0.5f + 0.5f * sinf(animT * 2.3f); }

static void startMelt() {
  if (!haveNext) {
    // BLE build: no second frame to dissolve into. Switch directly to the next
    // entry and keep the same six-second cadence.
    quiet = true;
    enterEntry(idx + 1);
    quiet = false;
    phase = P_HOLD;
    entryAt = millis();
    return;
  }
  const uint32_t t0 = micros();
  fb.pushSprite(&nb, 0, 0);
  const uint32_t d = micros() - t0;
  freezeUs += d;
  if (d > freezeWorst) freezeWorst = d;
  quiet = true;
  enterEntry(idx + 1);
  quiet = false;
  phase = P_MELT;
  entryAt = millis();
  if (verbose) Serial.println("melt");
}

static int16_t wobX[W];
static int16_t frontX[W];
static int meltMin = 0, meltMax = 0;

static void buildWobble() {
  for (int x = 0; x < W; x++) {
    const float w = 0.085f * sinf(x * 0.043f) + 0.05f * sinf(x * 0.11f + 1.7f)
                  + 0.04f * sinf(x * 0.21f + 0.4f);
    wobX[x] = (int16_t)(w * (float)H);
  }
}

static void meltFronts(float p) {
  const float e = p * (1.10f - 0.10f * p);
  const int base = -18 + (int)(e * (float)(H + 40));
  meltMin = H;
  meltMax = 0;
  for (int x = 0; x < W; x++) {
    int f = base + wobX[x];
    if (f < 0) f = 0;
    if (f > H) f = H;
    frontX[x] = (int16_t)f;
    if (f < meltMin) meltMin = f;
    if (f > meltMax) meltMax = f;
  }
}

static void applyMelt() {
  if (!haveNext || meltMin >= H) return;
  uint16_t *dst = (uint16_t *)fb.getBuffer();
  const uint16_t *src = (const uint16_t *)nb.getBuffer();
  if (!dst || !src) return;
  for (int y = meltMin; y < meltMax; y++) {
    uint16_t *drow = dst + y * W;
    const uint16_t *srow = src + y * W;
    int x = 0;
    while (x < W) {
      while (x < W && frontX[x] > y) x++;
      const int start = x;
      while (x < W && frontX[x] <= y) x++;
      if (x > start)
        memcpy(drow + start, srow + start, (size_t)(x - start) * sizeof(uint16_t));
    }
  }
}

static void drawScene() {
  const DexEntry &e = DEX[idx];
  const uint32_t frameT0 = micros();
  const uint32_t now = millis();
  const uint32_t dt = now - entryAt;
  animT = dt / 1000.0f;

  if (phase == P_MELT && haveNext) {
    meltFronts(clamp01((float)dt / OUT_MS));
    fb.setClipRect(0, 0, W, meltMax > 0 ? meltMax : 1);
  }

  // Refill the RAM copy only outside a melt: during one, nb is the frozen frame.
  if (phase != P_MELT) cacheBackdrop();
  uint32_t _mark = micros();
  drawBackdrop((phase == P_MELT && haveNext) ? meltMax : H);
  TICK(bgUs);
  _mark = micros();

  // A little light behind the creature, sampled from the scene it is actually
  // standing in. It peaks a third of the way up so there is no neon horizon.
  for (int i = 0; i < 18; i++) {
    const int y = GROUND_Y - 1 - i;
    const float a = 0.16f * sinf(3.14159f * (float)i / 17.0f);
    fb.drawFastHLine(0, y, W, lerp565(fb.readPixel(1, y), look.accent, a));
  }
  drawWeather(animT, BIOMES[biome].weather);
  TICK(worldUs);

  updateAttack(animT);
  targetHit *= 0.90f;
  drawMainPlatform();

  // --- motion: arrival, then the type's idle, with a lunge on physical moves
  float k = 1.0f, squash = 1.0f, bob = 0.0f, refl = 1.0f;
  int drop = 0, sway = 0;
  float reveal = 1.0f;
  if (phase == P_IN) {
    const float t = clamp01((float)dt / IN_MS);
    const float ex = easeOutCubic(t);
    reveal = clamp01(t * 1.55f);
    refl = 0.15f + 0.85f * ex;
    switch (arrive) {
      case A_DESCEND:
        k = 0.55f + 0.45f * ex;
        drop = (int)((1.0f - ex) * 74.0f);
        break;
      case A_RISE:
        k = 0.60f + 0.40f * ex;
        drop = -(int)((1.0f - ex) * 34.0f);
        refl = 0.0f;
        break;
      case A_FADE:
        k = 0.82f + 0.18f * ex;
        reveal = clamp01(t * 1.05f);
        break;
      case A_POP:
      default:
        k = 0.30f + 0.70f * easeOutBack(t);
        squash = 1.0f + 0.16f * sinf(t * 3.14159f);
        drop = (int)((1.0f - ex) * 46.0f);
        break;
    }
  } else {
    const float ph = animT * idle.bobHz * 6.2832f;
    bob = sinf(ph) * idle.bobAmp;
    sway = (int)(sinf(ph * 0.7f) * idle.swayAmp);
    if (idle.jitter > 0.0f) bob += sinf(animT * 38.0f) * idle.jitter;
    squash = 1.0f + 0.012f * sinf(animT * 2.4f);
  }

  // The creature is part of the action, not a sticker in front of it. Every
  // family gets a wind-up and a follow-through; the beam leans back and recoils,
  // the physical moves lunge, the ground moves jump and slam, the aura floats.
  if (atkActive) {
    const MoveDef &m = MOVES[moveIdx];
    const float wind = sinf(clamp01(atkP / 0.22f) * 3.14159f);
    const float swing = sinf(clamp01((atkP - 0.18f) / 0.45f) * 3.14159f);
    switch (m.fam) {
      case AF_BEAM: case AF_BREATH: case AF_BOLT:
        sway -= (int)(6.0f * wind);
        drop += (int)(2.0f * wind);
        squash = 1.0f + 0.02f * wind - 0.02f * swing;
        if (atkP > 0.20f) sway -= (int)(4.0f * swing);
        break;
      case AF_SLASH:
        sway += (int)(16.0f * swing);
        drop += (int)(4.0f * swing);
        squash = 1.0f + 0.03f * swing;
        break;
      case AF_QUAKE:
        drop -= (int)(10.0f * wind);
        drop += (int)(9.0f * swing);
        squash = 1.0f + 0.04f * wind;
        break;
      case AF_AURA:
        bob += 4.0f * wind;
        squash = 1.0f + 0.03f * wind;
        break;
      case AF_RAIN:
        drop -= (int)(5.0f * wind);
        drop += (int)(5.0f * swing);
        break;
      case AF_WAVE:
        sway += (int)(9.0f * sinf(atkP * 6.2832f));
        break;
      case AF_STORM:
        sway += (int)(6.0f * sinf(atkP * 9.4248f));
        break;
      default: break;
    }
  }
  if (shake > 0.01f) {
    sway += (int)(shake * sinf(animT * 88.0f));
    drop += (int)(shake * cosf(animT * 77.0f));
  }
  layoutCreature(k, squash, bob, drop, sway);

  if (atkActive && !reacted) {
    reacted = true;
    if (verbose) Serial.printf("move %s\n", MOVES[moveIdx].name);
  }

  if (atkActive) drawAttackBack(MOVES[moveIdx], atkP, animT);

  drawReflection(refl * (atkActive && MOVES[moveIdx].fam == AF_QUAKE ? 0.55f : 1.0f));
  fb.fillEllipse(CX + sway, GROUND_Y + 1, gW / 2 + 1, 3,
                 lerp565(floorTint, C(0x0A0810), 0.48f));

  if (atkActive && !isFistMove(MOVES[moveIdx]) && MOVES[moveIdx].fam != AF_SLASH)
    drawAttackFront(MOVES[moveIdx], atkP, animT);

  const uint32_t blitT0 = micros();
  blitCreature(atkFlash, reveal);
  blitUs += micros() - blitT0;

  // The opponent is drawn over beam/orb effects so its silhouette stays
  // readable. Fists and slashes are contact effects, so they get one more pass
  // on top of the opponent after this.
  drawTarget();
  if (atkActive) {
    const MoveDef &m = MOVES[moveIdx];
    if (isFistMove(m) || m.fam == AF_SLASH) drawAttackFront(m, atkP, animT);
  }

  if (reveal < 1.0f) {
    const int ry = gY + (int)(reveal * gH);
    fb.fillRect(gX - 5, ry, gW + 10, 1, look.glow);
    fb.fillRect(gX - 5, ry + 1, gW + 10, 1, lerp565(look.glow, scene.skyMid, 0.5f));
  }

  float hold = 1.0f;
  if (phase == P_HOLD) hold = 1.0f - clamp01((float)(dt - IN_MS) / HOLD_MS);
  fxUs += micros() - _mark;
  _mark = micros();
  // Opponent HP is UI, so it sits above the attack effects instead of being
  // crossed by a Shadow Ball or a Hyper Beam.
  drawDittoHp();
  drawHud(e, hold);
  TICK(hudUs);

  drawPokeball(19, 19, 11, ballSpin + ballTilt(), ballPulse());

  if (phase == P_MELT) {
    if (haveNext) fb.clearClipRect();
    const uint32_t apT0 = micros();
    applyMelt();
    meltApplyUs += micros() - apT0;
    // The ball was just painted into the new frame; draw it again after the
    // frozen band is restored so it is not cut in half by the melt.
    drawPokeball(19, 19, 11, ballSpin + ballTilt(), ballPulse());
    if (haveSprite) {
      const uint32_t pushT0 = micros();
      fb.pushSprite(&lcd, 0, 0);
      pushUs += micros() - pushT0;
    }
    const uint32_t md = micros() - frameT0;
    meltUs += md;
    meltFrames++;
    if (md > meltWorst) meltWorst = md;
    return;
  }

  if (haveSprite) {
    const uint32_t pushT0 = micros();
    fb.pushSprite(&lcd, 0, 0);
    pushUs += micros() - pushT0;
  }
}

// ------------------------------------------------------------------ runtime ---

static String cmdBuf;

static void takeShot() {
  uint32_t runs = 0;
  uint16_t prev = 0;
  bool first = true;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      const uint16_t c = fb.readPixel(x, y);
      if (first || c != prev) { runs++; prev = c; first = false; }
    }
  }
  Serial.printf("SHOT %u %d %d\n", (unsigned)runs, W, H);

  uint8_t buf[4];
  uint32_t len = 0;
  prev = 0;
  first = true;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      const uint16_t c = fb.readPixel(x, y);
      if (first) { prev = c; len = 1; first = false; continue; }
      if (c == prev) { len++; continue; }
      buf[0] = len & 0xFF; buf[1] = (len >> 8) & 0xFF;
      buf[2] = prev & 0xFF; buf[3] = (prev >> 8) & 0xFF;
      Serial.write(buf, 4);
      prev = c;
      len = 1;
    }
  }
  if (len) {
    buf[0] = len & 0xFF; buf[1] = (len >> 8) & 0xFF;
    buf[2] = prev & 0xFF; buf[3] = (prev >> 8) & 0xFF;
    Serial.write(buf, 4);
  }
  Serial.println();
}

static void resetPerf() {
  frameUsSum = frameCount = blitUs = pushUs = 0;
  bgUs = worldUs = fxUs = hudUs = 0;
  meltUs = meltFrames = meltWorst = 0;
  freezeUs = freezeWorst = meltApplyUs = 0;
  gapMeltWorst = gapNormalWorst = 0;
}

static void runCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  if (cmd.equalsIgnoreCase("NEXT")) {
    enterEntry(idx + 1);
    return;
  }
  if (cmd.equalsIgnoreCase("PREV")) {
    enterEntry(idx - 1);
    return;
  }
  if (cmd.equalsIgnoreCase("VERBOSE")) {
    verbose = !verbose;
    Serial.printf("verbose %s\n", verbose ? "on" : "off");
    return;
  }
  if (cmd.equalsIgnoreCase("PAUSE")) {
    paused = !paused;
    Serial.printf("paused %s\n", paused ? "yes" : "no");
    return;
  }
  if (cmd.startsWith("GOTO ") || cmd.startsWith("goto ")) {
    const int n = cmd.substring(5).toInt();
    if (n >= 1 && n <= DEX_COUNT) enterEntry(n - 1);
    else Serial.printf("GOTO 1..%d\n", DEX_COUNT);
    return;
  }
  if (cmd.equalsIgnoreCase("INFO")) {
    const DexEntry &e = DEX[idx];
    Serial.printf("entry %d/%d %s, biome %s, move %s (%s), %.1f s into a %u ms cycle, %s\n",
                  idx + 1, DEX_COUNT, e.name, BIOME_NAME[biome],
                  MOVES[moveIdx].name, FAM_NAME[MOVES[moveIdx].fam],
                  animT, (unsigned)CYCLE_MS, paused ? "paused" : "playing");
    return;
  }
  if (cmd.equalsIgnoreCase("OFF")) {
    Serial.println("power down: backlight off, deep sleep (reset to wake)");
    Serial.flush();
    lcd.setBrightness(0);
    lcd.sleep();
    if (haveNext) nb.deleteSprite();
    if (haveSprite) fb.deleteSprite();
    delay(150);
    esp_deep_sleep_start();
    return;
  }
  if (cmd.equalsIgnoreCase("SHOT")) {
    takeShot();
    return;
  }
  if (cmd.equalsIgnoreCase("SCAN")) {
    quiet = true;
    const int start = idx;
    uint32_t worst = 0, total = 0;
    int worstAt = 0;
    resetPerf();
    for (int i = 0; i < DEX_COUNT; i++) {
      enterEntry(i);
      phase = P_HOLD;
      entryAt = millis() - IN_MS - 1500;     // landed, in the middle of the move
      const uint32_t a = micros();
      drawScene();
      const uint32_t d = micros() - a;
      total += d;
      if (d > worst) { worst = d; worstAt = i; }
      if ((i + 1) % 100 == 0) Serial.printf("scan %d/%d\n", i + 1, DEX_COUNT);
    }
    quiet = false;
    const uint8_t worstMove = pickMove(worstAt + 1, DEX[worstAt].t1, DEX[worstAt].t2);
    Serial.printf("scan ok %d/%d, avg %.2f ms, worst %.2f ms at %03d %s (%s), heap %u\n",
                  DEX_COUNT, DEX_COUNT, (float)total / DEX_COUNT / 1000.0f,
                  worst / 1000.0f, worstAt + 1, DEX[worstAt].name,
                  MOVES[worstMove].name, ESP.getFreeHeap());
    resetPerf();
    enterEntry(start);
    return;
  }
  if (cmd.equalsIgnoreCase("PERF")) {
    const float avg = frameCount ? (float)frameUsSum / (float)frameCount / 1000.0f : 0.0f;
    Serial.printf("draw %.2f ms avg over %u frames (budget 16.6 ms at 60 fps, headroom %.1fx)\n",
                  avg, (unsigned)frameCount, avg > 0.01f ? 16.6f / avg : 0.0f);
    if (frameCount) {
      const float n = (float)frameCount;
      Serial.printf("  sprite %.2f ms, push %.2f ms, backdrop %.2f ms, world %.2f ms, "
                    "moves+creature %.2f ms, hud+ball %.2f ms\n",
                    (float)blitUs / n / 1000.0f, (float)pushUs / n / 1000.0f,
                    (float)bgUs / n / 1000.0f, (float)worldUs / n / 1000.0f,
                    (float)fxUs / n / 1000.0f, (float)hudUs / n / 1000.0f);
    }
    if (meltFrames) {
      Serial.printf("melt %.2f ms avg over %u frames, worst %.2f ms (a normal frame is %.2f ms)\n",
                    (float)meltUs / (float)meltFrames / 1000.0f, (unsigned)meltFrames,
                    meltWorst / 1000.0f, avg);
      Serial.printf("  dissolve %.2f ms/frame, freeze %.2f ms/frame (worst %.2f ms), "
                    "frame gap worst %u ms melt / %u ms normal\n",
                    (float)meltApplyUs / (float)meltFrames / 1000.0f,
                    (float)freezeUs / (float)meltFrames / 1000.0f, freezeWorst / 1000.0f,
                    (unsigned)gapMeltWorst, (unsigned)gapNormalWorst);
    } else {
      Serial.println("melt not measured (no transition since the last PERF)");
    }
    resetPerf();
    return;
  }
  Serial.println("commands: NEXT, PREV, GOTO <1-649>, PAUSE, INFO, PERF, SHOT, SCAN, VERBOSE, OFF");
}

static void handleSerial() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      runCommand(cmdBuf);
      cmdBuf = "";
    } else if (cmdBuf.length() < 40) {
      cmdBuf += c;
    }
  }
}

// --------------------------------------------------------- RainMaker / BLE ---
// The RainMaker app provisions over BLE and then exposes the same controls over
// its cloud/local channel. The UI is deliberately simple: Index is a 1..649
// slider, so dragging it left/right is the swipe gesture; Paused is a toggle.
static const char *RMAKER_SERVICE = "PROV_POKEDEX";
static const char *RMAKER_POP = "pokedex123";

// WARNING: this callback is called from the RainMaker task, not loop(). Only
// publish intent here; the display code applies it on the next frame.
static void pokedexWriteCb(Device *device, Param *param, const param_val_t val,
                           void *priv, write_ctx_t *ctx) {
  const char *name = param->getParamName();
  if (strcmp(name, "Index") == 0) {
    int n = val.val.i;
    if (n < 1) n = 1;
    if (n > DEX_COUNT) n = DEX_COUNT;
    pendingGoto = n;
    lastReportedIndex = n;
  } else if (strcmp(name, "Paused") == 0) {
    pendingPause = val.val.b ? 1 : 0;
    lastReportedPause = val.val.b ? 1 : 0;
  }
  param->updateAndReport(val);
}

static void sysProvEvent(arduino_event_t *sys_event) {
  switch (sys_event->event_id) {
    case ARDUINO_EVENT_PROV_START:
      Serial.printf("RainMaker BLE: %s, PoP %s\n", RMAKER_SERVICE, RMAKER_POP);
      break;
    case ARDUINO_EVENT_PROV_INIT:
      // Keep the BLE window open long enough to find the board in the app.
      WiFiProv.disableAutoStop(300000);
      break;
    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
      WiFiProv.endProvision();
      Serial.println("RainMaker provisioning complete");
      break;
    default:
      break;
  }
}

static void startRainMaker() {
  Node node = RMaker.initNode("Pokedex T-Display");
  pokedexDev = new Device("Pokedex", "custom.device.pokedex", nullptr);
  if (!pokedexDev) {
    Serial.println("RainMaker: device allocation failed");
    return;
  }

  Param index("Index", "custom.param.index", value(1),
              PROP_FLAG_READ | PROP_FLAG_WRITE);
  index.addBounds(value(1), value(DEX_COUNT), value(1));
  index.addUIType(ESP_RMAKER_UI_SLIDER);
  pokedexDev->addParam(index);

  Param pause("Paused", "custom.param.paused", value(false),
              PROP_FLAG_READ | PROP_FLAG_WRITE);
  pause.addUIType(ESP_RMAKER_UI_TOGGLE);
  pokedexDev->addParam(pause);

  pokedexDev->assignPrimaryParam(pokedexDev->getParamByName("Index"));
  pokedexDev->addCb(pokedexWriteCb);
  node.addDevice(*pokedexDev);

  RMaker.start();
  WiFi.onEvent(sysProvEvent);
  WiFiProv.initProvision(NETWORK_PROV_SCHEME_BLE, NETWORK_PROV_SCHEME_HANDLER_FREE_BTDM);
  WiFiProv.beginProvision(NETWORK_PROV_SCHEME_BLE, NETWORK_PROV_SCHEME_HANDLER_FREE_BTDM,
                          NETWORK_PROV_SECURITY_1, RMAKER_POP, RMAKER_SERVICE);
  Serial.println("RainMaker started; connect the app to PROV_POKEDEX");
}

static void applyRemote() {
  if (pendingPause >= 0) {
    paused = pendingPause != 0;
    pendingPause = -1;
  }
  if (pendingGoto > 0 && phase != P_MELT) {
    const int n = pendingGoto;
    pendingGoto = 0;
    enterEntry(n - 1);
  }
}

static void reportRemote() {
  if (!pokedexDev || WiFi.status() != WL_CONNECTED) return;
  const int n = idx + 1;
  if (n != lastReportedIndex) {
    pokedexDev->updateAndReportParam("Index", n);
    lastReportedIndex = n;
  }
  const int p = paused ? 1 : 0;
  if (p != lastReportedPause) {
    pokedexDev->updateAndReportParam("Paused", paused);
    lastReportedPause = p;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\npokedex: %d entries, 6 s each\n", DEX_COUNT);

  pinMode(15, OUTPUT);
  digitalWrite(15, HIGH);

  lcd.init();
  lcd.setRotation(1);
  lcd.setBrightness(255);
  lcd.fillScreen(TFT_BLACK);

  buildWobble();

  fb.setColorDepth(16);
  haveSprite = fb.createSprite(W, H) != nullptr;
  // RainMaker's BLE provisioning stack needs the internal RAM that the frozen
  // melt frame used to occupy. The display still uses one 108 KB framebuffer;
  // the world change becomes a hard cut instead of a dissolve.
  haveNext = false;
  Serial.printf("sprite: %s, melt buffer: disabled for BLE, heap %u\n",
                haveSprite ? "ok" : "unavailable, drawing direct", ESP.getFreeHeap());

  enterEntry(0);
  startRainMaker();
}

void loop() {
  static uint32_t lastFrame = 0;
  const uint32_t nowMs = millis();
  applyRemote();
  if (nowMs - lastFrame < 16) {
    handleSerial();
    delay(1);
    return;
  }
  const uint32_t gap = nowMs - lastFrame;
  static bool haveGap = false;
  if (haveGap) {
    if (phase == P_MELT) {
      if (gap > gapMeltWorst) gapMeltWorst = gap;
    } else if (gap > gapNormalWorst) {
      gapNormalWorst = gap;
    }
  } else {
    haveGap = true;
  }
  lastFrame = nowMs;
  if (haveGap) {
    ballSpin += (float)gap * 0.001f * (6.2831853f / (CYCLE_MS * 0.001f));
    if (ballSpin >= 6.2831853f) ballSpin -= 6.2831853f;
  }
  handleSerial();
  applyRemote();
  reportRemote();

  const uint32_t dt = millis() - entryAt;
  if (!paused) {
    if (phase == P_IN && dt >= IN_MS) {
      phase = P_HOLD;
      entryAt = millis() - IN_MS;
    } else if (phase == P_HOLD && dt >= CYCLE_MS) {
      startMelt();
    } else if (phase == P_MELT && dt >= OUT_MS) {
      // startMelt() already entered the incoming creature and made it the melt
      // target. Advancing a second time here was the bug that skipped every
      // other entry: it moved the dex on before the just-entered creature got a
      // single frame of its own. Finish the transition on the entry already in
      // place instead.
      entryAt = millis();
      phase = P_HOLD;
    }
  }

  const uint32_t t0 = micros();
  drawScene();
  frameUsSum += micros() - t0;
  frameCount++;
}
