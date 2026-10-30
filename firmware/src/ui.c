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
 *   MOD ENV  (ENV again) an AD envelope straight on the noise program's pitch (no oscillator, no VCA), started by
 *            each note: KNOB 1 LENGTH, KNOB 2 SKEW (where the peak falls), KNOB 3 HEIGHT (the sweep at the peak, up to
 *            36 semitones), KNOB 4 BEND (its curve, both ways); drawn: its shape, the sweep now
 *   ENV      KNOB 1..4      ATTACK, DECAY, SUSTAIN, RELEASE of the ADSR that drives the low pass gate; the envelope drawn
 *   SCL      the PAN page (again: back HOME): KNOB 1 BASIS: the stereo spread, each note the next place of a cycle
 *            (left, right, half left, ...); KNOB 2 ORBIT: how much each voice's phase moves with its picture (all-passes: the circuit's
 *            scroll left / right, a drift); the voices drawn where they are
 *   FX       KNOB 1..4      STRUCTURE, BRIGHTNESS, DAMPING, MIX of the resonators (resonator.c: Rings' models, 4 voices;
 *            and the waveguides of waveguide.c: THROAT (KNOB 1 the ANIMAL, SELECT the VOWEL), FLUTE (JET, BREATH),
 *            SYRINX (CHIRP, TWO VOICE));
 *            ALGORITHM the model (MODAL, SYMP STRING, STRING), SELECT the POSITION; its resonances and output drawn
 *   FX (2nd) KNOB 1..4      SIZE, DECAY, SHIMMER, MIX of the shimmer reverb (shimmer.c); ALGORITHM the INTERVAL,
 *            SELECT the TONE; the tail's level over the last seconds drawn (the riser building)
 *   SELECT   HOME: the view (3D turning, X-Y, X-Z, Y-Z, TIME)
 * Each parameter lives in one place: RING on HOME (KNOB 4), the oscillator on HOME (ALGORITHM).
 *   PRESETS the presets
 *   PLAY     starts / stops the ARP or KARMA (on their pages that one; elsewhere the last of them opened; lit while
 *            one runs)
 *   ARP      (the ARP button; again: back HOME; its LED lit while on) the arpeggiator (arp.c): KNOB 1 BPM (EXT under
 *            MIDI clock), KNOB 2 SUBDIVIDE, KNOB 3 GATE, KNOB 4 SWING; ALGORITHM the pattern, SELECT HOLD
 *   REC      the FREE EG page (feg.c): KNOB 1 the knob recorded, KNOB 2 LENGTH (NOTE, 1..8 STEPS, 1..4 BARS),
 *            KNOB 3 DEPTH, KNOB 4 the TARGET; SELECT PLAY; REC again there: record (NOTE: REC once more to stop)
 *   SEQ      the KARMA page (karma.c; again: back HOME; PLAY starts it, its LED lit while on): ALGORITHM the GE (the phrase
 *            generator), KNOB 1..4 its four realtime controls (named per GE: DENSITY, RANGE, REPEAT ...), SELECT LATCH,
 *            PRESETS the tempo (the ARP page's BPM: one for all)
 *   EDIT     the EDIT page (again: back HOME): KNOB 1 the MOD WHEEL's parameter (MIDI CC 1; FREE EG's list), KNOB 2
 *            its DEPTH (FREE EG playing the same one: the wheel sets FREE EG's depth), KNOB 3 VELOCITY on the sound (OFF, BRIGHT, FM, BOTH), KNOB 4 how much; MIDI program change
 *            loads the preset (program 1 the first)
 *   PRESETS  on the FX page: the FX presets (a resonator / waveguide with its knobs), on SHIMMER: the shimmer
 *            presets; elsewhere the sound presets (on KARMA: the tempo)
 *   OCT- / OCT+  octave -3..+3               SAVE       the settings into flash (restored at power-on)
 *   GLO      the MIDI page (again: back HOME): KNOB 1 TRS IN on / off, KNOB 2 USB (OFF, IN, OUT, IN+OUT), KNOB 3 the
 *            RX channel (OMNI, 1..16), KNOB 4 the TX channel; SELECT 2ND CORE on / off (saved at once; from the next
 *            power-on: the core starts only at boot)
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
    uint8_t menv_len, menv_skew, menv_height;    /* its AD envelope (HEIGHT 0, left out: none) */
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
    /* the waveguides (FX: THROAT, FLUTE, SYRINX), the voices their source */
    {"CAT MEOW", 6, 64, 64, 30, 80, 90, 80, 30, RZ_THROAT, 8, 70, 60, 80, 127},      /* a pure cycle into a cat's */
                                                                                       /* mouth, opening I > A > U */
    {"COW MOO", 85, 76, 64, 60, 90, 110, 90, 20, RZ_THROAT, 88, 40, 80, 10, 120},      /* the scroll, a long nasal U */
    {"WOLF HOWL", 6, 100, 64, 90, 100, 110, 110, 30, RZ_THROAT, 72, 60, 90, 20, 115,  /* a slow U > O in a big room */
     110, 110, 40, 70, SI_OCT, 70},
    {"TALKING CHAOS", 85, 76, 32, 4, 70, 100, 70, 40, RZ_THROAT, 40, 80, 60, 64, 110},  /* a human throat: turn VOWEL */
    {"ELEPHANT", 96, 16, 0, 20, 90, 100, 90, 30, RZ_THROAT, 120, 30, 100, 40, 127},   /* the trunk: a low trumpet */
    {"NIGHT FLUTE", 6, 64, 64, 40, 80, 110, 90, 20, RZ_FLUTE, 30, 70, 90, 50, 127,    /* breathy, in the shimmer */
     110, 100, 60, 60, SI_OCT, 70},
    {"OVERBLOWN", 54, 90, 64, 10, 70, 100, 70, 30, RZ_FLUTE, 115, 90, 100, 20, 120},  /* the jet short: octaves */
    {"PAN NOISE FLUTE", 85, 76, 0, 30, 80, 110, 80, 20, RZ_FLUTE, 50, 60, 90, 90, 127, /* a noise program blows it */
     NOSH, NZ(0, 9), 70, 50},
    {"BIRD SONG", 6, 64, 64, 0, 90, 0, 60, 30, RZ_SYRINX, 90, 70, 60, 15, 127,        /* chirps falling away */
     100, 90, 30, 40, SI_OCT, 100},
    {"TWO BIRDS", 30, 100, 32, 0, 60, 40, 50, 30, RZ_SYRINX, 60, 90, 70, 110, 127},   /* the two sides a fifth */
    {"HAWK CRY", 6, 64, 64, 0, 100, 0, 80, 30, RZ_HAWK, 70, 90, 70, 70, 127,          /* kee-eeee-arr, over a valley */
     110, 100, 40, 35, SI_OCT, 90},
    /* the MOD ENV on the pitch (ENV twice) */
    {"PING BELL", 85, 76, 0, 0, 62, 0, 70, 40, RZ_MODAL, 70, 100, 110, 12, 80,           /* the cluster bell, each */
     NOSH, NZ(1, 0), 30, 80, 30, 0, 40},                                                   /* strike a little ping */
    {"PITCH ZAP", 85, 76, 0, 0, 64, 40, 64, 40, NOFX, NOSH, NZ(1, 3), 10, 20,           /* a fast drop, a zap */
     56, 0, 110},
};
#define NPRESETS (sizeof PRESETS / sizeof PRESETS[0])

/* the FX page's own presets (PRESETS turned there): the resonator / waveguide alone, the sound left as it is */
typedef struct { const char *name; uint8_t model, st, br, dm, pos, mix; } fx_preset_t;
static const fx_preset_t FX_PRESETS[] = {
    {"FX OFF", 0, 70, 90, 100, 24, 0},
    {"BELL", RZ_MODAL, 70, 100, 110, 12, 100},     {"TUNED BAR", RZ_MODAL, 116, 40, 80, 24, 70},
    {"GLASS", RZ_MODAL, 100, 127, 120, 40, 80},    {"SITAR", RZ_SYMP, 16, 80, 105, 70, 90},
    {"DRONE STRINGS", RZ_SYMP, 60, 60, 115, 40, 80}, {"PIANO STRING", RZ_STRING, 100, 70, 96, 20, 85},
    {"PLUCKED STRING", RZ_STRING, 40, 90, 60, 50, 90}, {"CAT", RZ_THROAT, 8, 70, 60, 80, 127},
    {"COW", RZ_THROAT, 88, 40, 80, 10, 120},       {"WOLF", RZ_THROAT, 72, 60, 90, 20, 115},
    {"TALKING", RZ_THROAT, 40, 80, 60, 64, 110},   {"ELEPHANT", RZ_THROAT, 120, 30, 100, 40, 127},
    {"NIGHT FLUTE", RZ_FLUTE, 30, 70, 90, 50, 127}, {"OVERBLOWN", RZ_FLUTE, 115, 90, 100, 20, 120},
    {"BIRD", RZ_SYRINX, 90, 70, 60, 15, 127},      {"TWO BIRDS", RZ_SYRINX, 60, 90, 70, 110, 127},
    {"HAWK", RZ_HAWK, 70, 90, 70, 70, 127},
};
#define NFX_PRESETS (sizeof FX_PRESETS / sizeof FX_PRESETS[0])
/* the SHIMMER page's own presets */
typedef struct { const char *name; uint8_t size, decay, shimmer, mix, interval, tone; } sh_preset_t;
static const sh_preset_t SH_PRESETS[] = {
    {"SHIMMER OFF", 100, 90, 70, 0, SI_OCT, 80},
    {"HEAVEN", 110, 110, 100, 96, SI_OCT, 100},     {"OCTAVE HAZE", 127, 120, 50, 80, SI_OCT, 80},
    {"FIFTHS", 90, 110, 120, 100, SI_FIFTH, 110},   {"SMALL ROOM", 60, 60, 20, 60, SI_OCT, 70},
    {"VALLEY", 110, 100, 40, 35, SI_OCT, 90},       {"TWO OCTAVES", 120, 115, 90, 80, SI_2OCT, 100},
    {"OCT + 5TH CLOUD", 127, 125, 80, 90, SI_OCT5, 90},
};
#define NSH_PRESETS (sizeof SH_PRESETS / sizeof SH_PRESETS[0])
static uint8_t fx_pre, sh_pre;                   /* the last chosen of each (not saved: the knobs are) */

static void preset_load(uint32_t i)
{
    const preset_t *p = &PRESETS[i % NPRESETS];
    mwd.on = 0;                                         /* (the mod wheel takes the new value as its base) */
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
        sys.menv_len = p->menv_height ? p->menv_len : 80;
        sys.menv_skew = p->menv_height ? p->menv_skew : 10;
        sys.menv_height = p->menv_height;
        sys.menv_bend = 64;
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
           sys.osc != p->osc || (p->osc && (sys.np_x != p->np_x || sys.np_y != p->np_y || sys.np_level != 64u ||
           sys.menv_height != p->menv_height || (p->menv_height && (sys.menv_len != p->menv_len ||
           sys.menv_skew != p->menv_skew || sys.menv_bend != 64u))));
}

/* ------------------------------------------------------------ settings --- */
#define SET_MAGIC 0x43485543u      /* "CHUA": bump when the layout changes (older records are then ignored) */
typedef struct {
    uint32_t magic;
    int8_t octave;
    uint8_t chaos, shape, output, attack, release, view, preset, cpu2, decay, sustain, ring;
    uint8_t fx_model, fx_struct, fx_bright, fx_damp, fx_pos, fx_mix;
    uint8_t sh_size, sh_decay, sh_shimmer, sh_mix, sh_interval, sh_tone;
    uint8_t osc, np_x, np_y, np_level;
    uint8_t menv_len, menv_skew, menv_height, basis, orbit, menv_bend;
    uint8_t arp_on, arp_type, arp_bpm, arp_div, arp_gate, arp_swing, arp_hold;
    uint8_t midi_trs, midi_usb, midi_rx, midi_tx;
    uint8_t feg_len, feg_depth, feg_target, feg_on, feg_has, feg_npts, feg_curve[FEG_N];
    uint8_t k_on, k_ge, k_latch, k_m[4];
    uint8_t mw_target, mw_depth, vel_to, vel_amt;
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
        sys.cpu2 = (uint8_t)(s.cpu2 & 3u);
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
        sys.menv_len = (uint8_t)(s.menv_len & 127u);
        sys.menv_skew = (uint8_t)(s.menv_skew & 127u);
        sys.menv_height = (uint8_t)(s.menv_height & 127u);
        sys.menv_bend = (uint8_t)(s.menv_bend & 127u);
        sys.basis = (uint8_t)(s.basis & 127u);
        sys.orbit = (uint8_t)(s.orbit & 127u);
        sys.arp_on = (uint8_t)(s.arp_on != 0);
        sys.arp_type = (uint8_t)(s.arp_type % ARP_NTYPE);
        sys.arp_bpm = (uint8_t)clamp(s.arp_bpm, 40, 250);
        sys.arp_div = (uint8_t)(s.arp_div % ARP_NDIV);
        sys.arp_gate = (uint8_t)(s.arp_gate & 127u);
        sys.arp_swing = (uint8_t)(s.arp_swing & 127u);
        sys.arp_hold = (uint8_t)(s.arp_hold != 0);
        sys.midi_trs = (uint8_t)(s.midi_trs != 0);
        sys.midi_usb = (uint8_t)(s.midi_usb & 3u);
        sys.midi_rx = (uint8_t)(s.midi_rx > 16u ? 0u : s.midi_rx);
        sys.midi_tx = (uint8_t)(s.midi_tx & 15u);
        sys.feg_len = (uint8_t)(s.feg_len % FEG_NLEN);
        sys.feg_depth = (uint8_t)(s.feg_depth & 127u);
        sys.feg_target = (uint8_t)(s.feg_target % FEG_NT);
        sys.feg_on = (uint8_t)(s.feg_on != 0);
        sys.feg_has = (uint8_t)(s.feg_has != 0);
        sys.feg_npts = (uint8_t)(s.feg_npts & 127u);
        for (uint32_t i = 0; i < FEG_N; i++)
            feg.curve[i] = (uint8_t)(s.feg_curve[i] & 127u);
        sys.k_on = (uint8_t)(s.k_on != 0 && !sys.arp_on);
        sys.k_ge = (uint8_t)(s.k_ge % KGE_N);
        sys.k_latch = (uint8_t)(s.k_latch != 0);
        sys.mw_target = (uint8_t)(s.mw_target % FEG_NT);
        sys.mw_depth = (uint8_t)(s.mw_depth & 127u);
        sys.vel_to = (uint8_t)(s.vel_to & 3u);
        sys.vel_amt = (uint8_t)(s.vel_amt & 127u);
        for (uint32_t i = 0; i < 4u; i++)
            sys.k_m[i] = (uint8_t)(s.k_m[i] & 127u);
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
    s.menv_len = sys.menv_len;
    s.menv_skew = sys.menv_skew;
    s.menv_height = sys.menv_height;
    s.menv_bend = sys.menv_bend;
    s.basis = sys.basis;
    s.orbit = sys.orbit;
    s.arp_on = sys.arp_on;
    s.arp_type = sys.arp_type;
    s.arp_bpm = sys.arp_bpm;
    s.arp_div = sys.arp_div;
    s.arp_gate = sys.arp_gate;
    s.arp_swing = sys.arp_swing;
    s.arp_hold = sys.arp_hold;
    s.midi_trs = sys.midi_trs;
    s.midi_usb = sys.midi_usb;
    s.midi_rx = sys.midi_rx;
    s.midi_tx = sys.midi_tx;
    s.feg_len = sys.feg_len;
    s.feg_depth = sys.feg_depth;
    s.feg_target = sys.feg_target;
    s.feg_on = sys.feg_on;
    s.feg_has = sys.feg_has;
    s.feg_npts = sys.feg_npts;
    memcpy(s.feg_curve, feg.curve, sizeof s.feg_curve);
    s.k_on = sys.k_on;
    s.k_ge = sys.k_ge;
    s.k_latch = sys.k_latch;
    s.mw_target = sys.mw_target;
    s.mw_depth = sys.mw_depth;
    s.vel_to = sys.vel_to;
    s.vel_amt = sys.vel_amt;
    for (uint32_t i = 0; i < 4u; i++)
        s.k_m[i] = sys.k_m[i];
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

/* the MOD ENV page's colour (the pitch envelope, the FM of the noise programs): the DX7's green, for overview */
#define T_FM ((uint16_t)(7u << 11 | 49u << 5 | 17u))    /* (60, 196, 140) */
#define T_FM_IN T_THEME                           /* a program's own FM (not the MOD oscillator's): the theme's orange */
static uint16_t ui_val_c;                        /* the cells' values in a page's own colour (0: T_TEXT) */
static int np_is_fm(const char *s)              /* a noise program's knob that is FM: its name has FM or MOD in it */
{
    for (; *s; s++)
        if ((s[0] == 'F' && s[1] == 'M') || (s[0] == 'M' && s[1] == 'O' && s[2] == 'D'))
            return 1;
    return 0;
}
#define FEG_FM(t) ((t) % FEG_NT == 7u)   /* FREE EG's MOD ENV HEIGHT (green, as its page) */

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
    cv_text(0, AF_S.h, &AF_M, value, hot[0] == '1' ? T_ACCENT : ui_val_c ? ui_val_c : T_TEXT);
    cv_blit(X[c], Y[c]);
    lcd_sync();
}

/* the status line; a button with more than one page (ENV: + MOD ENV, FX: + SHIMMER) shows its layers as
 * small blocks in the right corner: the page shown filled in the theme's orange, the other an empty outline */
static void ui_status(const char *s)
{
    static const uint8_t LAYERS[12] = {0, 1, 1, 2, 0, 2, 0, 0, 0, 0, 0, 0};   /* each page: its layer (1, 2; 0 none) */
    uint32_t pg = sys.page < 12u ? sys.page : 0u, l = LAYERS[pg];
    char sig[2] = {(char)('0' + l), 0};
    if (!ui_changed(T_STATUS, s, sig))
        return;
    if (!l) {
        draw_text_line(0, 223, 240, &AF_S, s, T_MID, T_BG, 1);
        return;
    }
    draw_text_line(0, 223, 216, &AF_S, s, T_MID, T_BG, 1);
    cv_begin(24, AF_S.h, T_BG);
    {
        uint32_t i, y = (AF_S.h - 7u) / 2u;
        for (i = 0; i < 2u; i++) {
            if (i + 1u == l)
                cv_rect((int32_t)(4u + i * 10u), (int32_t)y, 7, 7, T_THEME);
            else
                cv_frame((int32_t)(4u + i * 10u), (int32_t)y, 7, 7, T_MID);
        }
    }
    cv_blit(216, 223);
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

/* the ENV page's picture: the gate's lamp. A round light in the theme's orange (no highlight), as the LED inside a vactrol, lit by the newest voice's
 * gate control: full when the gate is open, dimming as the envelope falls (with the vactrol's own lag), a dark
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
    /* the lit glass's colour at this level: the theme's orange, from a quarter (dark) to full */
    int32_t tr = (T_THEME >> 11 & 31) * 255 / 31, tg = (T_THEME >> 5 & 63) * 255 / 63, tb = (T_THEME & 31) * 255 / 31;
    int32_t cr = tr * (64 + 192 * lv / 255) / 256, cg = tg * (64 + 192 * lv / 255) / 256, cb = tb * (64 + 192 * lv / 255) / 256;
    cv_begin(240, PLOT_H, T_BG);
    for (y = LAMP_Y - LAMP_G; y <= LAMP_Y + LAMP_G; y++)
        for (x = LAMP_X - LAMP_G; x <= LAMP_X + LAMP_G; x++) {
            int32_t dx = x - LAMP_X, dy = y - LAMP_Y, d2 = dx * dx + dy * dy, r, g, b;
            if (d2 > gg)
                continue;
            if (d2 <= rr) {
                /* the glass: brighter in the middle (1 - 0.4 (d / R)^2), and a soft rim */
                int32_t sh = 256 - 102 * d2 / rr;
                r = cr * sh / 256; g = cg * sh / 256; b = cb * sh / 256;
                if (d2 > rr - 2 * LAMP_R) {                      /* the edge: antialiased against the glow */
                    int32_t k = (rr - d2) * 256 / (2 * LAMP_R), a = lv * 7 / 10;   /* (the glow at the rim) */
                    int32_t or_ = br + (tr - br) * a / 256, og = bg_ + (tg - bg_) * a / 256, ob = bb + (tb - bb) * a / 256;
                    r = or_ + (r - or_) * k / 256; g = og + (g - og) * k / 256; b = ob + (b - ob) * k / 256;
                }
            } else {
                /* the glow: (1 - t)^2 of the level, t from the rim (0) to its reach (1) */
                int32_t t = (d2 - rr) * 256 / (gg - rr), f = (256 - t) * (256 - t) / 256, a = f * lv / 255;
                a = a * 7 / 10;                                  /* (a glow, not a second lamp) */
                r = br + (tr - br) * a / 256;
                g = bg_ + (tg - bg_) * a / 256;
                b = bb + (tb - bb) * a / 256;
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

/* the FREE EG page's picture: the recording (128 points over the length), the play head, the DEPTH's base */
static void draw_feg(void)
{
    const int32_t top = 8, bot = PLOT_H - 16, hh = bot - top;
    int32_t i, px = 0, py = 0;
    uint32_t pos = feg.pos, n = sys.feg_len % FEG_NLEN ? FEG_N : (feg.rec ? FEG_N : sys.feg_npts);
    uint16_t c = feg.rec ? T_ACCENT : sys.feg_on ? (FEG_FM(sys.feg_target) ? T_FM : T_THEME) : T_DIM;
    cv_begin(240, PLOT_H, T_BG);
    cv_rect(8, bot, 224, 1, T_LINE);
    cv_rect(8, top, 224, 1, T_LINE);
    if (sys.feg_has || feg.rec)
        for (i = 0; i < (int32_t)n && i < (int32_t)FEG_N; i++) {
            int32_t x = 8 + i * 224 / (int32_t)FEG_N, y = bot - (int32_t)feg.curve[i] * hh / 127;
            if (i && (!feg.rec || i <= (int32_t)pos))
                cv_line(px, py, x, y, c);
            px = x;
            py = y;
        }
    if (pos < FEG_N)
        cv_rect(8 + (int32_t)pos * 224 / (int32_t)FEG_N, top, 1, hh, T_ACCENT);
    cv_text(8, bot + 3, &AF_S, FEG_T_NAME[sys.feg_target % FEG_NT], FEG_FM(sys.feg_target) ? T_FM : T_DIM);
    cv_text_r(232, bot + 3, &AF_S, FEG_LEN_NAME[sys.feg_len % FEG_NLEN], T_DIM, T_BG);
    cv_blit(0, PLOT_Y);
}

/* the KARMA page's picture: the GE's rhythm (each step: a hit's weight its height, a long one wide, ties joined, the
 * step playing lit), under it the last notes made as a roll, the newest bright */
static void draw_karma(void)
{
    const ge_t *G = k_ge();
    const char *r = G->rhy;
    const int32_t rt = 6, rh = 26, top = 42, bot = PLOT_H - 14;
    uint32_t len = str_len(r), i, wv = kar_view.w, lo = 127, hi = 0, cur = kar.view_step;
    int32_t w = 224 / (int32_t)(len ? len : 1u);
    char a[40], v[12];
    cv_begin(240, PLOT_H, T_BG);
    for (i = 0; i < len; i++) {
        char h = r[i];
        int32_t x = 8 + (int32_t)i * w, hh = h == '1' || h == 'L' ? rh : h == '2' ? rh * 2 / 3 : h == '3' ? rh / 3 : 0;
        uint16_t c = i == cur && sys.k_on ? T_ACCENT : sys.k_on ? T_THEME : T_DIM;
        if (h == '-')                            /* a tie: joined to the step before */
            cv_rect(x - 1, rt + rh - 3, w, 3, ux_mix(T_BG, c, 60));
        else if (hh)
            cv_rect(x, rt + rh - hh, h == 'L' ? w * 2 - 1 : w - 1, hh, c);
        else
            cv_rect(x + w / 2 - 1, rt + rh - 2, 2, 2, T_LINE);
    }
    for (i = 0; i < 32u; i++) {
        uint32_t nt = kar_view.note[(wv - 32u + i) & 31u];
        if (nt && wv >= 32u - i) {
            lo = nt < lo ? nt : lo;
            hi = nt > hi ? nt : hi;
        }
    }
    if (hi < lo + 12u)
        hi = lo + 12u;
    cv_rect(8, bot, 224, 1, T_LINE);
    for (i = 0; i < 32u; i++) {
        uint32_t nt = kar_view.note[(wv - 32u + i) & 31u];
        if (nt && wv >= 32u - i) {
            int32_t y = bot - 4 - (int32_t)(nt - lo) * (bot - top - 8) / (int32_t)(hi - lo);
            cv_rect(8 + (int32_t)i * 7, y, 6, 4, i == 31u && sys.k_on ? T_ACCENT :
                    sys.k_on ? ux_mix(T_BG, T_THEME, 40 + (int32_t)i * 2) : T_DIM);
        }
    }
    str_cpy(a, !sys.k_on ? "OFF  " : arp_ext() ? "EXT " : "", sizeof a);
    fmt_int(v, (int32_t)(arp_ext() ? arp_bpm_now() : sys.arp_bpm));
    ui_cat(a, sizeof a, v);
    ui_cat(a, sizeof a, " BPM   ");
    fmt_int(v, (int32_t)len);
    ui_cat(a, sizeof a, v);
    ui_cat(a, sizeof a, G->res == 3u ? " x 1/32" : G->res == 6u ? " x 1/16" : G->res == 8u ? " x 1/8T" :
                        G->res == 12u ? " x 1/8" : " x 1/4");
    cv_text(8, bot + 1, &AF_S, a, T_DIM);
    cv_blit(0, PLOT_Y);
}

/* the PAN page's picture: the stereo field from left to right, BASIS's cycle of places (the next one ringed), the
 * voices sounding where they are, each with its note */
static void draw_pan(void)
{
    const int32_t x0 = 16, w = 208, yc = 58;
    int32_t basis = (int32_t)(sys.basis & 127u), k;
    char nm[8], v[8];
    cv_begin(240, PLOT_H, T_BG);
    cv_rect(x0, yc, w, 1, T_LINE);
    cv_rect(x0 + w / 2, yc - 30, 1, 61, T_LINE);
    cv_text(x0 - 8, yc + 34, &AF_S, "L", T_DIM);
    cv_text_r(x0 + w + 8, yc + 34, &AF_S, "R", T_DIM, T_BG);
    for (k = 0; k < (int32_t)NVOICE; k++) {      /* BASIS: the voices' places (the next note's: tall) */
        int32_t x = x0 + w / 2 + (int32_t)VOICE_PAN[k & 3] * basis / 127 * (w / 2) / 32767;
        int nx = (uint32_t)k == voice_rr;
        cv_rect(x, yc - (nx ? 12 : 6), nx ? 2 : 1, nx ? 25 : 13, nx ? T_MID : ux_mix(T_BG, T_THEME, 50));
    }
    for (k = 0; k < (int32_t)NVOICE; k++)        /* the voices sounding, where they are */
        if (voice[k].stage != ST_OFF && voice[k].stage != ST_TAIL) {
            int32_t x = x0 + w / 2 + vpan[k] * (w / 2) / 32767, y = yc - 26 + k * 17;
            cv_rect(x - 4, y - 4 + 13, 9, 9, T_ACCENT);
            str_cpy(nm, NOTE_NAME[voice[k].note % 12u], sizeof nm);
            fmt_int(v, (int32_t)voice[k].note / 12 - 1);
            ui_cat(nm, sizeof nm, v);
            cv_text(x + 8 > 200 ? x - 40 : x + 8, y + 8, &AF_S, nm, T_MID);
        }
    cv_blit(0, PLOT_Y);
}

/* the EDIT page's picture: the mod wheel now and the parameter it moves, the last velocity and what it does */
static const char *const VEL_TO_NAME[4] = {"OFF", "BRIGHT", "SWEEP", "BRIGHT + SWEEP"};
static void draw_edit(void)
{
    const int32_t x0 = 8, w = 224, h = 14;
    int32_t mw = (int32_t)(sys.mw & 127u), vel = sys.last_vel;
    char a[40];
    cv_begin(240, PLOT_H, T_BG);
    cv_text(x0, 6, &AF_S, "MOD WHEEL (CC 1)", T_DIM);
    cv_frame(x0, 24, w, h, T_LINE);
    if (mw)
        cv_rect(x0 + 1, 25, (w - 2) * mw / 127, h - 2, FEG_FM(sys.mw_target) ? T_FM : T_THEME);
    if (sys.mw_seen && feg.playing && feg.target == sys.mw_target % FEG_NT) {
        str_cpy(a, "FREE EG DEPTH ON ", sizeof a);
        ui_cat(a, sizeof a, FEG_T_NAME[sys.mw_target % FEG_NT]);
    } else {
        str_cpy(a, "MOVES ", sizeof a);
        ui_cat(a, sizeof a, FEG_T_NAME[sys.mw_target % FEG_NT]);
    }
    cv_text(x0, 42, &AF_S, a, FEG_FM(sys.mw_target) ? T_FM : T_MID);
    cv_text(x0, 66, &AF_S, "LAST VELOCITY", T_DIM);
    cv_frame(x0, 84, w, h, T_LINE);
    if (vel && sys.last_note < 128u)
        cv_rect(x0 + 1, 85, (w - 2) * vel / 127, h - 2, T_THEME);
    str_cpy(a, "ON THE SOUND: ", sizeof a);
    ui_cat(a, sizeof a, VEL_TO_NAME[sys.vel_to & 3u]);
    cv_text(x0, 102, &AF_S, a, T_MID);
    cv_blit(0, PLOT_Y);
}

/* the MIDI page's picture: where notes come in and go out, and the clock */
static void draw_midi(void)
{
    char a[40], v[12];
    int32_t y = 6;
    cv_begin(240, PLOT_H, T_BG);
    cv_text(8, y, &AF_S, "IN", T_DIM);
    str_cpy(a, sys.midi_trs ? "TRS JACK" : "TRS JACK (OFF)", sizeof a);
    cv_text(48, y, &AF_S, a, sys.midi_trs ? T_TEXT : T_DIM);
    cv_text(140, y, &AF_S, sys.midi_usb & 1u ? "USB" : "USB (OFF)", sys.midi_usb & 1u ? T_TEXT : T_DIM);
    y += 19;
    str_cpy(a, "CHANNEL ", sizeof a);
    if (sys.midi_rx) {
        fmt_int(v, sys.midi_rx);
        ui_cat(a, sizeof a, v);
    } else
        ui_cat(a, sizeof a, "ANY (OMNI)");
    cv_text(48, y, &AF_S, a, T_MID);
    y += 24;
    cv_text(8, y, &AF_S, "OUT", T_DIM);
    cv_text(48, y, &AF_S, sys.midi_usb & 2u ? "USB: THE KEYS" : "USB (OFF)", sys.midi_usb & 2u ? T_TEXT : T_DIM);
    y += 19;
    str_cpy(a, "CHANNEL ", sizeof a);
    fmt_int(v, (int32_t)(sys.midi_tx & 15u) + 1);
    ui_cat(a, sizeof a, v);
    cv_text(48, y, &AF_S, a, T_MID);
    y += 24;
    cv_text(8, y, &AF_S, arp_ext() ? "CLOCK  MIDI IN" : "CLOCK  INTERNAL", T_DIM);
#if FELUCCA_CPU2
    {   /* the second core: how much of its stack it ever used, its longest job (a diagnostic) */
        str_cpy(a, "2ND CORE  STACK ", sizeof a);
        fmt_int(v, (int32_t)fm1_cpu1_stack_used());
        ui_cat(a, sizeof a, v);
        ui_cat(a, sizeof a, " B  WAIT ");
        fmt_int(v, (int32_t)(fm1_c1_wait_max / 24u));
        ui_cat(a, sizeof a, v);
        ui_cat(a, sizeof a, " us");
        if (sys.cpu2)
            cv_text(8, y + 17, &AF_S, a, T_DIM);
    }
#endif
    cv_blit(0, PLOT_Y);
}

/* the ARP page's picture: the last 32 steps as a roll (their notes' heights, rests empty), the newest bright; ON / OFF
 * and the tempo's beat flashing */
static void draw_arp(void)
{
    const int32_t top = 8, bot = PLOT_H - 14, hh = bot - top, w = 7;
    uint32_t i, wv = arp_view.w, lo = 127, hi = 0;
    cv_begin(240, PLOT_H, T_BG);
    for (i = 0; i < 32u; i++) {
        uint32_t nt = arp_view.note[(wv - 32u + i) & 31u];
        if (nt) {
            lo = nt < lo ? nt : lo;
            hi = nt > hi ? nt : hi;
        }
    }
    if (hi < lo + 12u)                           /* (at least an octave tall) */
        hi = lo + 12u;
    cv_rect(8, bot, 224, 1, T_LINE);
    for (i = 0; i < 32u; i++) {
        uint32_t nt = arp_view.note[(wv - 32u + i) & 31u];
        if (nt && wv >= 32u - i) {
            int32_t y = bot - 4 - (int32_t)(nt - lo) * (hh - 8) / (int32_t)(hi - lo);
            cv_rect(8 + (int32_t)i * w, y, w - 1, 4, i == 31u && sys.arp_on ? T_ACCENT :
                    sys.arp_on ? ux_mix(T_BG, T_THEME, 40 + (int32_t)i * 2) : T_DIM);
        }
    }
    cv_text(8, bot + 1, &AF_S, sys.arp_on ? (arp_ext() ? "MIDI CLOCK" : "INTERNAL") : "OFF: PRESS PLAY", T_DIM);
    cv_blit(0, PLOT_Y);
}

/* the MOD ENV page's picture: the pitch sweep over the envelope's LENGTH (the time axis: 0.5 ms .. 10 s, log), the
 * note the floor, HEIGHT the peak (3 octaves the top) at SKEW, BEND its curve; on the right the newest voice's now */
static void draw_menv(void)
{
    const int32_t top = 10, bot = PLOT_H - 16, hh = bot - top, x0 = 8;
    int32_t xl = x0 + 20 + (int32_t)(sys.menv_len & 127u) * 170 / 127;           /* (the length's end) */
    int32_t xp = x0 + (xl - x0) * (int32_t)(sys.menv_skew & 127u) / 127;          /* the peak */
    int32_t dh = (int32_t)((sys.menv_height & 127u) * (sys.menv_height & 127u));
    int32_t yf = bot, yp = bot - dh * hh / 16129;
    int32_t lv = (int32_t)sys.menv_view * hh / 32767;
    uint32_t on = sys.osc && sys.menv_height;
    uint16_t c = on ? T_FM : T_DIM;
    cv_begin(240, PLOT_H, T_BG);
    cv_rect(x0, bot, 196, 1, T_LINE);
    {   /* the rise and the fall, BEND curving them as noise.c does */
        int32_t i, n = xl - x0, px = x0, py = yf;
        for (i = 1; i <= n; i++) {
            int32_t x = x0 + i, lvq = x <= xp ? (xp > x0 ? (x - x0) * 32768 / (xp - x0) : 32768)
                                               : (xl > xp ? (xl - x) * 32768 / (xl - xp) : 0);
            int32_t y = yf + (yp - yf) * np_bend(lvq, sys.menv_bend) / 32768;
            cv_line_t(px, py, x, y, c, 2);
            px = x;
            py = y;
        }
    }
    cv_line_t(xl, yf, x0 + 196, yf, c, 2);
    cv_frame(212, top, 20, hh + 1, T_LINE);      /* the depth now */
    if (lv > 0)
        cv_rect(214, bot - lv + 1, 16, lv - 1 > 0 ? lv - 1 : 1, on ? T_FM : T_DIM);
    cv_text(x0, bot + 3, &AF_S, "NOTE", T_DIM);
    cv_text_r(xl < 60 ? 60 : xl, bot + 3, &AF_S, "END", T_DIM, T_BG);
    cv_text(200, bot + 3, &AF_S, "PITCH", T_DIM);
    cv_blit(0, PLOT_Y);
}

/* the FX page's SELECT (POSITION) per model: its name, its value */
static void fx_pos_text(char *d, uint32_t n, int brief)
{
    uint32_t md = sys.fx_model % RZ_COUNT, p = sys.fx_pos & 127u;
    char v[12];
    static const char *const L[RZ_COUNT] = {"POSITION", "AMOUNT", "POSITION", "VOWEL", "BREATH", "TWO VOICE", "RASP"};
    static const char *const S[RZ_COUNT] = {"POS", "AMT", "POS", "VOWEL", "BREATH", "TWO", "RASP"};
    str_cpy(d, brief ? S[md] : L[md], n);
    ui_cat(d, n, " ");
    if (md == RZ_THROAT) {                       /* the vowel: U O A E I, or between two */
        uint32_t t = p * 4u * 256u / 127u, k = t >> 8, f = t & 255u;
        k = k > 3u ? 3u : k;
        if (t >= 4u * 256u) { k = 3u; f = 256u; }
        if (f < 40u)
            ui_cat(d, n, WG_VOWEL[k]);
        else if (f > 216u)
            ui_cat(d, n, WG_VOWEL[k + 1u]);
        else {
            ui_cat(d, n, WG_VOWEL[k]);
            ui_cat(d, n, ">");
            ui_cat(d, n, WG_VOWEL[k + 1u]);
        }
        return;
    }
    if (md == RZ_SYRINX) {                       /* the second whistle: semitones above the first */
        uint32_t c = p * 700u / 127u;
        fmt_int(v, (int32_t)(c / 100u));
        ui_cat(d, n, "+");
        ui_cat(d, n, v);
        ui_cat(d, n, ".");
        fmt_int(v, (int32_t)(c / 10u % 10u));
        ui_cat(d, n, v);
        return;
    }
    fmt_int(v, (int32_t)p);
    ui_cat(d, n, v);
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

    ui_val_c = sys.page == 5u ? T_FM : 0;   /* (the MOD ENV: its values green) */
    if (sys.page == 11u) {
        ui_val_c = FEG_FM(sys.mw_target) ? T_FM : 0;
        ui_cell(T_C1, "1 MOD WHEEL", FEG_T_NAME[sys.mw_target % FEG_NT]);
        ui_val_c = 0;
        fmt_int(v, sys.mw_depth);
        ui_cell(T_C2, "2 DEPTH", v);
        ui_cell(T_C3, "3 VELOCITY", VEL_TO_NAME[sys.vel_to & 3u]);
        fmt_int(v, sys.vel_amt);
        ui_cell(T_C4, "4 AMOUNT", sys.vel_to ? v : "-");
    } else if (sys.page == 10u) {
        if (sys.basis)
            fmt_int(v, sys.basis);
        else
            str_cpy(v, "MONO", sizeof v);
        ui_cell(T_C1, "1 BASIS", v);
        if (sys.orbit)
            fmt_int(v, sys.orbit);
        else
            str_cpy(v, "OFF", sizeof v);
        ui_cell(T_C2, "2 ORBIT", v);
        ui_cell(T_C3, "3", "");
        ui_cell(T_C4, "4", "");
    } else if (sys.page == 9u) {
        static const uint8_t CELL[4] = {T_C1, T_C2, T_C3, T_C4};
        const ge_t *G = k_ge();
        for (uint32_t i = 0; i < 4u; i++) {
            str_cpy(a, "1 ", sizeof a);
            a[0] = (char)('1' + i);
            ui_cat(a, sizeof a, MC_NAME[G->knob[i] % MC_COUNT]);
            if (G->knob[i] == MC_TRANSPOSE) {    /* (in octaves) */
                int32_t o = ((int32_t)(sys.k_m[i] & 127u) - 64) / 32 + G->transpose;
                fmt_int(v, o);
                str_cpy(b, o > 0 ? "+" : "", sizeof b);
                ui_cat(b, sizeof b, v);
                ui_cat(b, sizeof b, " OCT");
                ui_cell(CELL[i], a, b);
                continue;
            }
            fmt_int(v, sys.k_m[i] & 127u);
            ui_cell(CELL[i], a, v);
        }
    } else if (sys.page == 8u) {
        fmt_int(v, sys.feg_val);
        ui_cell(T_C1, "1 VALUE", v);
        ui_cell(T_C2, "2 LENGTH", FEG_LEN_NAME[sys.feg_len % FEG_NLEN]);
        fmt_int(v, sys.feg_depth);
        ui_cell(T_C3, "3 DEPTH", v);
        ui_val_c = FEG_FM(sys.feg_target) ? T_FM : 0;   /* (an FM target: green) */
        ui_cell(T_C4, "4 TARGET", FEG_T_NAME[sys.feg_target % FEG_NT]);
        ui_val_c = 0;
    } else if (sys.page == 7u) {
        static const char *const USBN[4] = {"OFF", "IN", "OUT", "IN + OUT"};
        ui_cell(T_C1, "1 TRS MIDI IN", sys.midi_trs ? "ON" : "OFF");
        ui_cell(T_C2, "2 USB MIDI", USBN[sys.midi_usb & 3u]);
        if (sys.midi_rx) {
            fmt_int(v, sys.midi_rx);
            ui_cell(T_C3, "3 RX CHANNEL", v);
        } else
            ui_cell(T_C3, "3 RX CHANNEL", "OMNI");
        fmt_int(v, (int32_t)(sys.midi_tx & 15u) + 1);
        ui_cell(T_C4, "4 TX CHANNEL", v);
    } else if (sys.page == 6u) {
        uint32_t bpm = arp_bpm_now();
        if (arp_ext()) {
            str_cpy(b, "EXT ", sizeof b);
            fmt_int(v, (int32_t)bpm);
            ui_cat(b, sizeof b, bpm ? v : "?");
        } else {
            fmt_int(v, sys.arp_bpm);
            str_cpy(b, v, sizeof b);
        }
        ui_cell(T_C1, "1 BPM", b);
        ui_cell(T_C2, "2 SUBDIVIDE", ARP_DIV_NAME[sys.arp_div % ARP_NDIV]);
        fmt_int(v, (int32_t)(8u + (sys.arp_gate & 127u) * 92u / 127u));
        str_cpy(b, v, sizeof b);
        ui_cat(b, sizeof b, " %");
        ui_cell(T_C3, "3 GATE", b);
        fmt_int(v, (int32_t)(50u + (sys.arp_swing & 127u) * 33u / 127u));
        str_cpy(b, v, sizeof b);
        ui_cat(b, sizeof b, " %");
        ui_cell(T_C4, "4 SWING", b);
    } else if (sys.page == 5u) {
        env_time(b, sizeof b, sys.menv_len);
        ui_cell(T_C1, "1 LENGTH", b);
        {
            uint32_t k = sys.menv_skew & 127u;
            fmt_int(v, (int32_t)(k * 100u / 127u));
            str_cpy(b, k == 0u ? "FALL" : k == 127u ? "RISE" : v, sizeof b);
            if (k && k < 127u)
                ui_cat(b, sizeof b, " %");
        }
        ui_cell(T_C2, "2 SKEW", b);
        if (sys.menv_height) {                       /* (the sweep at the peak, in semitones: HEIGHT^2, 36 the top) */
            uint32_t h = sys.menv_height & 127u;
            fmt_int(v, (int32_t)((h * h * 36u + 8064u) / 16129u));
            str_cpy(b, "+", sizeof b);
            ui_cat(b, sizeof b, v);
            ui_cat(b, sizeof b, " ST");
        } else
            str_cpy(b, "OFF", sizeof b);
        ui_cell(T_C3, "3 HEIGHT", b);
        {
            int32_t bd = (int32_t)(sys.menv_bend & 127u) - 64;
            fmt_int(v, bd);
            str_cpy(b, bd > 0 ? "+" : "", sizeof b);
            ui_cat(b, sizeof b, bd ? v : "STRAIGHT");
            ui_cell(T_C4, "4 BEND", b);
        }
    } else if (sys.page == 0u && sys.osc) {
        uint32_t alg = (sys.osc - 1u) % NP_COUNT;
        str_cpy(a, "1 ", sizeof a);
        ui_cat(a, sizeof a, NP_X[alg]);
        fmt_int(v, sys.np_x);
        ui_val_c = np_is_fm(NP_X[alg]) ? T_FM_IN : 0;   /* (the program's own FM: orange) */
        ui_cell(T_C1, a, v);
        str_cpy(a, "2 ", sizeof a);
        ui_cat(a, sizeof a, NP_Y[alg]);
        fmt_int(v, sys.np_y);
        ui_val_c = np_is_fm(NP_Y[alg]) ? T_FM_IN : 0;
        ui_cell(T_C2, a, v);
        ui_val_c = 0;
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
        static const char *const K1[RZ_COUNT] = {"1 STRUCTURE", "1 CHORD", "1 STRUCTURE", "1 ANIMAL", "1 JET", "1 CHIRP", "1 SCREAM"};
        if (md == RZ_SYMP)
            str_cpy(b, RZ_CHORD_NAME[(sys.fx_struct & 127u) >> 4], sizeof b);
        else if (md == RZ_THROAT)
            str_cpy(b, WG_ANIMAL[(sys.fx_struct & 127u) * WA_COUNT / 128u], sizeof b);
        else {
            fmt_int(v, sys.fx_struct);
            str_cpy(b, v, sizeof b);
        }
        ui_cell(T_C1, K1[md], b);
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
            ui_cat(a, sizeof a, "   ");
            fx_pos_text(b, sizeof b, 1);
            ui_cat(a, sizeof a, b);
        } else if (sys.page == 9u) {             /* KARMA: on or off, the GE, LATCH */
            str_cpy(a, sys.k_on ? "KARMA ON  " : "KARMA OFF  ", sizeof a);
            ui_cat(a, sizeof a, k_ge()->name);
            if (sys.k_latch)
                ui_cat(a, sizeof a, "  LATCH");
        } else if (sys.page == 8u) {             /* FREE EG: recording, playing, or what to do */
            str_cpy(a, feg.rec ? "FREE EG   RECORDING: TURN KNOB 1" : !sys.feg_has ? "FREE EG   REC: RECORD" :
                    sys.feg_on ? "FREE EG   PLAYING" : "FREE EG   SELECT: PLAY", sizeof a);
        } else if (sys.page == 7u) {             /* MIDI: the second core (SELECT) */
            static const char *const C2[4] = {"MIDI   2ND CORE OFF", "MIDI   2ND CORE ALL", "MIDI   2ND CORE VOICES",
                                              "MIDI   2ND CORE FX"};
            str_cpy(a, C2[sys.cpu2 & 3u], sizeof a);
        } else if (sys.page == 6u) {             /* ARP: on or off, the pattern, HOLD */
            str_cpy(a, sys.arp_on ? "ARP ON  " : "ARP OFF  ", sizeof a);
            ui_cat(a, sizeof a, arp_type_name(sys.arp_type));
            if (sys.arp_hold)
                ui_cat(a, sizeof a, "  HOLD");
        } else if (sys.page == 5u) {             /* MOD ENV: on what */
            str_cpy(a, "MOD ENV   ", sizeof a);
            if (!sys.osc)
                ui_cat(a, sizeof a, "CHUA: NOISE ONLY");
            else {
                osc_code(v, sizeof v, sys.osc);
                ui_cat(a, sizeof a, v);
                ui_cat(a, sizeof a, " PITCH");
            }
        } else if (sys.page) {
            str_cpy(a, "BASIS ", sizeof a);
            if (sys.basis)
                fmt_int(v, sys.basis);
            else
                str_cpy(v, "MONO", sizeof v);
            ui_cat(a, sizeof a, v);
            ui_cat(a, sizeof a, "  ORBIT ");
            if (sys.orbit)
                fmt_int(v, sys.orbit);
            else
                str_cpy(v, "OFF", sizeof v);
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
    else if (sys.page == 5u)
        draw_menv();
    else if (sys.page == 6u)
        draw_arp();
    else if (sys.page == 7u)
        draw_midi();
    else if (sys.page == 8u)
        draw_feg();
    else if (sys.page == 9u)
        draw_karma();
    else if (sys.page == 10u)
        draw_pan();
    else if (sys.page == 11u)
        draw_edit();
    else
        draw_plot();
    ui_force = 0;
}

/* the splash's 8-bit letters: 7 x 7 cells, strokes two cells wide (rows, bit 6 the left cell) */
static const struct { char ch; uint8_t row[7]; } PIX_FONT[] = {
    {'A', {0x3E, 0x63, 0x63, 0x7F, 0x63, 0x63, 0x63}}, {'C', {0x3E, 0x63, 0x60, 0x60, 0x60, 0x63, 0x3E}},
    {'E', {0x7F, 0x60, 0x60, 0x7E, 0x60, 0x60, 0x7F}}, {'H', {0x63, 0x63, 0x63, 0x7F, 0x63, 0x63, 0x63}},
    {'I', {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E}}, {'K', {0x63, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0x63}},
    {'L', {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7F}}, {'P', {0x7E, 0x63, 0x63, 0x7E, 0x60, 0x60, 0x60}},
    {'S', {0x3F, 0x60, 0x60, 0x3E, 0x03, 0x03, 0x7E}}, {'U', {0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x3E}},
    {'W', {0x63, 0x63, 0x6B, 0x6B, 0x7F, 0x77, 0x63}},
};

/* s in those letters, cells of c px, centred at screen row y: an orange outline, no fill, a white glint inside each
 * top left corner (the highlight) */
static void pix_text(const char *s, uint32_t y, int32_t c, int32_t gap)
{
    const uint8_t *g[16];
    int32_t n = 0, l, r, k, t = c >= 6 ? 2 : 1, w, x0;
    for (; *s && n < 16; s++) {
        uint32_t i;
        for (i = 0; i < sizeof PIX_FONT / sizeof PIX_FONT[0]; i++)
            if (PIX_FONT[i].ch == *s)
                g[n++] = PIX_FONT[i].row;
    }
    w = n * 7 * c + (n - 1) * gap;
    x0 = (240 - w) / 2;
    cv_begin(240, (uint32_t)(7 * c), T_BG);
    for (l = 0; l < n; l++)
        for (r = 0; r < 7; r++)
            for (k = 0; k < 7; k++) {
                int32_t x = x0 + l * (7 * c + gap) + k * c, yy = r * c;
#define ON(rr, kk) ((rr) >= 0 && (rr) < 7 && (kk) >= 0 && (kk) < 7 && (g[l][rr] >> (6 - (kk)) & 1u))
                if (!ON(r, k))
                    continue;
                if (!ON(r - 1, k))                      /* the outline: the edges facing an empty cell */
                    cv_rect(x, yy, c, t, T_THEME);
                if (!ON(r + 1, k))
                    cv_rect(x, yy + c - t, c, t, T_THEME);
                if (!ON(r, k - 1))
                    cv_rect(x, yy, t, c, T_THEME);
                if (!ON(r, k + 1))
                    cv_rect(x + c - t, yy, t, c, T_THEME);
                if (!ON(r - 1, k - 1) && ON(r - 1, k) && ON(r, k - 1))   /* (inner corners closed) */
                    cv_rect(x, yy, t, t, T_THEME);
                if (!ON(r - 1, k + 1) && ON(r - 1, k) && ON(r, k + 1))
                    cv_rect(x + c - t, yy, t, t, T_THEME);
                if (!ON(r + 1, k - 1) && ON(r + 1, k) && ON(r, k - 1))
                    cv_rect(x, yy + c - t, t, t, T_THEME);
                if (!ON(r + 1, k + 1) && ON(r + 1, k) && ON(r, k + 1))
                    cv_rect(x + c - t, yy + c - t, t, t, T_THEME);
                if (!ON(r, k - 1) && !ON(r - 1, k)) {   /* the highlight: a white glint inside the letter, in its */
                    int32_t hl = c > 2 * t ? c : 1, ht = t > 1 ? t - 1 : 1;   /* top left corners, along the stroke */
                    cv_rect(x + t, yy + t, ON(r, k + 1) ? hl : c - 2 * t, ht, T_TEXT);
                    cv_rect(x + t, yy + t, ht, ON(r + 1, k) ? hl : c - 2 * t, T_TEXT);
                }
#undef ON
            }
    cv_blit(0, y);
}

/* the splash: HAWK, CHUA smaller, what it is, PLIEUWSKI, all in the 8-bit letters */
static void draw_splash(void)
{
    lcd_fill(0, 0, 240, 240, T_BG);
    pix_text("HAWK", 30, 7, 8);
    pix_text("CHUA", 96, 4, 5);
    draw_text_box(0, 138, 240, &AF_S, "MODELING SYNTHESIZER", T_MID, 1);
    pix_text("PLIEUWSKI", 166, 3, 3);
    draw_text_box(0, 216, 240, &AF_S, UI_VERSION, T_DIM, 1);
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
    if (sys.page == 1u || sys.page == 5u)
        led_on(nl, panel.btn[B_ENV]);
    if (sys.arp_on || sys.page == 6u)
        led_on(nl, panel.btn[B_ARP]);
    if (sys.page == 7u)
        led_on(nl, panel.btn[B_GLO]);
    if (sys.page == 10u)
        led_on(nl, panel.btn[B_SCL]);
    if (sys.page == 11u)
        led_on(nl, panel.btn[B_EDIT]);
    if (sys.k_on || sys.page == 9u)
        led_on(nl, panel.btn[B_SEQ]);
    if (sys.arp_on || sys.k_on) {                       /* PLAY: lit (and its green) while the ARP or KARMA runs */
        led_on(nl, panel.btn[B_PLAY]);
        nl[LED_PLAY_GREEN_COL] |= (uint8_t)(1u << LED_PLAY_GREEN_ROW);
    }
    if (sys.page == 8u || (feg.rec && (fm1_ms & 256u)) || (sys.feg_on && sys.feg_has))   /* (blinking: recording) */
        led_on(nl, panel.btn[B_REC]);
    if ((sys.page >= 2u && sys.page <= 3u) || sys.fx_mix || sys.sh_mix)   /* (FX lit: its pages, or FX on) */
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
/* a knob turned on (the same way within 40 ms) speeds up to x4: 0 .. 127 in a flick, one detent still one step
 * (after M-VAVE's fm1_open, app/screen.c read_encoders); every knob and encoder (ui_enc), lists too */
static uint32_t enc_t[NE];
static int8_t enc_dir[NE];
static uint8_t enc_speed[NE];
static int32_t knob_accel(uint32_t role, int32_t s)
{
    int8_t d = (int8_t)(s > 0 ? 1 : -1);
    enc_speed[role] = (uint8_t)(d == enc_dir[role] && fm1_ms - enc_t[role] <= 40u ?
                                (enc_speed[role] < 4u ? enc_speed[role] + 1u : 4u) : 1u);
    enc_dir[role] = d;
    enc_t[role] = fm1_ms;
    return s * enc_speed[role];
}

static int32_t ui_enc(uint32_t role)            /* every knob and encoder: detents, accelerated */
{
    int32_t s = panel_enc(role);
    return s ? knob_accel(role, s) : 0;
}

static void knob(volatile uint8_t *p, uint32_t role, uint32_t cell)
{
    int32_t s = ui_enc(role);
    if (!s)
        return;
    *p = (uint8_t)clamp(*p + s, 0, 127);
    if (cell < 4u) {
        ui_hot = (uint8_t)cell;
        ui_hot_ms = fm1_ms;
    }
}

static uint8_t play_last;                        /* PLAY outside the ARP / KARMA pages: 0 the ARP, 1 KARMA (the last opened) */
static void ui_input(void)
{
    uint32_t pressed = fm1_input_edges(0), b;
    int32_t s;
    char t[24], v[12];
    if (keys_prog_req) {                                /* MIDI program change: that preset */
        uint32_t pg = keys_prog_req - 1u;
        keys_prog_req = 0;
        preset_load(pg);
        str_cpy(t, PRESETS[sys.preset % NPRESETS].name, sizeof t);
        ui_say(t);
    }
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
        case B_HOME:                                    /* the oscillator's page */
            sys.page = 0;
            ui_force = 1;
            break;
        case B_ENV:                                     /* the ENV page, the MOD ENV page, back HOME */
            sys.page = sys.page == 1u ? 5u : sys.page == 5u ? 0u : 1u;
            ui_force = 1;
            break;
        case B_FX:                                      /* the resonator, the shimmer, back HOME */
            sys.page = sys.page == 2u ? 3u : sys.page == 3u ? 0u : 2u;
            ui_force = 1;
            break;
        case B_REC:                                     /* the FREE EG page; there: record (and stop) */
            if (sys.page == 8u) {
                feg_req = feg.rec ? 2u : 1u;            /* (done by the audio ISR, at its next block) */
                ui_say(feg.rec ? "RECORDED" : "RECORDING");
            } else {
                sys.page = 8;
                ui_force = 1;
            }
            break;
        case B_ARP:                                     /* the ARP page (again: back HOME); PLAY starts it */
            sys.page = sys.page == 6u ? 0u : 6u;
            play_last = 0;
            ui_force = 1;
            break;
        case B_SEQ:                                     /* the KARMA page (again: back HOME); PLAY starts it */
            sys.page = sys.page == 9u ? 0u : 9u;
            play_last = 1;
            ui_force = 1;
            break;
        case B_PLAY:                                    /* start / stop: the ARP or KARMA (the page's, else the last
                                                         * one opened); what runs stops (the other one off) */
            if (sys.page == 6u || (sys.page != 9u && !play_last && !sys.k_on)) {
                sys.arp_on = (uint8_t)!sys.arp_on;
                ui_say(sys.arp_on ? (sys.k_on ? "ARP ON, KARMA OFF" : "ARP ON") : "ARP OFF");
            } else if (sys.page == 9u || (play_last && !sys.arp_on)) {
                sys.k_on = (uint8_t)!sys.k_on;
                ui_say(sys.k_on ? (sys.arp_on ? "KARMA ON, ARP OFF" : "KARMA ON") : "KARMA OFF");
            } else {                                    /* (elsewhere, the other one running: it stops) */
                sys.arp_on = sys.k_on = 0;
                ui_say("STOP");
            }
            break;
        case B_EDIT:                                    /* the EDIT page (mod wheel, velocity) and back */
            sys.page = sys.page == 11u ? 0u : 11u;
            ui_force = 1;
            break;
        case B_SCL:                                     /* the PAN page (BASIS, ORBIT) and back */
            sys.page = sys.page == 10u ? 0u : 10u;
            ui_force = 1;
            break;
        case B_GLO:                                     /* the MIDI page and back */
            sys.page = sys.page == 7u ? 0u : 7u;
            ui_force = 1;
            break;
        default:
            str_cpy(t, "BUTTON ", sizeof t);
            ui_cat(t, sizeof t, B_NAME[b]);
            ui_cat(t, sizeof t, ": FREE");
            ui_say(t);
            break;
        }
    }
    if (sys.page == 11u) {
        if ((s = ui_enc(EN_K1)) != 0) {
            sys.mw_target = (uint8_t)clamp((int32_t)sys.mw_target + s, 0, FEG_NT - 1);
            ui_hot = 0; ui_hot_ms = fm1_ms;
        }
        knob(&sys.mw_depth, EN_K2, 1);
        if ((s = ui_enc(EN_K3)) != 0) {
            sys.vel_to = (uint8_t)clamp((int32_t)sys.vel_to + (s > 0 ? 1 : -1), 0, 3);
            ui_hot = 2; ui_hot_ms = fm1_ms;
        }
        knob(&sys.vel_amt, EN_K4, 3);
    } else if (sys.page == 10u) {
        knob(&sys.basis, EN_K1, 0);
        knob(&sys.orbit, EN_K2, 1);
    } else if (sys.page == 9u) {
        knob(&sys.k_m[0], EN_K1, 0);
        knob(&sys.k_m[1], EN_K2, 1);
        knob(&sys.k_m[2], EN_K3, 2);
        knob(&sys.k_m[3], EN_K4, 3);
    } else if (sys.page == 8u) {
        knob(&sys.feg_val, EN_K1, 0);
        if ((s = ui_enc(EN_K2)) != 0) {
            sys.feg_len = (uint8_t)clamp((int32_t)sys.feg_len + s, 0, FEG_NLEN - 1);
            ui_hot = 1; ui_hot_ms = fm1_ms;
        }
        knob(&sys.feg_depth, EN_K3, 2);
        if ((s = ui_enc(EN_K4)) != 0) {
            sys.feg_target = (uint8_t)clamp((int32_t)sys.feg_target + s, 0, FEG_NT - 1);
            ui_hot = 3; ui_hot_ms = fm1_ms;
        }
    } else if (sys.page == 7u) {                /* MIDI: an input changed: what it held let go (no note hangs) */
        if ((s = ui_enc(EN_K1)) != 0) {
            sys.midi_trs = (uint8_t)(s > 0);
            keys_panic_req = 1;
            ui_hot = 0; ui_hot_ms = fm1_ms;
        }
        if ((s = ui_enc(EN_K2)) != 0) {
            sys.midi_usb = (uint8_t)clamp((int32_t)sys.midi_usb + s, 0, 3);
            keys_panic_req = 1;
            ui_hot = 1; ui_hot_ms = fm1_ms;
        }
        if ((s = ui_enc(EN_K3)) != 0) {
            sys.midi_rx = (uint8_t)clamp((int32_t)sys.midi_rx + s, 0, 16);
            keys_panic_req = 1;
            ui_hot = 2; ui_hot_ms = fm1_ms;
        }
        if ((s = ui_enc(EN_K4)) != 0) {
            sys.midi_tx = (uint8_t)clamp((int32_t)sys.midi_tx + s, 0, 15);
            ui_hot = 3; ui_hot_ms = fm1_ms;
        }
    } else if (sys.page == 6u) {
        if ((s = ui_enc(EN_K1)) != 0) {
            sys.arp_bpm = (uint8_t)clamp((int32_t)sys.arp_bpm + s, 40, 250);
            ui_hot = 0; ui_hot_ms = fm1_ms;
        }
        if ((s = ui_enc(EN_K2)) != 0) {
            sys.arp_div = (uint8_t)clamp((int32_t)sys.arp_div + s, 0, ARP_NDIV - 1);
            ui_hot = 1; ui_hot_ms = fm1_ms;
        }
        knob(&sys.arp_gate, EN_K3, 2);
        knob(&sys.arp_swing, EN_K4, 3);
    } else if (sys.page == 5u) {
        knob(&sys.menv_len, EN_K1, 0);
        knob(&sys.menv_skew, EN_K2, 1);
        knob(&sys.menv_height, EN_K3, 2);
        knob(&sys.menv_bend, EN_K4, 3);
    } else if (sys.page == 0u && sys.osc) {
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
    if ((s = ui_enc(EN_SELECT)) != 0 && sys.page == 0u) {   /* HOME: SELECT turns the view */
        sys.view = (uint8_t)((sys.view + V_COUNT * 8 + s) % V_COUNT);
        str_cpy(t, "VIEW ", sizeof t);
        ui_cat(t, sizeof t, VIEW_NAME[sys.view]);
        ui_say(t);
    } else if (s && sys.page == 9u) {           /* KARMA: LATCH (right on, left off) */
        sys.k_latch = (uint8_t)(s > 0);
        ui_say(sys.k_latch ? "LATCH ON" : "LATCH OFF");
    } else if (s && sys.page == 8u) {           /* FREE EG: PLAY (right on, left off) */
        sys.feg_on = (uint8_t)(s > 0);
        ui_say(sys.feg_on ? (sys.feg_has ? "PLAY ON" : "NOTHING RECORDED") : "PLAY OFF");
    } else if (s && sys.page == 7u) {           /* MIDI: 2ND CORE: OFF, ALL, VOICES only, FX only (saved at once) */
        static const char *const C2[4] = {"2ND CORE OFF", "2ND CORE ALL", "2ND CORE VOICES", "2ND CORE FX"};
        uint8_t was = sys.cpu2;
        sys.cpu2 = (uint8_t)clamp((int32_t)sys.cpu2 + (s > 0 ? 1 : -1), 0, 3);
        if (sys.cpu2 != was)
            settings_save();
        str_cpy(t, C2[sys.cpu2 & 3u], sizeof t);
        ui_say(!was != !sys.cpu2 ? (sys.cpu2 ? "2ND CORE ON: RESTART" : "2ND CORE OFF: RESTART") : t);
    } else if (s && sys.page == 6u) {           /* ARP: HOLD (right on, left off) */
        sys.arp_hold = (uint8_t)(s > 0);
        ui_say(sys.arp_hold ? "HOLD ON" : "HOLD OFF");
    } else if (s && (sys.page == 2u || sys.page == 3u)) {   /* (MOD, MOD ENV: nothing; RING is on HOME) */
        volatile uint8_t *p = sys.page == 3u ? &sys.sh_tone : &sys.fx_pos;
        *p = (uint8_t)clamp(*p + s, 0, 127);
        if (sys.page == 3u) {
            str_cpy(t, "TONE ", sizeof t);
            fmt_int(v, *p);
            ui_cat(t, sizeof t, v);
        } else
            fx_pos_text(t, sizeof t, 0);
        ui_say(t);
    }
    if ((s = ui_enc(EN_ALGO)) != 0 && sys.page == 3u) {
        sys.sh_interval = (uint8_t)((sys.sh_interval + SI_COUNT * 8 + s) % SI_COUNT);
        str_cpy(t, "INTERVAL ", sizeof t);
        ui_cat(t, sizeof t, SH_INTERVAL_NAME[sys.sh_interval]);
        ui_say(t);
    } else if (s && sys.page == 2u) {
        sys.fx_model = (uint8_t)((sys.fx_model + RZ_COUNT * 8 + s) % RZ_COUNT);
        str_cpy(t, "MODEL ", sizeof t);
        ui_cat(t, sizeof t, RZ_NAME[sys.fx_model]);
        ui_say(t);
    } else if (s && sys.page == 9u) {           /* KARMA: the GE */
        sys.k_ge = (uint8_t)(((int32_t)sys.k_ge + (int32_t)KGE_N * 8 + s) % (int32_t)KGE_N);
        str_cpy(t, "GE ", sizeof t);
        ui_cat(t, sizeof t, k_ge()->name);
        ui_say(t);
    } else if (s && sys.page == 6u) {           /* ARP: the pattern */
        sys.arp_type = (uint8_t)(((int32_t)sys.arp_type + (int32_t)ARP_NTYPE * 8 + s) % (int32_t)ARP_NTYPE);
        str_cpy(t, "ARP ", sizeof t);
        ui_cat(t, sizeof t, arp_type_name(sys.arp_type));
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
    if ((s = ui_enc(EN_PRESET)) != 0 && sys.page == 9u) {   /* KARMA: PRESETS is the tempo (the ARP page's BPM) */
        sys.arp_bpm = (uint8_t)clamp((int32_t)sys.arp_bpm + s, 40, 250);
        str_cpy(t, arp_ext() ? "MIDI CLOCK: BPM " : "BPM ", sizeof t);
        fmt_int(v, sys.arp_bpm);
        ui_cat(t, sizeof t, v);
        ui_say(t);
    } else if (s && sys.page == 2u) {                   /* FX: PRESETS goes through the FX presets */
        const fx_preset_t *f;
        fx_pre = (uint8_t)(((int32_t)fx_pre + (int32_t)NFX_PRESETS * 8 + s) % (int32_t)NFX_PRESETS);
        f = &FX_PRESETS[fx_pre];
        sys.fx_mix = 0;                                 /* (the model changed silent, then its knobs, then heard) */
        sys.fx_model = f->model; sys.fx_struct = f->st; sys.fx_bright = f->br;
        sys.fx_damp = f->dm; sys.fx_pos = f->pos;
        sys.fx_mix = f->mix;
        str_cpy(t, "FX: ", sizeof t);
        ui_cat(t, sizeof t, f->name);
        ui_say(t);
        ui_force = 1;
    } else if (s && sys.page == 3u) {                   /* SHIMMER: through the shimmer presets */
        const sh_preset_t *h;
        sh_pre = (uint8_t)(((int32_t)sh_pre + (int32_t)NSH_PRESETS * 8 + s) % (int32_t)NSH_PRESETS);
        h = &SH_PRESETS[sh_pre];
        sys.sh_size = h->size; sys.sh_decay = h->decay; sys.sh_shimmer = h->shimmer;
        sys.sh_interval = h->interval; sys.sh_tone = h->tone; sys.sh_mix = h->mix;
        str_cpy(t, "SHIMMER: ", sizeof t);
        ui_cat(t, sizeof t, h->name);
        ui_say(t);
        ui_force = 1;
    } else if (s)
        preset_load((uint32_t)((int32_t)sys.preset + (int32_t)NPRESETS * 8 + s));
}
