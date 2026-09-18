# Sleepy Star — build spec

A tilt-and-tap laser puzzle for the **Waveshare ESP32-S3-Touch-AMOLED-1.75** (466×466 round AMOLED).

This document is written to be handed to a coding agent. It contains the complete game model,
verified level data, exact screen geometry, and the algorithms. Where something must be confirmed
against the vendor's code rather than invented, it says so explicitly.

---

## 1. The game in three sentences

> **The laser always falls straight down.** Turn your wrist and the board turns under it, so the beam enters somewhere new.
> **Tap a ring to turn it** one notch.
> **Turning a ring turns the ring inside it the opposite way.**

You are trying to get the beam through the concentric rings to the sleeping star in the middle.
The star only accepts light through its **door**, which is fixed to the board and turns with it.

Spinning the watch is **free and unlimited**. Only taps are counted. Every level has a known
minimum tap count ("best"); beating or matching it is the score.

Why it works: spinning gives continuous, instant, zero-cost feedback — the thing reads as a toy
before it reads as a puzzle — while the ring coupling means you can never move just one thing.

---

## 2. Target hardware

Confirmed from the Waveshare wiki for **ESP32-S3-Touch-AMOLED-1.75**:

| Part | Chip | Bus |
|---|---|---|
| MCU | ESP32-S3R8 (Xtensa LX7 dual-core, 240 MHz) | — |
| RAM | 512 KB SRAM + **8 MB PSRAM**, 16 MB flash | — |
| Display | **CO5300**, 466×466, 16.7M colour AMOLED | QSPI |
| Touch | **CST9217** capacitive | I²C |
| IMU | **QMI8658** 6-axis (accel + gyro) | I²C |
| RTC | PCF85063 | I²C |
| PMIC | AXP2101 | I²C |
| Audio | ES8311 codec + speaker, ES7210 mic AEC | I²S |

Vendor toolchain: ESP-IDF v5.1.4, Arduino-ESP32 v3.1.0, LVGL v8.4.0.

> ⚠️ **Do not invent GPIO numbers.** Clone Waveshare's demo repo for this exact board and lift the
> pin map, the CO5300 init sequence, and the CST9217/QMI8658 I²C addresses from their working
> example. Several Waveshare boards share the "1.75 AMOLED" name (`1.75`, `1.75B`, `1.75C`) and
> the touch controller differs between them — some ship **FT3168** instead of CST9217. Detect at
> runtime by probing both I²C addresses and log which one answered.

### Framework choice

**Use ESP-IDF with a custom software renderer. Do not use LVGL for the dial.**
Everything on screen is rotated geometry with hard pixel edges; LVGL's widget model fights that and
its canvas rotation is slower than drawing the shapes directly. Use the vendor's `esp_lcd` CO5300
panel driver for the transport only.

---

## 3. Screen geometry (all numbers in device pixels, 466×466)

Centre is `(233, 233)`. Keep all UI inside radius **222** — the glass is round and the outer few
pixels are unreliable.

**Angles.** The board has **8 spokes**, numbered 0–7.

```
board_angle(s)  = s * 45° - 90°          // spoke 0 points to board "up"
screen_angle(s) = board_angle(s) + board_rotation
x = 233 + r * cos(screen_angle)
y = 233 + r * sin(screen_angle)          // y grows downward
```

**Ring layout** — depends only on how many rings the level has. Band thickness is **25 px**.

| rings | ring radii (outer → inner) | teeth per ring | star radius |
|---|---|---|---|
| 2 | 113, 71 | 24, 16 | 32 |
| 3 | 128, 92, 56 | 32, 24, 16 | 28 |
| 4 | 134, 103, 72, 41 | 32, 24, 16, 8 | 22 |

- **Hole (slot)**: 27×27 px square centred on the band at each of the 8 spoke positions, filled with
  the board background colour, then the element glyph drawn inside it. A `BLOCK` is **not** drawn at
  all — the band simply stays solid there. This is the single most important rendering decision:
  it makes "the rings are walls with holes in them" visible instead of something you have to be told.
- **Teeth**: 6×10 px marks at radius `ring_r + 15`, at every tooth index that is *not* a spoke
  position. Teeth exist purely so rotation is visible.
- **Laser origin radius**: 168. The first beam segment runs from r=168 inward to the outer ring.
- **Star door**: board spoke **4**, drawn as a 22×14 px bright notch on the star's edge.

**UI element positions** (polar from centre, r=182 unless noted):

| Element | Position | Size |
|---|---|---|
| Level label `LVL 7` | top-left, ~(76, 92) | 11 px text |
| Tap counter `2/3` | top-right, ~(390, 92) right-aligned | 11 px text |
| Ring buttons pod | 40° (lower-right) | 111×65, tangential |
| Reset button | 180° (left) | 48×46 |
| Progress pips | (233, 390) | 12 px squares |

Pods are rotated tangentially by `angle - 90°` so their corners stay inside radius 222.
Touch targets must be **≥ 44 px**; on a 44 mm round panel that is ~4.5 mm, which is the practical
floor. The primary ring interaction is tapping the ring itself, so the ring buttons are a
convenience, not the main path.

### Which thing rotates on screen

The board is **fixed to the device**; the **laser emitter orbits the rim** to whichever point is
currently world-up. This is the physically honest reading — the board lives inside the watch, and
gravity moves. (The design canvas draws the mathematically equivalent inverse view, board spinning
under a fixed top-mounted emitter. Same puzzle; pick the emitter-orbits version for hardware, it
matches what the hand is doing.)

---

## 4. Game model

```c
typedef enum { EL_BLOCK = 0, EL_GAP, EL_MIRROR_L, EL_MIRROR_R, EL_SPLIT } element_t;

#define SPOKES 8
#define MAX_RINGS 5
#define DOOR_SPOKE 4

typedef struct {
    uint8_t   n_rings;
    element_t ring[MAX_RINGS][SPOKES];   // ring[0] = OUTERMOST
    uint8_t   start[MAX_RINGS];          // opening offsets, 0..7
    uint8_t   best_taps;                 // verified minimum
} level_t;

typedef struct {
    uint8_t rot[MAX_RINGS];   // current offsets 0..7
    uint8_t entry;            // spoke the beam enters on, derived from gravity
    uint16_t taps;
} play_t;
```

### 4.1 Element lookup

A ring's offset shifts which element faces a given spoke:

```c
static inline uint8_t m8(int n) { return (uint8_t)(((n % 8) + 8) % 8); }

element_t element_at(const level_t *L, const play_t *P, int ring, int spoke) {
    return L->ring[ring][ m8(spoke - P->rot[ring]) ];
}
```

### 4.2 Beam trace

The beam always travels **inward**, one ring at a time. An element decides whether it continues and
on which spoke. A splitter produces two beams, so this is a small breadth-first walk, not a single
path.

```c
typedef struct { uint8_t ring, spoke; } beam_t;

// returns true if any branch reaches the star's door.
// out_segments/out_stops are filled for rendering; pass NULL to just test.
bool trace(const level_t *L, const play_t *P, trace_out_t *out)
{
    beam_t cur[8], nxt[8];
    int n_cur = 1, n_nxt;
    cur[0] = (beam_t){ .ring = 0, .spoke = P->entry };
    bool win = false;
    int guard = 0;

    while (n_cur > 0 && guard++ < 16) {
        n_nxt = 0;
        for (int i = 0; i < n_cur; i++) {
            beam_t b = cur[i];
            element_t el = element_at(L, P, b.ring, b.spoke);

            emit_segment(out, b.ring, b.spoke);          // for rendering

            uint8_t outs[2]; int n_outs = 0;
            switch (el) {
                case EL_BLOCK:    n_outs = 0; emit_stop(out, b.ring, b.spoke); break;
                case EL_GAP:      outs[n_outs++] = b.spoke;             break;
                case EL_MIRROR_R: outs[n_outs++] = m8(b.spoke + 1);     break;
                case EL_MIRROR_L: outs[n_outs++] = m8(b.spoke - 1);     break;
                case EL_SPLIT:    outs[n_outs++] = b.spoke;
                                  outs[n_outs++] = m8(b.spoke + 1);     break;
            }

            for (int k = 0; k < n_outs; k++) {
                if (b.ring == L->n_rings - 1) {
                    emit_core_segment(out, outs[k]);
                    if (outs[k] == DOOR_SPOKE) win = true;
                    else emit_stop_at_core(out, outs[k]);
                } else if (n_nxt < 8) {
                    nxt[n_nxt++] = (beam_t){ .ring = b.ring + 1, .spoke = outs[k] };
                }
            }
        }
        memcpy(cur, nxt, sizeof(beam_t) * n_nxt);
        n_cur = n_nxt;
    }
    return win;
}
```

**Geometry of a segment.** A beam leaving ring `i` at spoke `s` and meeting ring `i+1` at spoke `s'`
is drawn as a straight chord from `polar(radius[i], s)` to `polar(radius[i+1], s')`. When `s == s'`
that chord is radial; when a mirror bent it, it is a slanted chord. This is what produces the
folded-polyline look.

### 4.3 The tap rule

```c
void tap_ring(const level_t *L, play_t *P, int idx) {
    P->rot[idx] = m8(P->rot[idx] + 1);
    if (idx < L->n_rings - 1)
        P->rot[idx + 1] = m8(P->rot[idx + 1] - 1);   // the ring inside turns the other way
    P->taps++;
}
```

Taps are one-directional. That is deliberate: it keeps the rule to one sentence, and because the
group generated by these moves is the whole of `(Z/8)^n`, every configuration is still reachable.

**Proof it is always solvable.** Tapping ring `i` applies the vector `e_i − e_{i+1}`; tapping the
innermost applies `e_{n-1}`. From `e_{n-1}` you get `e_{n-2}` by adding `e_{n-2} − e_{n-1}`, and so on
by induction — the moves generate every basis vector, hence all `8^n` states. No level can be
bricked, and reset is only a convenience.

### 4.4 Win condition

`trace()` returns true. There is no fail state and no move limit — you cannot lose, only take longer.

---

## 5. Tilt → entry spoke (QMI8658)

This is the part most likely to feel bad if rushed. Budget real time for it.

```c
// 1. Read accelerometer. Project gravity into the screen plane.
//    The sign/axis mapping depends on how the IMU is mounted relative to the panel —
//    DETERMINE IT EXPERIMENTALLY. Hold the board with its USB port down, print
//    atan2(ay, ax), and pick the transform that makes "up on screen" read as up.
float g_mag = sqrtf(ax*ax + ay*ay);

// 2. Reject the flat case. Lying on a table there is no in-plane gravity to read.
if (g_mag < 0.25f) { /* hold last good angle; after 1s show a "tilt me" nudge */ }
else {
    float up_angle = atan2f(-ay, -ax);            // screen-space direction of world-UP
    // 3. Low-pass. alpha ~0.15 at 50 Hz is a good starting point.
    filtered = filtered + alpha * wrap_pi(up_angle - filtered);
}

// 4. Snap to one of 8 spokes, WITH HYSTERESIS so it does not chatter on a boundary.
//    Only leave the current sector once you are 6° past its edge.
int target = lroundf((degrees(filtered) + 90.0f) / 45.0f);
if (angular_distance(filtered, sector_centre(current)) > (22.5f + 6.0f))
    current = m8(target);
P->entry = m8(current);
```

Tuning notes:
- Sample the IMU at 50–100 Hz; render only when `P->entry` actually changes.
- 6° of hysteresis and `alpha = 0.15` is the starting point, not the answer. Sit with it on the wrist.
- Optional: when `g_mag < 0.25`, integrate gyro Z to allow flat-on-table play. Nice-to-have, and it
  drifts — gate it behind a setting rather than making it the default.

---

## 6. Touch input (CST9217)

- **Ring tap** is the primary interaction. Convert the touch point to a radius from centre:
  `r = hypot(tx - 233, ty - 233)`. If `|r - ring_radius[i]| <= 16` for some ring `i`, that is a tap
  on ring `i`. The target is a full annulus — enormous and forgiving, which is the whole reason this
  control scheme suits a round watch.
- Reject touches with `r < star_radius + 6` (the star is not a button) or `r > outer_ring + 20`
  (that is the bezel).
- Check the rectangular button hit-boxes (reset, ring buttons, level arrows) **before** the ring test.
- Debounce: ignore a second touch within 120 ms; require a clean press-then-release, and cancel if
  the finger travels more than 20 px.

---

## 7. Verified campaign levels

These six are **machine-verified**: solvable, with `best_taps` equal to the true breadth-first
minimum, and each opens in a state where no wrist angle wins. Use them as the tutorial spine.

Element codes: `.` BLOCK, `O` GAP, `\` MIRROR_L, `/` MIRROR_R, `Y` SPLIT.
Arrays run spoke 0 → 7. `rings[0]` is the outermost ring.

```c
// LVL 1 — 2 rings, 1 tap. Gaps only. The player solves this by accident.
{ .n_rings=2, .best_taps=1, .start={3,2}, .ring={
    { O,.,O,.,.,.,O,. },
    { .,.,.,O,O,.,.,O } } },

// LVL 2 — 2 rings, 2 taps. Fewer gaps; the coupling becomes noticeable.
{ .n_rings=2, .best_taps=2, .start={7,6}, .ring={
    { .,.,.,.,.,O,.,O },
    { .,.,.,O,O,.,.,. } } },

// LVL 4 — 2 rings, 3 taps. Mirrors arrive.
{ .n_rings=2, .best_taps=3, .start={5,0}, .ring={
    { .,.,.,.,O,\,\,. },
    { .,.,.,.,O,O,\,. } } },

// LVL 7 — 3 rings, 3 taps. A third ring; the coupling now chains.
{ .n_rings=3, .best_taps=3, .start={3,4,2}, .ring={
    { .,.,O,\,.,.,.,O },
    { .,\,O,.,.,.,.,/ },
    { .,\,/,.,/,.,.,. } } },

// LVL 11 — 3 rings, 4 taps. Splitters: one beam becomes a branching tree.
{ .n_rings=3, .best_taps=4, .start={4,3,0}, .ring={
    { .,.,O,\,.,O,.,. },
    { O,.,.,.,.,\,/,. },
    { /,.,.,\,Y,.,.,\ } } },

// LVL 16 — 4 rings, 5 taps. The full board.
{ .n_rings=4, .best_taps=5, .start={0,6,1,6}, .ring={
    { \,.,.,.,O,.,\,O },
    { .,/,.,.,.,/,.,\ },
    { \,.,O,/,.,/,.,. },
    { .,O,.,.,.,/,/,. } } },
```

Difficulty is tuned by **rarity of the winning line**, not by adding rules. Fraction of all ring
configurations that some wrist angle can open:

| level | rings | taps | openable configurations |
|---|---|---|---|
| 1 | 2 | 1 | 9 / 64 — one in seven |
| 2 | 2 | 2 | 4 / 64 |
| 4 | 2 | 3 | 5 / 64 |
| 7 | 3 | 3 | 18 / 512 |
| 11 | 3 | 4 | 30 / 512 |
| 16 | 4 | 5 | 96 / 4096 — one in forty-three |

Fill the gaps (3, 5, 6, 8–10, 12–15) with generated levels at interpolated targets.

---

## 8. Level generator (runs on-device — this is how endless mode works)

The whole state space is at most `8^4 = 4096` for four rings, so generation and verification are
cheap enough to do on the ESP32 in a few milliseconds. No pre-baked level packs needed.

```
generate(n_rings, target_taps, max_rarity):
  loop:
    1. Fill each ring with random elements from the tier's allowed set.
       Constrain: 3..4 non-BLOCK cells per ring, at most one SPLIT per ring.
    2. rarity = (# of the 8^n configurations for which ANY of the 8 entry
                 spokes wins) / 8^n
       Reject if 0 (impossible) or > max_rarity (too easy).
    3. Pick a random start offset vector. Reject if it already wins at any angle.
    4. Breadth-first over tap moves from that start until the first state that
       wins at some angle. That depth is best_taps.
       Reject if best_taps != target_taps.
    5. Accept. Store rings + start + best_taps.
```

Tier schedule (ring count, allowed elements, target taps, rarity ceiling):

| levels | rings | elements | taps | max rarity |
|---|---|---|---|---|
| 1–3 | 2 | GAP | 1–2 | 0.30 |
| 4–6 | 2 | GAP, MIRROR | 3 | 0.16 |
| 7–10 | 3 | GAP, MIRROR | 3–4 | 0.10 |
| 11–15 | 3 | + SPLIT | 4 | 0.06 |
| 16–30 | 4 | + SPLIT | 5–6 | 0.04 |
| 31+ | 4–5 | all | 6–9 | 0.02 |

Seed the RNG from the level number so a given level is **the same for everybody** — that makes
"I'm on 34" a comparable statement, which is the whole social hook.

---

## 9. Rendering

### Palette (RGB565)

AMOLED: black pixels draw almost no current, so the dark ground is a battery decision as much as an
aesthetic one. Keep large areas near-black.

| Name | Hex | Use |
|---|---|---|
| VOID | `#0E0E1C` | screen background |
| DISC | `#16162A` | inside the bezel, and the inside of every hole |
| PLATE 1–4 | `#2E3550` `#343C5C` `#3A4368` `#404A74` | ring bands, outer → inner |
| TOOTH 1–4 | `#414B70` `#48537C` `#4F5B88` `#566394` | tooth marks |
| GAP EDGE | `#4A5580` | the lip drawn top and bottom inside a gap |
| MIRROR | `#DCEBFF` | mirror glyphs |
| SPLIT | `#7FE8FF` | splitter glyphs, tilt UI |
| BEAM | `#FF4DD2` | the laser (white 1 px core down the middle) |
| STAR ASLEEP | `#3C4266` | star body, unlit |
| STAR AWAKE | `#FFD93D` | star body, lit, and all win-state chrome |
| TEXT | `#EAF0FF` | primary text |
| TEXT DIM | `#8A97C0` | secondary text |

Glyphs are drawn as 5×5 grids of 4 px blocks (so 20×20 px), hard-edged, no anti-aliasing:

```
MIRROR_L  \ : (0,0)(1,1)(2,2)(3,3)(4,4)
MIRROR_R  / : (0,4)(1,3)(2,2)(3,1)(4,0)
SPLIT     Y : (0,4)(1,3)(3,1)(4,0)          // broken diagonal = half-silvered
GAP         : no glyph — two 3 px lips at the top and bottom edge of the hole
BLOCK       : nothing. The band is not cut.
```

Each glyph is rotated to its spoke angle before drawing, so a mirror always presents the same face
to an inbound radial beam.

### Draw order

1. Background: VOID fill, bezel ring, DISC circle. **Pre-render once** into a static buffer.
2. For each ring, outer → inner: the band annulus, then teeth, then holes (fill with DISC, then glyph).
3. Star: halo (win only), body octagon, door notch, face.
4. Beam: for each segment, a wide low-alpha quad, then a 5 px quad, then a 1 px white core line.
5. Stop markers where a branch hit a BLOCK.
6. UI text, buttons, pips.
7. Emitter marker at the current world-up rim position.

### Framebuffer strategy

- One 466×466 RGB565 framebuffer in PSRAM (434 KB) plus the static background layer (another 434 KB).
  8 MB PSRAM makes this a non-issue.
- **The game is turn-based: do not run a render loop.** Redraw only when state changes, and animate
  only during the ~260 ms ring tween. Idle cost should be zero.
- On redraw, `memcpy` the background layer over only the **dial bounding square** (~300×300 = 180 KB),
  redraw the dial into it, and push just that rect with `esp_lcd_panel_draw_bitmap`. Budget ~6 ms per
  frame; 30 fps during tweens is plenty.
- Primitives needed: scanline-filled annulus, scanline-filled rotated quad (for teeth, glyph blocks
  and beam segments), and a bitmap font blit. No anti-aliasing — the crisp edges *are* the art style.

### Animation

| Event | Treatment |
|---|---|
| Ring tap | Both affected rings tween 45° over 260 ms, stepped in 3–4 discrete jumps (mechanical, not smooth) |
| Tilt change | Emitter jumps to the new spoke; beam re-traces immediately, no tween |
| Solve | Star swaps to the awake face, 8 rays pop out, halo fades in, banner scales up over 420 ms |
| Blocked branch | Stop marker with a 1-frame flash |

---

## 10. Audio (ES8311 + onboard speaker)

Small, dry, and skippable — a mute toggle in settings.

- Ring tap: 40 ms click, ~800 Hz square, short decay.
- Tilt detent: quieter 25 ms tick at ~1.2 kHz, only on sector change.
- Beam reaches one ring deeper than before: soft rising blip. (This is the "getting warmer" cue —
  it does a lot of work.)
- Solve: 4-note rising arpeggio, ~600 ms.
- Generate as square waves at runtime; no audio assets needed.

---

## 11. Persistence (NVS)

Namespace `sleepystar`:

| Key | Type | Meaning |
|---|---|---|
| `depth` | u16 | highest level reached — the headline number |
| `best_N` | u8 | taps used on level N, for the star rating |
| `mute` | u8 | audio off |
| `seen_tut` | u8 | tutorial card dismissed |

Star rating: 3 stars at `best_taps`, 2 at `best_taps + 2`, 1 for any solve. Stars never gate
progress — they exist only for the people who want them.

---

## 12. Suggested structure

```
main/
  main.c              app entry, task setup
  game/
    model.h/.c        level_t, play_t, element_at, tap_ring
    trace.h/.c        beam_t walk, segment output
    generate.h/.c     level generator + BFS solver
    levels.c          the six verified campaign levels
  hw/
    display.c         CO5300 QSPI init + panel handle (from vendor demo)
    touch.c           CST9217/FT3168 probe and read
    imu.c             QMI8658 read, filter, hysteresis → entry spoke
    audio.c           ES8311 tone synth
    store.c           NVS
  render/
    raster.h/.c       annulus, rotated quad, line, blit primitives
    glyphs.c          element glyph block tables
    scene.c           draw order, dirty rect, tween state
    font.c            bitmap font (8×8 and 5×7)
```

Tasks: one game/render task pinned to core 1; IMU + touch polling on core 0, posting events to a
queue. Do not touch the framebuffer from two tasks.

---

## 13. Build order

1. **Bring-up.** Vendor demo running unmodified. Confirm display, touch coordinates, IMU axes. Log
   raw accel while rotating the board and write down the transform that makes world-up read correctly.
2. **Raster primitives.** Annulus, rotated quad, line, font blit. Test card artboard.
3. **Static board.** Draw level 1 at a fixed rotation, no input. Verify the holes read as holes.
4. **Beam trace.** Port `trace()`, render segments. Fake the entry spoke with a button.
5. **Tilt.** Wire the IMU to the entry spoke. **Spend real time on the feel here** — this is the
   whole game. It should be impossible to make it chatter on a boundary.
6. **Tap + coupling.** Ring hit test, tap rule, tween.
7. **Win state.** Star wakes, banner, audio, NVS write.
8. **Campaign.** Six levels, level select, tutorial card.
9. **Generator.** Endless mode past level 16.
10. **Polish.** Battery, sleep on wrist-down, mute, star ratings.

---

## 14. Acceptance tests

- `trace()` on each of the six levels at its `start` returns false for **all 8** entry spokes.
- Breadth-first from each `start` reaches a winning state in exactly `best_taps` taps.
- Tapping the innermost ring changes exactly one offset; tapping any other changes exactly two,
  in opposite directions.
- From any reachable state, repeated tapping can restore the start state (reachability of `(Z/8)^n`).
- Rotating the device through a full turn changes the entry spoke exactly 8 times, with no
  double-triggering when held near a boundary and deliberately jiggled.
- Ring tap registers anywhere on the annulus, including directly on a glyph.
- With the board flat on a table, the entry spoke holds steady and the nudge appears.

---

## 15. Open questions for whoever builds this

1. **Board variant.** Confirm `1.75` vs `1.75B`/`1.75C` — touch controller and pin map differ.
2. **Emitter convention.** Spec says board fixed, emitter orbits. Build both behind a compile flag
   and try them on the wrist for ten minutes each; the inverse (board counter-rotates, emitter fixed
   at top) is more dramatic and may win.
3. **Tap direction.** One-directional taps keep the rule to one sentence. If playtesting shows people
   repeatedly tapping 7 times to undo one, add "tap the outer half clockwise, inner half
   anticlockwise" — but only if it actually comes up, because it doubles the rule.
4. **Wake-on-wrist.** The AXP2101 and the IMU can support a raise-to-wake gesture. Worth it for a
   thing you glance at, but out of scope for v1.
5. **Level 1 tutorial.** Currently a dismissible card. Consider instead making level 1 unloseable and
   silent — the board reacting to the wrist may teach rule 1 faster than any sentence.
