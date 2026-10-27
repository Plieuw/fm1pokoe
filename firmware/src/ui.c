/* SPDX-License-Identifier: GPL-3.0-only */
/* CHUA UI (main loop only): the attractor drawn live, the knobs, presets, LEDs, the settings in flash.
 *
 * Two pages; ENV switches between them (its LED lit on the ENV page):
 *   CIRCUIT  KNOB 1 CHAOS   the regime: cycle, period 2, spiral, double scroll, outer cycle
 *            KNOB 2 SHAPE   beta: slow round spirals .. tight fast ones
 *            KNOB 3 OUTPUT  which voltage you hear: X, X+Y, Y, Y+Z, Z
 *            KNOB 4 RING    the low pass gate's resonance
 *   ENV      KNOB 1..4      ATTACK, DECAY, SUSTAIN, RELEASE of the ADSR that drives the low pass gate; the envelope drawn
 *   SELECT   RING (on either page)
 *   PRESETS the presets                      ALGORITHM  the view: 3D (turning), X-Y, X-Z, Y-Z, TIME
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
/* ADSR times: 0 = 0.5 ms .. 127 = 10 s (exponential: 40 ~11 ms, 64 ~74 ms, 80 ~250 ms, 100 ~1.2 s); RING: 0 none .. 127 Q 40 */
typedef struct { const char *name; uint8_t chaos, shape, output, attack, decay, sustain, release, ring; } preset_t;
static const preset_t PRESETS[] = {
    {"DOUBLE SCROLL", 85, 76, 0, 6, 80, 110, 70, 90},     /* the classic double scroll, x (the scroll jumps) */
    {"RING PLUCK", 85, 90, 64, 0, 62, 0, 66, 124},        /* the gate pinged: the filter rings on the spiral */
    {"PURE CYCLE", 6, 64, 64, 10, 70, 120, 64, 60},       /* just past the Hopf point: a clean periodic orbit, y */
    {"PERIOD TWO", 38, 64, 32, 4, 60, 90, 66, 100},       /* the first period doubling: a sub-octave appears */
    {"SPIRAL", 54, 56, 64, 2, 64, 80, 72, 110},           /* Roessler-like chaos on one scroll: pitched, rough */
    {"BONGO GATE", 30, 100, 32, 0, 50, 0, 52, 112},       /* a short gate on a clean cycle: a wooden ring */
    {"CRISIS EDGE", 110, 64, 96, 2, 76, 100, 74, 104},    /* the double scroll about to break out */
    {"OUTER LOOP", 124, 40, 64, 0, 56, 60, 60, 80},       /* the saturated circuit's big cycle: loud, buzzy */
    {"SLOW BLOOM", 70, 110, 127, 92, 96, 110, 100, 116},  /* z, fast spirals, slow envelope, ringing gate: a pad */
    {"GRIT BASS", 96, 16, 0, 0, 54, 70, 50, 70},          /* x at low beta: slow, deep switching */
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
}
static int preset_edited(void)
{
    const preset_t *p = &PRESETS[sys.preset % NPRESETS];
    return sys.chaos != p->chaos || sys.shape != p->shape || sys.output != p->output || sys.attack != p->attack ||
           sys.decay != p->decay || sys.sustain != p->sustain || sys.release != p->release || sys.ring != p->ring;
}

/* ------------------------------------------------------------ settings --- */
#define SET_MAGIC 0x43485533u      /* "CHU3": bump when the layout changes (older records are then ignored) */
typedef struct {
    uint32_t magic;
    int8_t octave;
    uint8_t chaos, shape, output, attack, release, view, preset, cpu2, decay, sustain, ring;
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

/* the ENV page's picture: the ADSR on a log-time axis (each segment as wide as its knob is far), the sustain held a
 * while, filled; the newest voice's gate level as a line, and the cutoff it sets */
static void draw_env(void)
{
    const int32_t top = 10, bot = PLOT_H - 14, hh = bot - top;
    int32_t wa = 8 + sys.attack * 2 / 5, wd = 8 + sys.decay * 2 / 5, wr = 8 + sys.release * 2 / 5, ws = 40;
    int32_t x0 = (240 - (wa + wd + ws + wr)) / 2, x, prev = bot, sus = bot - (int32_t)sys.sustain * hh / 127;
    uint16_t fill = ux_mix(T_BG, T_THEME, 18);
    cv_begin(240, PLOT_H, T_BG);
    cv_rect(8, bot, 224, 1, T_LINE);
    for (x = 0; x < wa + wd + ws + wr; x++) {                /* the curve, column by column */
        int32_t y;
        if (x < wa)
            y = bot - hh * x / wa;
        else if (x < wa + wd) {                              /* exponential: ~4 time constants across */
            int32_t t = (x - wa) * 4096 / wd, e = 4096;
            int32_t k;
            for (k = 0; k < 4; k++)                          /* e = exp(-4 t) by four steps of (1 - t)^... */
                e = e * (4096 - t) / 4096;
            y = sus - (sus - top) * e / 4096;
        } else if (x < wa + wd + ws)
            y = sus;
        else {
            int32_t t = (x - wa - wd - ws) * 4096 / wr, e = 4096, k;
            for (k = 0; k < 4; k++)
                e = e * (4096 - t) / 4096;
            y = bot - (bot - sus) * e / 4096;
        }
        cv_rect(x0 + x, y, 1, bot - y, fill);
        if (x)
            cv_line(x0 + x - 1, prev, x0 + x, y, T_THEME);
        prev = y;
    }
    {
        static const char *const L[4] = {"A", "D", "S", "R"};
        const int32_t xs[4] = {x0 + wa / 2, x0 + wa + wd / 2, x0 + wa + wd + ws / 2, x0 + wa + wd + ws + wr / 2};
        int32_t i;
        for (i = 0; i < 4; i++)
            cv_text(xs[i] - 3, bot + 1, &AF_S, L[i], T_MID);
    }
    if (sys.env_stage != 0u) {                               /* a voice sounds: its gate's level, and the cutoff */
        int32_t y = bot - (int32_t)sys.env_view * hh / 255;
        uint32_t i = sys.env_view, hz = 25u, k;
        char b[16], d[12];
        for (k = 0; k < i; k++)                              /* 25 Hz x 600^(i / 255): x 1.0254 a step */
            hz = hz + (hz * 26u + 512u) / 1024u;
        cv_rect(8, y, 224, 1, T_ACCENT);
        if (hz < 1000u) {
            fmt_int(d, (int32_t)hz);
            str_cpy(b, d, sizeof b);
            ui_cat(b, sizeof b, " Hz");
        } else {
            fmt_int(d, (int32_t)(hz / 1000u));
            str_cpy(b, d, sizeof b);
            ui_cat(b, sizeof b, ".");
            fmt_int(d, (int32_t)(hz / 100u % 10u));
            ui_cat(b, sizeof b, d);
            ui_cat(b, sizeof b, " kHz");
        }
        cv_text_r(232, y > top + 16 ? y - 16 : y + 3, &AF_S, b, T_ACCENT, T_BG);
    }
    cv_blit(0, PLOT_Y);
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

    if (sys.page == 0u) {
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
        str_cpy(a, sys.page ? "ENV   RING " : "RING ", sizeof a);
        fmt_int(v, sys.ring);
        ui_cat(a, sizeof a, v);
        ui_cat(a, sizeof a, "   ");
        if (!sys.page)
            ui_cat(a, sizeof a, VIEW_NAME[sys.view % V_COUNT]);
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
    if (sys.page)
        draw_env();
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
    if (sys.page)
        led_on(nl, panel.btn[B_ENV]);
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
        case B_ENV:                                     /* the ENV page and back */
            sys.page = (uint8_t)!sys.page;
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
    if (sys.page == 0u) {
        knob(&sys.chaos, EN_K1, 0);
        knob(&sys.shape, EN_K2, 1);
        knob(&sys.output, EN_K3, 2);
        knob(&sys.ring, EN_K4, 3);
    } else {
        knob(&sys.attack, EN_K1, 0);
        knob(&sys.decay, EN_K2, 1);
        knob(&sys.sustain, EN_K3, 2);
        knob(&sys.release, EN_K4, 3);
    }
    if ((s = panel_enc(EN_SELECT)) != 0) {
        sys.ring = (uint8_t)clamp(sys.ring + s, 0, 127);
        str_cpy(t, "RING ", sizeof t);
        fmt_int(v, sys.ring);
        ui_cat(t, sizeof t, v);
        ui_say(t);
    }
    if ((s = panel_enc(EN_ALGO)) != 0 && !sys.page) {
        sys.view = (uint8_t)((sys.view + V_COUNT * 8 + s) % V_COUNT);
        str_cpy(t, "VIEW ", sizeof t);
        ui_cat(t, sizeof t, VIEW_NAME[sys.view]);
        ui_say(t);
    }
    if ((s = panel_enc(EN_PRESET)) != 0)
        preset_load((uint32_t)((int32_t)sys.preset + (int32_t)NPRESETS * 8 + s));
}
