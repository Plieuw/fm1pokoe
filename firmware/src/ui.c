/* SPDX-License-Identifier: GPL-3.0-only */
/* CHUA UI (main loop only): the attractor drawn live, the knobs, presets, LEDs, the settings in flash.
 *
 * Four pages: HOME the circuit (the oscillator), ENV its page (again: back HOME), FX the resonator, FX again the
 * shimmer, FX again back HOME; the LED of the page's button is lit:
 *   HOME     ALGORITHM the oscillator: CHUA (the circuit) or one of the 30 noise programs (noise.c: A0..C9)
 *   CIRCUIT  KNOB 1 CHAOS   the regime: cycle, period 2, spiral, double scroll, outer cycle
 *            KNOB 2 SHAPE   beta: slow round spirals .. tight fast ones
 *            KNOB 3 OUTPUT  which voltage you hear: X, X+Y, Y, Y+Z, Z
 *            KNOB 4 RING    the low pass gate's resonance
 *   NOISE    KNOB 1 X, KNOB 2 Y (the program's two controls), KNOB 3 LEVEL, KNOB 4 RING; drawn: the output against
 *            itself a moment ago (its "attractor")
 *   ENV      KNOB 1..4      ATTACK, DECAY, SUSTAIN, RELEASE of the ADSR that drives the low pass gate; the envelope drawn
 *   FX       KNOB 1..4      STRUCTURE, BRIGHTNESS, DAMPING, MIX of the resonator (resonator.c: Rings' models);
 *            ALGORITHM the model (MODAL, SYMP STRING, STRING), SELECT the POSITION; its resonances and output drawn
 *   FX (2nd) KNOB 1..4      SIZE, DECAY, SHIMMER, MIX of the shimmer reverb (shimmer.c); ALGORITHM the INTERVAL,
 *            SELECT the TONE; the tail's level over the last seconds drawn (the riser building)
 *   SELECT   HOME: the view (3D turning, X-Y, X-Z, Y-Z, TIME); ENV: RING
 *   PRESETS the presets
 *   OCT- / OCT+  octave -3..+3               SAVE       the settings into flash (restored at power-on)
 *   GLO          2ND CORE on / off (saved at once; from the next power-on: the core starts only at boot)
 *   MASTER pot   volume
 *
 * The picture: synth.c keeps a ring of the circuit's state (chua_trace: the newest voice, or the ghost circuit when
 * nothing plays); each frame draws its last TRACE_DRAW points as a line, the oldest faint, the newest bright, scaled
 * to fit (each axis follows its own range). Drawing: the canvas (gfx.c cv_*) is 240 x 124 at most, blitted by DMA
 * while the next frame is prepared. */
#define UI_VERSION FELUCCA_VERSION
#define TRACE_DRAW 640u                  /* points drawn per frame (of chua_trace's TRACE_N) */
#define PLOT_Y 24u                       /* the plot: 240 x 124 from here */
#define PLOT_H 124
#define UI_PALETTE 2u                    /* AMBER (build/gen/ui_palettes.h): an oscilloscope's colours */

static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
enum { V_3D, V_XY, V_XZ, V_YZ, V_TIME, V_COUNT };
static const char *const VIEW_NAME[V_COUNT] = {"3D", "X-Y", "X-Z", "Y-Z", "TIME"};

/* ------------------------------------------------------------- presets --- */
/* ADSR times: 0 = 0.5 ms .. 127 = 10 s (exponential: 40 ~11 ms, 64 ~74 ms, 80 ~250 ms, 100 ~1.2 s); RING: 0 none .. 127 Q 6 */
/* FX (resonator.c): model, STRUCTURE, BRIGHTNESS, DAMPING, POSITION, MIX; MIX 0 (left out): no resonator */
typedef struct {
    const char *name;
    uint8_t chaos, shape, output, attack, decay, sustain, release, ring;
    uint8_t fx_model, fx_struct, fx_bright, fx_damp, fx_pos, fx_mix;
    uint8_t sh_size, sh_decay, sh_shimmer, sh_mix, sh_interval, sh_tone;   /* SHIMMER; MIX 0 (left out): off */
    uint8_t osc, np_x, np_y;                     /* the oscillator: 0 (left out) the circuit, 1..30 a noise program */
} preset_t;
#define NZ(a, b) (1 + (a) * 10 + (b))            /* noise program A0 = NZ(0, 0) .. C9 = NZ(2, 9) */
#define NOFX 0, 0, 0, 0, 0, 0
#define NOSH 0, 0, 0, 0, 0, 0
static const preset_t PRESETS[] = {
    {"DOUBLE SCROLL", 85, 76, 0, 6, 80, 110, 70, 50},     /* the classic double scroll, x (the scroll jumps) */
    {"RING PLUCK", 85, 90, 64, 0, 62, 0, 66, 110},        /* the gate pinged: a short ring on the spiral */
    {"PURE CYCLE", 6, 64, 64, 10, 70, 120, 64, 30},       /* just past the Hopf point: a clean periodic orbit, y */
    {"PERIOD TWO", 38, 64, 32, 4, 60, 90, 66, 60},       /* the first period doubling: a sub-octave appears */
    {"SPIRAL", 54, 56, 64, 2, 64, 80, 72, 70},           /* Roessler-like chaos on one scroll: pitched, rough */
    {"BONGO GATE", 30, 100, 32, 0, 50, 0, 52, 100},       /* a short gate on a clean cycle: a wooden ring */
    {"CRISIS EDGE", 110, 64, 96, 2, 76, 100, 74, 60},    /* the double scroll about to break out */
    {"OUTER LOOP", 124, 40, 64, 0, 56, 60, 60, 40},       /* the saturated circuit's big cycle: loud, buzzy */
    {"SLOW BLOOM", 70, 110, 127, 92, 96, 110, 100, 70},  /* z, fast spirals, slow envelope, ringing gate: a pad */
    {"GRIT BASS", 96, 16, 0, 0, 54, 70, 50, 40},          /* x at low beta: slow, deep switching */
    {"RINGS BELL", 54, 90, 64, 0, 60, 0, 70, 40, RZ_MODAL, 70, 100, 110, 12, 100},   /* the spiral struck into a bell */
    {"SITAR DRONE", 85, 64, 32, 20, 80, 110, 90, 30, RZ_SYMP, 16, 80, 105, 70, 90},  /* sympathetic strings, 5TH */
    {"CHAOS PIANO", 30, 76, 64, 0, 64, 40, 74, 50, RZ_STRING, 100, 70, 96, 20, 85},  /* a stiff string, dispersion */
    {"BURIED BAR", 110, 40, 0, 4, 76, 90, 80, 60, RZ_MODAL, 116, 40, 80, 24, 70},     /* a tuned bar under the crisis */
    {"HEAVEN RISER", 6, 64, 64, 30, 80, 100, 96, 30, 0, 0, 0, 0, 0, 0,              /* a pure cycle that climbs */
     110, 110, 100, 96, SI_OCT, 100},
    {"OCTAVE CLOUD", 85, 76, 32, 60, 90, 110, 100, 50, 0, 0, 0, 0, 0, 0,            /* the double scroll in a slow */
     127, 120, 50, 80, SI_OCT, 80},                                                  /* octave haze */
    {"FIFTH ASCENT", 54, 90, 64, 0, 62, 0, 70, 40, RZ_MODAL, 70, 100, 110, 12, 70,  /* struck bells rising in */
     90, 110, 120, 100, SI_FIFTH, 110},                                              /* fifths */
    /* the noise programs (noise.c): the gate as on the circuit, the CHAOS / SHAPE / OUTPUT kept for going back */
    {"RADIO OH NO", 85, 76, 0, 0, 70, 100, 70, 40, NOFX, NOSH, NZ(0, 0), 40, 90},          /* a broken radio */
    {"GRAIN SHOWER", 85, 76, 0, 20, 80, 90, 96, 30, NOFX,                                 /* glitch grains */
     100, 100, 60, 70, SI_OCT, 90, NZ(0, 6), 70, 50},                                    /* rising in octaves */
    {"PRIME HATS", 85, 76, 0, 0, 40, 0, 44, 70, NOFX, NOSH, NZ(1, 5), 96, 64},           /* metal clusters: hats */
    {"FIBONACCI PAD", 85, 76, 0, 80, 96, 110, 100, 50, NOFX,                             /* a stretched cluster */
     110, 110, 90, 80, SI_FIFTH, 100, NZ(1, 7), 50, 40},
    {"CLUSTER BELL", 85, 76, 0, 0, 62, 0, 70, 40, RZ_MODAL, 70, 100, 110, 12, 80,        /* a saw cluster struck */
     NOSH, NZ(1, 0), 30, 80},                                                              /* into the bell */
    {"SATAN WORKOUT", 85, 76, 0, 2, 76, 100, 74, 60, NOFX, NOSH, NZ(2, 7), 80, 80},       /* harsh and wild */
    {"BITCRUSH WALK", 85, 76, 0, 0, 64, 60, 60, 50, NOFX, NOSH, NZ(2, 8), 64, 100},       /* a crushed random walk */
};
#define NPRESETS (sizeof PRESETS / sizeof PRESETS[0])

static void preset_load(uint32_t i)
{
    const preset_t *p = &PRESETS[i % NPRESETS];
    sys.preset = (uint8_t)(i % NPRESETS);
    sys.chaos = p->chaos;
    sys.shape = p->shape;
    sys.output = p->output;
    sys.attack = p->attack;
    sys.decay = p->decay;
    sys.sustain = p->sustain;
    sys.release = p->release;
    sys.ring = p->ring;
    if (p->fx_mix) {                             /* (a preset without FX leaves the resonator's knobs, MIX off) */
        sys.fx_model = p->fx_model;
        sys.fx_struct = p->fx_struct;
        sys.fx_bright = p->fx_bright;
        sys.fx_damp = p->fx_damp;
        sys.fx_pos = p->fx_pos;
    }
    sys.fx_mix = p->fx_mix;
    if (p->sh_mix) {                             /* (as the resonator: a preset without the shimmer turns it off) */
        sys.sh_size = p->sh_size;
        sys.sh_decay = p->sh_decay;
        sys.sh_shimmer = p->sh_shimmer;
        sys.sh_interval = p->sh_interval;
        sys.sh_tone = p->sh_tone;
    }
    sys.sh_mix = p->sh_mix;
    sys.osc = (uint8_t)(p->osc % (NP_COUNT + 1u));
    if (p->osc) {                                /* (a circuit preset leaves the noise knobs) */
        sys.np_x = p->np_x;
        sys.np_y = p->np_y;
        sys.np_level = 64;
    }
}
static int preset_edited(void)
{
    const preset_t *p = &PRESETS[sys.preset % NPRESETS];
    return sys.chaos != p->chaos || sys.shape != p->shape || sys.output != p->output || sys.attack != p->attack ||
           sys.decay != p->decay || sys.sustain != p->sustain || sys.release != p->release || sys.ring != p->ring ||
           sys.fx_mix != p->fx_mix || (p->fx_mix && (sys.fx_model != p->fx_model || sys.fx_struct != p->fx_struct ||
           sys.fx_bright != p->fx_bright || sys.fx_damp != p->fx_damp || sys.fx_pos != p->fx_pos)) ||
           sys.sh_mix != p->sh_mix || (p->sh_mix && (sys.sh_size != p->sh_size || sys.sh_decay != p->sh_decay ||
           sys.sh_shimmer != p->sh_shimmer || sys.sh_interval != p->sh_interval || sys.sh_tone != p->sh_tone)) ||
           sys.osc != p->osc || (p->osc && (sys.np_x != p->np_x || sys.np_y != p->np_y || sys.np_level != 64u));
}

/* ------------------------------------------------------------ settings --- */
#define SET_MAGIC 0x43485536u      /* "CHU6": bump when the layout changes (older records are then ignored) */
typedef struct {
    uint32_t magic;
    int8_t octave;
    uint8_t chaos, shape, output, attack, release, view, preset, cpu2, decay, sustain, ring;
    uint8_t fx_model, fx_struct, fx_bright, fx_damp, fx_pos, fx_mix;
    uint8_t sh_size, sh_decay, sh_shimmer, sh_mix, sh_interval, sh_tone;
    uint8_t osc, np_x, np_y, np_level;
} settings_t;

static void settings_boot(void)    /* power-on, before the audio */
{
    settings_t s;
    uint32_t f = irq_save();
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;
    irq_restore(f);
    if (!flash_ok)
        return;
    if (st_load(OBJ_SETTINGS, &s, sizeof s) == (int)sizeof s && s.magic == SET_MAGIC) {
        sys.octave = (int8_t)clamp(s.octave, OCT_MIN, OCT_MAX);
        sys.chaos = (uint8_t)(s.chaos & 127u);
        sys.shape = (uint8_t)(s.shape & 127u);
        sys.output = (uint8_t)(s.output & 127u);
        sys.attack = (uint8_t)(s.attack & 127u);
        sys.release = (uint8_t)(s.release & 127u);
        sys.decay = (uint8_t)(s.decay & 127u);
        sys.sustain = (uint8_t)(s.sustain & 127u);
        sys.ring = (uint8_t)(s.ring & 127u);
        sys.view = (uint8_t)(s.view % V_COUNT);
        sys.preset = (uint8_t)(s.preset % NPRESETS);
        sys.cpu2 = (uint8_t)(s.cpu2 != 0);
        sys.fx_model = (uint8_t)(s.fx_model % RZ_COUNT);
        sys.fx_struct = (uint8_t)(s.fx_struct & 127u);
        sys.fx_bright = (uint8_t)(s.fx_bright & 127u);
        sys.fx_damp = (uint8_t)(s.fx_damp & 127u);
        sys.fx_pos = (uint8_t)(s.fx_pos & 127u);
        sys.fx_mix = (uint8_t)(s.fx_mix & 127u);
        sys.sh_size = (uint8_t)(s.sh_size & 127u);
        sys.sh_decay = (uint8_t)(s.sh_decay & 127u);
        sys.sh_shimmer = (uint8_t)(s.sh_shimmer & 127u);
        sys.sh_mix = (uint8_t)(s.sh_mix & 127u);
        sys.sh_interval = (uint8_t)(s.sh_interval % SI_COUNT);
        sys.sh_tone = (uint8_t)(s.sh_tone & 127u);
        sys.osc = (uint8_t)(s.osc % (NP_COUNT + 1u));
        sys.np_x = (uint8_t)(s.np_x & 127u);
        sys.np_y = (uint8_t)(s.np_y & 127u);
        sys.np_level = (uint8_t)(s.np_level & 127u);
    }
}

static int settings_save(void)     /* 0 = saved */
{
    settings_t s;
    if (!flash_ok)
        return -1;
    memset(&s, 0, sizeof s);
    s.magic = SET_MAGIC;
    s.octave = sys.octave;
    s.chaos = sys.chaos;
    s.shape = sys.shape;
    s.output = sys.output;
    s.attack = sys.attack;
    s.release = sys.release;
    s.decay = sys.decay;
    s.sustain = sys.sustain;
    s.ring = sys.ring;
    s.view = sys.view;
    s.preset = sys.preset;
    s.cpu2 = sys.cpu2;
    s.fx_model = sys.fx_model;
    s.fx_struct = sys.fx_struct;
    s.fx_bright = sys.fx_bright;
    s.fx_damp = sys.fx_damp;
    s.fx_pos = sys.fx_pos;
    s.fx_mix = sys.fx_mix;
    s.sh_size = sys.sh_size;
    s.sh_decay = sys.sh_decay;
    s.sh_shimmer = sys.sh_shimmer;
    s.sh_mix = sys.sh_mix;
    s.sh_interval = sys.sh_interval;
    s.sh_tone = sys.sh_tone;
    s.osc = sys.osc;
    s.np_x = sys.np_x;
    s.np_y = sys.np_y;
    s.np_level = sys.np_level;
    return st_save(OBJ_SETTINGS, &s, sizeof s);
}

/* -------------------------------------------------------------- text --- */
enum { T_HEAD, T_C1, T_C2, T_C3, T_C4, T_STATUS, NTEXT };
static char ui_shown[NTEXT][48];
static uint8_t ui_force = 1;       /* redraw everything (after the splash, the update screen) */
static char ui_event[24];
static uint32_t ui_event_ms, ui_hot_ms;
static uint8_t ui_hot = 0xFF;      /* the cell whose knob turned last (its value in the accent colour, 1 s) */

static void ui_say(const char *s)                    /* the status line, for 2 s */
{
    str_cpy(ui_event, s, sizeof ui_event);
    ui_event_ms = fm1_ms | 1u;
}

static void ui_cat(char *d, uint32_t n, const char *s)
{
    uint32_t i = str_len(d);
    while (*s && i + 1u < n)
        d[i++] = *s++;
    d[i] = 0;
}

static int ui_changed(uint32_t i, const char *a, const char *b)   /* the text of slot i is new (and kept) */
{
    char sig[48];
    str_cpy(sig, a, sizeof sig);
    ui_cat(sig, sizeof sig, "|");
    ui_cat(sig, sizeof sig, b);
    if (!ui_force && str_eq(ui_shown[i], sig))
        return 0;
    str_cpy(ui_shown[i], sig, sizeof ui_shown[i]);
    return 1;
}

static void ui_head(const char *name, const char *note)
{
    if (!ui_changed(T_HEAD, name, note))
        return;
    cv_begin(240, AF_M.h, T_BG);
    cv_text(8, 0, &AF_M, name, T_TEXT);
    cv_text_r(232, 0, &AF_M, note, T_THEME, T_BG);
    cv_blit(0, 2);
    lcd_sync();
}

/* a knob's cell: its label small, its value under it */
static void ui_cell(uint32_t i, const char *label, const char *value)
{
    static const uint8_t X[4] = {8, 124, 8, 124}, Y[4] = {152, 152, 186, 186};
    uint32_t c = i - T_C1;
    char hot[2] = {(char)('0' + (ui_hot == c && fm1_ms - ui_hot_ms < 1000u)), 0};
    char v[24];
    str_cpy(v, value, sizeof v);
    ui_cat(v, sizeof v, hot);                               /* (the highlight is part of what is shown) */
    if (!ui_changed(i, label, v))
        return;
    cv_begin(112, AF_S.h + AF_M.h, T_BG);
    cv_text(0, 0, &AF_S, label, T_MID);
    cv_text(0, AF_S.h, &AF_M, value, hot[0] == '1' ? T_ACCENT : T_TEXT);
    cv_blit(X[c], Y[c]);
    lcd_sync();
}

static void ui_status(const char *s)
{
    if (!ui_changed(T_STATUS, s, ""))
        return;
    draw_text_line(0, 223, 240, &AF_S, s, T_MID, T_BG, 1);
}

/* -------------------------------------------------------------- plot --- */
static int32_t sc_c[3], sc_r[3] = {2 << 8, 1 << 7, 3 << 8};   /* each axis: its centre and half range, Q8 (smoothed) */

/* the axes' ranges over the points drawn: the centre follows, the range grows at once and shrinks slowly */
static void plot_scale(const int16_t *const *a, uint32_t w, uint32_t n)
{
    uint32_t k, i;
    for (k = 0; k < 3u; k++) {
        int32_t lo = 32767, hi = -32768, c, r;
        for (i = w - n; i != w; i++) {
            int32_t v = a[k][i & (TRACE_N - 1u)];
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
        }
        c = (lo + hi) / 2;
        r = (hi - lo) / 2 + 8;
        sc_c[k] += (c - sc_c[k]) / 4;
        sc_r[k] = r > sc_r[k] ? r : sc_r[k] - (sc_r[k] - r) / 6;
    }
}
static int32_t plot_norm(int32_t v, uint32_t k)             /* -> -4096..4096 */
{
    return clamp((v - sc_c[k]) * 4096 / sc_r[k], -4096, 4096);
}

static int16_t plot_sin(uint32_t a)                          /* a: 256 per turn, Q15 */
{
    uint32_t q = a & 255u;
    static const int16_t S[65] = {0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278,
        11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403,
        22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268,
        29621, 29956, 30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678,
        32728, 32757, 32767};
    int16_t s = S[q & 64u ? 64u - (q & 63u) : q & 63u];
    return (int16_t)(q & 128u ? -s : s);
}

static void draw_plot(void)
{
    static const int16_t *const AX[3] = {chua_trace.x, chua_trace.y, chua_trace.z};
    uint32_t w = chua_trace.w, n = w < TRACE_DRAW ? w : TRACE_DRAW, i, view = sys.view % V_COUNT;
    int32_t px = 0, py = 0, cx = 120, cy = PLOT_H / 2, sx = 112, sy = PLOT_H / 2 - 6;
    uint16_t band[4];
    int32_t ca = 0, sa = 0;
    if (n < 2u)
        return;
    band[0] = ux_mix(T_BG, T_THEME, 22);
    band[1] = ux_mix(T_BG, T_THEME, 45);
    band[2] = ux_mix(T_BG, T_THEME, 72);
    band[3] = T_THEME;
    plot_scale(AX, w, n);
    cv_begin(240, PLOT_H, T_BG);
    cv_rect(cx - sx, cy, 2 * sx, 1, T_LINE);                 /* faint axes through the centre */
    cv_rect(cx, cy - sy, 1, 2 * sy, T_LINE);
    if (view == V_3D) {
        uint32_t ang = (fm1_ms / 47u) & 255u;               /* one turn in ~12 s */
        ca = plot_sin(ang + 64u);
        sa = plot_sin(ang);
    }
    if (view == V_TIME) {                                    /* x (bright) and y (faint) against time */
        uint32_t m = n < 240u ? n : 240u, k;
        for (k = 0; k < 2u; k++) {
            uint32_t ax = k ? 1u : 0u;
            for (i = 0; i < m; i++) {
                int32_t v = AX[ax][(w - m + i) & (TRACE_N - 1u)];
                int32_t x = (int32_t)i, y = cy - plot_norm(v, ax) * sy / 4096;
                if (i)
                    cv_line(px, py, x, y, k ? band[1] : band[3]);
                px = x;
                py = y;
            }
        }
    } else {
        for (i = 0; i < n; i++) {
            uint32_t at = (w - n + i) & (TRACE_N - 1u);
            int32_t x = plot_norm(chua_trace.x[at], 0), y = plot_norm(chua_trace.y[at], 1), z = plot_norm(chua_trace.z[at], 2);
            int32_t u, v;
            switch (view) {
            case V_3D: {                                     /* turning about the vertical, tilted ~25 degrees */
                int32_t d = (-x * sa + z * ca) >> 15;
                u = (x * ca + z * sa) >> 15;
                v = (y * 29696 + d * 13848) >> 15;
                break;
            }
            case V_XY: u = x; v = y; break;
            case V_XZ: u = x; v = z; break;
            default:   u = y; v = z; break;
            }
            u = cx + u * sx / 4096;
            v = cy - v * sy / 4096;
            if (i)
                cv_line(px, py, u, v, band[i * 4u / n]);
            px = u;
            py = v;
        }
        cv_rect(px - 1, py - 1, 3, 3, T_ACCENT);            /* where the circuit is now */
    }
    cv_blit(0, PLOT_Y);                                      /* (DMA: runs on while the next frame is prepared) */
}

/* an ADSR time as text: 0.5 ms .. 10 s */
static void env_time(char *b, uint32_t n, uint32_t v)
{
    uint32_t us = ENV_FRAMES[v & 127u] * 1000u / 44u;        /* (frames / 44.1 kHz, close enough to show) */
    char d[12];
    if (us < 10000u) {                                       /* 0.5 .. 9.9 ms */
        fmt_int(d, (int32_t)(us / 1000u));
        str_cpy(b, d, n);
        ui_cat(b, n, ".");
        fmt_int(d, (int32_t)(us / 100u % 10u));
        ui_cat(b, n, d);
        ui_cat(b, n, " ms");
    } else if (us < 1000000u) {
        fmt_int(d, (int32_t)(us / 1000u));
        str_cpy(b, d, n);
        ui_cat(b, n, " ms");
    } else {
        fmt_int(d, (int32_t)(us / 1000000u));
        str_cpy(b, d, n);
        ui_cat(b, n, ".");
        fmt_int(d, (int32_t)(us / 100000u % 10u));
        ui_cat(b, n, d);
        ui_cat(b, n, " s");
    }
}

/* the ENV page's picture: the gate's lamp. A round red light, as the LED inside a vactrol, lit by the newest voice's
 * gate control: full when the gate is open, dimming as the envelope falls (with the vactrol's own lag), a dark red
 * glass when nothing sounds; its glow spreads with the level. Beside it the cutoff the gate sets now. */
#define LAMP_X 120
#define LAMP_Y 58
#define LAMP_R 30                                /* the glass */
#define LAMP_G 56                                /* the glow's reach */
static uint16_t rgb565(int32_t r, int32_t g, int32_t b)
{
    r = clamp(r, 0, 255); g = clamp(g, 0, 255); b = clamp(b, 0, 255);
    return (uint16_t)(((uint32_t)r >> 3) << 11 | ((uint32_t)g >> 2) << 5 | (uint32_t)b >> 3);
}
static void draw_env(void)
{
    int32_t lv = sys.env_stage != 0u ? sys.env_view : 0;       /* 0..255 */
    int32_t br = (T_BG >> 11 & 31) * 255 / 31, bg_ = (T_BG >> 5 & 63) * 255 / 63, bb = (T_BG & 31) * 255 / 31;
    int32_t x, y, rr = LAMP_R * LAMP_R, gg = LAMP_G * LAMP_G;
    /* the lit glass's colour at this level: dark red (60, 6, 4) .. bright red (255, 40, 24), the centre hot */
    int32_t cr = 60 + 195 * lv / 255, cg = 6 + 34 * lv / 255, cb = 4 + 20 * lv / 255;
    cv_begin(240, PLOT_H, T_BG);
    for (y = LAMP_Y - LAMP_G; y <= LAMP_Y + LAMP_G; y++)
        for (x = LAMP_X - LAMP_G; x <= LAMP_X + LAMP_G; x++) {
            int32_t dx = x - LAMP_X, dy = y - LAMP_Y, d2 = dx * dx + dy * dy, r, g, b;
            if (d2 > gg)
                continue;
            if (d2 <= rr) {
                /* the glass: brighter in the middle (1 - 0.4 (d / R)^2), a soft rim, and a highlight up left */
                int32_t sh = 256 - 102 * d2 / rr, hx = dx + 11, hy = dy + 12, h2 = hx * hx + hy * hy, spec;
                r = cr * sh / 256; g = cg * sh / 256; b = cb * sh / 256;
                if (lv > 160) {                                  /* very bright: the core whitens a little */
                    int32_t w = (lv - 160) * (256 - 256 * d2 / rr) / 95;
                    r += (255 - r) * w / 512; g += (220 - g) * w / 512; b += (200 - b) * w / 512;
                }
                spec = h2 < 64 ? (64 - h2) * (40 + lv / 2) / 64 : 0;   /* the glass's reflection, lit or not */
                r += spec; g += spec * 3 / 4; b += spec * 3 / 4;
                if (d2 > rr - 2 * LAMP_R) {                      /* the edge: antialiased against the glow */
                    int32_t k = (rr - d2) * 256 / (2 * LAMP_R), a = lv * 7 / 10;   /* (the glow at the rim) */
                    int32_t or_ = br + (255 - br) * a / 256, og = bg_ + (30 - bg_) * a / 256, ob = bb + (18 - bb) * a / 256;
                    r = or_ + (r - or_) * k / 256; g = og + (g - og) * k / 256; b = ob + (b - ob) * k / 256;
                }
            } else {
                /* the glow: (1 - t)^2 of the level, t from the rim (0) to its reach (1) */
                int32_t t = (d2 - rr) * 256 / (gg - rr), f = (256 - t) * (256 - t) / 256, a = f * lv / 255;
                a = a * 7 / 10;                                  /* (a glow, not a second lamp) */
                r = br + (255 - br) * a / 256;
                g = bg_ + (30 - bg_) * a / 256;
                b = bb + (18 - bb) * a / 256;
            }
            cv_pset(x, y, rgb565(r, g, b));
        }
    if (sys.env_stage != 0u) {                               /* the cutoff the gate sets now */
        uint32_t i = (uint32_t)lv, hz = 25u, k;
        char b2[16], d[12];
        for (k = 0; k < i; k++)                              /* 25 Hz x 600^(i / 255): x 1.0254 a step */
            hz = hz + (hz * 26u + 512u) / 1024u;
        if (hz < 1000u) {
            fmt_int(d, (int32_t)hz);
            str_cpy(b2, d, sizeof b2);
            ui_cat(b2, sizeof b2, " Hz");
        } else {
            fmt_int(d, (int32_t)(hz / 1000u));
            str_cpy(b2, d, sizeof b2);
            ui_cat(b2, sizeof b2, ".");
            fmt_int(d, (int32_t)(hz / 100u % 10u));
            ui_cat(b2, sizeof b2, d);
            ui_cat(b2, sizeof b2, " kHz");
        }
        cv_text_r(232, LAMP_Y - AF_S.h / 2, &AF_S, b2, T_MID, T_BG);
    }
    cv_text(8, LAMP_Y - AF_S.h / 2, &AF_S, "GATE", T_DIM);
    cv_blit(0, PLOT_Y);
}

/* the FX page's picture: the resonator's resonances on a log-frequency axis (40 Hz .. 12 kHz; the sympathetic
 * strings marked), and under them its output */
static void draw_fx(void)
{
    const int32_t top = 6, bars = 62, mid = 92, amp = 26;
    uint32_t i, n = rz_view.n > 16u ? 16u : rz_view.n, w = rz_view.w;
    uint16_t dim = ux_mix(T_BG, T_THEME, 30);
    cv_begin(240, PLOT_H, T_BG);
    cv_rect(8, top + bars, 224, 1, T_LINE);
    {   /* the axis: 100 Hz, 1 kHz, 10 kHz */
        static const uint16_t HZ[3] = {100, 1000, 10000};
        static const char *const HL[3] = {"100", "1k", "10k"};
        for (i = 0; i < 3u; i++) {
            /* x = 8 + 224 log(f / 40) / log(300): log2 via the bit length, fine enough for ticks */
            uint32_t f = HZ[i], l2 = 0, t = f * 256u / 40u;
            while (t >= 512u) { t >>= 1; l2 += 256u; }
            l2 += (t - 256u);                                   /* log2(f / 40) Q8, linear between octaves */
            {
                int32_t x = 8 + (int32_t)(l2 * 224u / 2106u);   /* log2(300) = 8.23 octaves, Q8 2106 */
                cv_rect(x, top, 1, bars, T_LINE);
                if (i < 2u)
                    cv_text(x + 2, top, &AF_S, HL[i], T_DIM);
                else
                    cv_text_r(x - 2, top, &AF_S, HL[i], T_DIM, T_BG);   /* (the last one left of its line) */
            }
        }
    }
    if (sys.fx_mix)
        for (i = 0; i < n; i++) {
            uint32_t f = rz_view.f[i], hz, l2 = 0, t;
            int32_t x, h;
            if (!f)
                continue;
            hz = (uint32_t)(((uint64_t)f * 44100u) >> 32);
            if (hz < 40u || hz > 12000u)
                continue;
            t = hz * 256u / 40u;
            while (t >= 512u) { t >>= 1; l2 += 256u; }
            l2 += t - 256u;
            x = 8 + (int32_t)(l2 * 224u / 2106u);
            h = rz_view.a[i] < 0 ? bars : 4 + rz_view.a[i] * (bars - 4) / 32767;
            cv_rect(x - 1, top + bars - h, 3, h, rz_view.a[i] < 0 ? T_ACCENT : T_THEME);
        }
    {   /* the output, the last 224 points, scaled to its peak (which falls back slowly) */
        static int32_t scale = 2000;
        int32_t px = 0, py = mid, pk = 0;
        for (i = 0; i < 224u; i++) {
            int32_t v = rz_view.wave[(w - 224u + i) & (RZ_WAVE - 1u)];
            pk = v > pk ? v : -v > pk ? -v : pk;
        }
        scale = pk > scale ? pk : scale - (scale - (pk > 500 ? pk : 500)) / 8;
        for (i = 0; i < 224u; i++) {
            int32_t v = sys.fx_mix ? rz_view.wave[(w - 224u + i) & (RZ_WAVE - 1u)] : 0;
            int32_t y = mid - clamp(v * amp / scale, -amp, amp);
            if (i)
                cv_line(8 + px, py, 8 + (int32_t)i, y, sys.fx_mix ? T_THEME : dim);
            px = (int32_t)i;
            py = y;
        }
    }
    if (!sys.fx_mix)
        cv_text(96, top + bars / 2 - AF_S.h / 2, &AF_S, "MIX OFF", T_MID);
    cv_blit(0, PLOT_Y);
}

/* the SHIMMER page's picture: the tail's level over the last ~4 s (one column a UI frame, scrolling left: the riser
 * building and fading), and on the right the interval's staircase: the steps the tail climbs */
static uint8_t sh_hist[200];
static uint32_t sh_seen;
static void draw_sh(void)
{
    const int32_t top = 8, bot = PLOT_H - 10, hh = bot - top;
    uint32_t i, w = sh_view.w, pk = 0;
    static const uint8_t SEMI[SI_COUNT] = {12, 7, 19, 24};
    if (w - sh_seen > SH_ENV)
        sh_seen = w - SH_ENV;
    for (; sh_seen != w; sh_seen++) {
        uint32_t e = sh_view.env[sh_seen & (SH_ENV - 1u)];
        pk = e > pk ? e : pk;
    }
    for (i = 0; i + 1u < sizeof sh_hist; i++)
        sh_hist[i] = sh_hist[i + 1u];
    {   /* the level in decibels, roughly: 6 dB a bit of the peak, 2 bits of fraction; 0 .. 255 over ~70 dB */
        uint32_t b = 0, t = pk, lv;
        while (t) { b++; t >>= 1; }
        lv = b > 4u ? (b - 4u) * 20u + (b >= 3u ? ((pk >> (b - 3u)) & 3u) * 5u : 0u) : 0u;
        sh_hist[sizeof sh_hist - 1u] = (uint8_t)(sys.sh_mix ? (lv > 255u ? 255u : lv) : 0u);
    }
    cv_begin(240, PLOT_H, T_BG);
    cv_rect(8, bot, 200, 1, T_LINE);
    for (i = 0; i < sizeof sh_hist; i++) {
        int32_t h = (int32_t)sh_hist[i] * hh / 255;
        if (h > 0)
            cv_rect(8 + (int32_t)i, bot - h, 1, h, i + 40u >= sizeof sh_hist ? T_THEME : ux_mix(T_BG, T_THEME, 55));
    }
    {   /* the staircase: four steps of the interval */
        int32_t x0 = 214, sw = 6, st = (int32_t)SEMI[sys.sh_interval % SI_COUNT] * hh / 60;
        for (i = 0; i < 4u; i++) {
            int32_t y = bot - (int32_t)i * st - 2;
            if (y < top)
                break;
            cv_rect(x0 + (int32_t)i * sw, y, sw, 2, sys.sh_mix ? T_ACCENT : T_DIM);
            if (i)
                cv_rect(x0 + (int32_t)i * sw, y, 1, st, sys.sh_mix ? T_ACCENT : T_DIM);
        }
    }
    if (!sys.sh_mix) {
        memset(sh_hist, 0, sizeof sh_hist);      /* (off: the history starts again when it is turned on) */
        cv_rect(8, top, 200, hh, T_BG);
        cv_text(84, top + hh / 2 - AF_S.h / 2, &AF_S, "MIX OFF", T_MID);
    }
    cv_blit(0, PLOT_Y);
}

/* the oscillator's short name: "CHUA", or the noise program's bank and number ("A5") */
static void osc_code(char *d, uint32_t n, uint32_t osc)
{
    char c[3] = {(char)('A' + (osc - 1u) / 10u), (char)('0' + (osc - 1u) % 10u), 0};
    str_cpy(d, osc ? c : "CHUA", n);
}

static void ui_draw(void)
{
    char a[40], b[24], v[12];
    uint32_t note = sys.last_note, reg = chua_regime(sys.chaos);
    if (ui_force)
        lcd_fill(0, 0, 240, 240, T_BG);
    str_cpy(a, PRESETS[sys.preset % NPRESETS].name, sizeof a);
    if (preset_edited())
        ui_cat(a, sizeof a, " *");
    if (note < 128u && sys.voices) {
        str_cpy(b, NOTE_NAME[note % 12u], sizeof b);
        fmt_int(v, (int32_t)note / 12 - 1);
        ui_cat(b, sizeof b, v);
    } else
        str_cpy(b, usb.config && !usb.suspended ? "USB" : "", sizeof b);
    ui_head(a, b);

    if (sys.page == 0u && sys.osc) {
        uint32_t alg = (sys.osc - 1u) % NP_COUNT;
        str_cpy(a, "1 ", sizeof a);
        ui_cat(a, sizeof a, NP_X[alg]);
        fmt_int(v, sys.np_x);
        ui_cell(T_C1, a, v);
        str_cpy(a, "2 ", sizeof a);
        ui_cat(a, sizeof a, NP_Y[alg]);
        fmt_int(v, sys.np_y);
        ui_cell(T_C2, a, v);
        fmt_int(v, sys.np_level);
        ui_cell(T_C3, "3 LEVEL", v);
        fmt_int(v, sys.ring);
        ui_cell(T_C4, "4 RING", v);
    } else if (sys.page == 0u) {
        str_cpy(a, "1 CHAOS ", sizeof a);
        fmt_int(v, sys.chaos);
        ui_cat(a, sizeof a, v);
        ui_cell(T_C1, a, REG_NAME[reg]);
        {   /* beta with one decimal */
            int32_t bq = chua_beta_q16(sys.shape), t = (bq * 10 + 32768) >> 16;
            fmt_int(v, t / 10);
            str_cpy(b, "BETA ", sizeof b);
            ui_cat(b, sizeof b, v);
            ui_cat(b, sizeof b, ".");
            fmt_int(v, t % 10);
            ui_cat(b, sizeof b, v);
        }
        ui_cell(T_C2, "2 SHAPE", b);
        {
            uint32_t o = sys.output;
            ui_cell(T_C3, "3 OUTPUT", o < 16u ? "X" : o < 48u ? "X + Y" : o < 80u ? "Y" : o < 112u ? "Y + Z" : "Z");
        }
        fmt_int(v, sys.ring);
        ui_cell(T_C4, "4 RING", v);
    } else if (sys.page == 3u) {
        fmt_int(v, sys.sh_size);
        ui_cell(T_C1, "1 SIZE", v);
        {   /* RT60, as the DSP has it: 0.4 s x 75^(d / 127) */
            uint32_t ms = (uint32_t)(((uint64_t)400u * px_exp2((int32_t)((sys.sh_decay & 127u) * 408220u / 127u))) >> 16);
            fmt_int(v, (int32_t)(ms / 1000u));
            str_cpy(b, v, sizeof b);
            ui_cat(b, sizeof b, ".");
            fmt_int(v, (int32_t)(ms / 100u % 10u));
            ui_cat(b, sizeof b, v);
            ui_cat(b, sizeof b, " s");
        }
        ui_cell(T_C2, "2 DECAY", b);
        fmt_int(v, sys.sh_shimmer);
        ui_cell(T_C3, "3 SHIMMER", v);
        if (sys.sh_mix) {
            fmt_int(v, (int32_t)(sys.sh_mix * 100u / 127u));
            str_cpy(b, v, sizeof b);
            ui_cat(b, sizeof b, " %");
        } else
            str_cpy(b, "OFF", sizeof b);
        ui_cell(T_C4, "4 MIX", b);
    } else if (sys.page == 2u) {
        uint32_t md = sys.fx_model % RZ_COUNT;
        if (md == RZ_SYMP)
            str_cpy(b, RZ_CHORD_NAME[(sys.fx_struct & 127u) >> 4], sizeof b);
        else {
            fmt_int(v, sys.fx_struct);
            str_cpy(b, v, sizeof b);
        }
        ui_cell(T_C1, md == RZ_SYMP ? "1 CHORD" : "1 STRUCTURE", b);
        fmt_int(v, sys.fx_bright);
        ui_cell(T_C2, "2 BRIGHTNESS", v);
        fmt_int(v, sys.fx_damp);
        ui_cell(T_C3, "3 DAMPING", v);
        if (sys.fx_mix) {
            fmt_int(v, (int32_t)(sys.fx_mix * 100u / 127u));
            str_cpy(b, v, sizeof b);
            ui_cat(b, sizeof b, " %");
        } else
            str_cpy(b, "OFF", sizeof b);
        ui_cell(T_C4, "4 MIX", b);
    } else {
        env_time(b, sizeof b, sys.attack);
        ui_cell(T_C1, "1 ATTACK", b);
        env_time(b, sizeof b, sys.decay);
        ui_cell(T_C2, "2 DECAY", b);
        fmt_int(v, (int32_t)(sys.sustain * 100u / 127u));
        str_cpy(b, v, sizeof b);
        ui_cat(b, sizeof b, " %");
        ui_cell(T_C3, "3 SUSTAIN", b);
        env_time(b, sizeof b, sys.release);
        ui_cell(T_C4, "4 RELEASE", b);
    }

    if (ui_event_ms && fm1_ms - ui_event_ms < 2000u)
        ui_status(ui_event);
    else {
        if (sys.page == 3u) {
            str_cpy(a, SH_INTERVAL_NAME[sys.sh_interval % SI_COUNT], sizeof a);
            ui_cat(a, sizeof a, "   TONE ");
            fmt_int(v, sys.sh_tone);
            ui_cat(a, sizeof a, v);
        } else if (sys.page == 2u) {
            str_cpy(a, RZ_NAME[sys.fx_model % RZ_COUNT], sizeof a);
            ui_cat(a, sizeof a, sys.fx_model % RZ_COUNT == RZ_SYMP ? "   AMT " : "   POS ");
            fmt_int(v, sys.fx_pos);
            ui_cat(a, sizeof a, v);
        } else if (sys.page) {
            str_cpy(a, "ENV   RING ", sizeof a);
            fmt_int(v, sys.ring);
            ui_cat(a, sizeof a, v);
        } else {                                 /* HOME: the oscillator and the view */
            osc_code(a, sizeof a, sys.osc);
            if (sys.osc) {                       /* (the program's name: no room for the view as well) */
                ui_cat(a, sizeof a, " ");
                ui_cat(a, sizeof a, NP_NAME[(sys.osc - 1u) % NP_COUNT]);
            } else {
                ui_cat(a, sizeof a, "   ");
                ui_cat(a, sizeof a, VIEW_NAME[sys.view % V_COUNT]);
            }
        }
        ui_cat(a, sizeof a, "   OCT ");
        if (sys.octave > 0)
            ui_cat(a, sizeof a, "+");
        fmt_int(v, sys.octave);
        ui_cat(a, sizeof a, v);
        ui_cat(a, sizeof a, "   CPU ");
        fmt_int(v, (int32_t)(sys.cpu_q8 * 100u / 256u));
        ui_cat(a, sizeof a, v);
        ui_cat(a, sizeof a, C1_ON() ? "% x2" : "%");
        ui_status(a);
    }
    if (sys.page == 1u)
        draw_env();
    else if (sys.page == 2u)
        draw_fx();
    else if (sys.page == 3u)
        draw_sh();
    else
        draw_plot();
    ui_force = 0;
}

static void draw_splash(void)
{
    lcd_fill(0, 0, 240, 240, T_BG);
    draw_text_box(0, 92, 240, &AF_L, "CHUA", T_THEME, 1);
    draw_text_box(0, 134, 240, &AF_S, "A CHAOTIC CIRCUIT SYNTH", T_MID, 1);
    draw_text_box(0, 152, 240, &AF_S, UI_VERSION, T_DIM, 1);
}

/* -------------------------------------------------------------- LEDs --- */
/* The picture is built off-line and written one byte per column: clearing and relighting would let the
 * 10 kHz scan catch the dark gap and flicker. Every LED glows dim (fm1_led_dim), lit while pressed. */
static void led_on(uint8_t *nl, uint32_t id)            /* matrix id: button 0..13, note key 14..40 */
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id)
                nl[p] |= (uint8_t)(1u << r);
}

static void ui_leds(void)
{
    uint8_t nl[FM1_NCOL];
    uint32_t i, notes = fm1_in.notes, btns = fm1_in.buttons;
    if (fm1_led_anim_on())                               /* the power-on sweep still runs (hal/fm1_led_anim.h) */
        return;
    memset(nl, 0, sizeof nl);
    for (i = 0; i < NKEYS; i++)
        if ((notes >> i) & 1u)
            led_on(nl, 14u + i);
    for (i = 0; i < 14u; i++)
        if ((btns >> i) & 1u)
            led_on(nl, i);
    if (sys.octave < 0)
        led_on(nl, panel.btn[B_OCTDN]);
    if (sys.octave > 0)
        led_on(nl, panel.btn[B_OCTUP]);
    if (sys.page == 0u)
        led_on(nl, panel.btn[B_HOME]);
    if (sys.page == 1u)
        led_on(nl, panel.btn[B_ENV]);
    if (sys.page >= 2u || sys.fx_mix || sys.sh_mix)     /* (FX lit: its pages, or the resonator or the shimmer on) */
        led_on(nl, panel.btn[B_FX]);
    for (i = 0; i < FM1_NCOL; i++)
        fm1_led[i] = nl[i];
}

static void ui_leds_init(void)                         /* the idle glow under every LED */
{
    uint8_t nl[FM1_NCOL];
    uint32_t i;
    memset(nl, 0, sizeof nl);
    for (i = 0; i < 14u + NKEYS; i++)
        led_on(nl, i);
    for (i = 0; i < FM1_NCOL; i++)
        fm1_led_dim[i] = nl[i];
}

/* ------------------------------------------------------------- input --- */
static void knob(volatile uint8_t *p, uint32_t role, uint32_t cell)
{
    int32_t s = panel_enc(role);
    if (!s)
        return;
    *p = (uint8_t)clamp(*p + s, 0, 127);
    if (cell < 4u) {
        ui_hot = (uint8_t)cell;
        ui_hot_ms = fm1_ms;
    }
}

static void ui_input(void)
{
    uint32_t pressed = fm1_input_edges(0), b;
    int32_t s;
    char t[24], v[12];
    for (b = 0; b < NB; b++) {
        if (!(pressed & btn_bit(b)))
            continue;
        switch (b) {
        case B_OCTDN:
        case B_OCTUP:
            if ((fm1_in.buttons & (btn_bit(B_OCTDN) | btn_bit(B_OCTUP))) == (btn_bit(B_OCTDN) | btn_bit(B_OCTUP)))
                break;                                  /* both: the UBOOT combo (main.c), not an octave */
            sys.octave = (int8_t)clamp(sys.octave + (b == B_OCTUP ? 1 : -1), OCT_MIN, OCT_MAX);
            break;
        case B_SAVE:
            ui_say(settings_save() == 0 ? "SAVED" : flash_ok ? "SAVE FAILED" : "NO FLASH");
            break;
        case B_HOME:                                    /* the circuit's page: the oscillator */
            if (sys.page) {
                sys.page = 0;
                ui_force = 1;
            }
            break;
        case B_ENV:                                     /* the ENV page and back */
            sys.page = sys.page == 1u ? 0u : 1u;
            ui_force = 1;
            break;
        case B_FX:                                      /* the resonator, the shimmer, back HOME */
            sys.page = sys.page == 2u ? 3u : sys.page == 3u ? 0u : 2u;
            ui_force = 1;
            break;
        case B_GLO:
            sys.cpu2 = (uint8_t)!sys.cpu2;
            settings_save();
            ui_say(sys.cpu2 ? "2ND CORE ON: RESTART" : "2ND CORE OFF: RESTART");
            break;
        default:
            str_cpy(t, "BUTTON ", sizeof t);
            ui_cat(t, sizeof t, B_NAME[b]);
            ui_cat(t, sizeof t, ": FREE");
            ui_say(t);
            break;
        }
    }
    if (sys.page == 0u && sys.osc) {
        knob(&sys.np_x, EN_K1, 0);
        knob(&sys.np_y, EN_K2, 1);
        knob(&sys.np_level, EN_K3, 2);
        knob(&sys.ring, EN_K4, 3);
    } else if (sys.page == 0u) {
        knob(&sys.chaos, EN_K1, 0);
        knob(&sys.shape, EN_K2, 1);
        knob(&sys.output, EN_K3, 2);
        knob(&sys.ring, EN_K4, 3);
    } else if (sys.page == 3u) {
        knob(&sys.sh_size, EN_K1, 0);
        knob(&sys.sh_decay, EN_K2, 1);
        knob(&sys.sh_shimmer, EN_K3, 2);
        knob(&sys.sh_mix, EN_K4, 3);
    } else if (sys.page == 2u) {
        knob(&sys.fx_struct, EN_K1, 0);
        knob(&sys.fx_bright, EN_K2, 1);
        knob(&sys.fx_damp, EN_K3, 2);
        knob(&sys.fx_mix, EN_K4, 3);
    } else {
        knob(&sys.attack, EN_K1, 0);
        knob(&sys.decay, EN_K2, 1);
        knob(&sys.sustain, EN_K3, 2);
        knob(&sys.release, EN_K4, 3);
    }
    if ((s = panel_enc(EN_SELECT)) != 0 && sys.page == 0u) {   /* HOME: SELECT turns the view */
        sys.view = (uint8_t)((sys.view + V_COUNT * 8 + s) % V_COUNT);
        str_cpy(t, "VIEW ", sizeof t);
        ui_cat(t, sizeof t, VIEW_NAME[sys.view]);
        ui_say(t);
    } else if (s) {
        volatile uint8_t *p = sys.page == 3u ? &sys.sh_tone : sys.page == 2u ? &sys.fx_pos : &sys.ring;
        *p = (uint8_t)clamp(*p + s, 0, 127);
        str_cpy(t, sys.page == 3u ? "TONE " : sys.page == 2u ? (sys.fx_model % RZ_COUNT == RZ_SYMP ? "AMOUNT " : "POSITION ")
                                                             : "RING ", sizeof t);
        fmt_int(v, *p);
        ui_cat(t, sizeof t, v);
        ui_say(t);
    }
    if ((s = panel_enc(EN_ALGO)) != 0 && sys.page == 3u) {
        sys.sh_interval = (uint8_t)((sys.sh_interval + SI_COUNT * 8 + s) % SI_COUNT);
        str_cpy(t, "INTERVAL ", sizeof t);
        ui_cat(t, sizeof t, SH_INTERVAL_NAME[sys.sh_interval]);
        ui_say(t);
    } else if (s && sys.page == 2u) {
        sys.fx_model = (uint8_t)((sys.fx_model + RZ_COUNT * 8 + s) % RZ_COUNT);
        str_cpy(t, "MODEL ", sizeof t);
        ui_cat(t, sizeof t, RZ_NAME[sys.fx_model]);
        ui_say(t);
    } else if (s && !sys.page) {                 /* HOME: the oscillator, CHUA and the 30 noise programs */
        sys.osc = (uint8_t)(((int32_t)sys.osc + (int32_t)(NP_COUNT + 1u) * 8 + s) % (int32_t)(NP_COUNT + 1u));
        osc_code(t, sizeof t, sys.osc);
        if (sys.osc) {
            ui_cat(t, sizeof t, " ");
            ui_cat(t, sizeof t, NP_NAME[sys.osc - 1u]);
        } else
            ui_cat(t, sizeof t, " CIRCUIT");
        ui_say(t);
    }
    if ((s = panel_enc(EN_PRESET)) != 0)
        preset_load((uint32_t)((int32_t)sys.preset + (int32_t)NPRESETS * 8 + s));
}
