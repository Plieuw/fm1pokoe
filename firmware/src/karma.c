/* SPDX-License-Identifier: GPL-3.0-only */
/* KARMA (the SEQ button): a phrase generator after Korg's KARMA (Kay Algorithmic Realtime Music Architecture, Stephen
 * Kay; its Basic Guide): the notes you hold are not played but feed a Generated Effect (GE) that makes a phrase of
 * them, in time, and keeps making it while you hold them (or LATCH). This firmware's own engine and GEs: the
 * architecture is KARMA's, the code and the phrases are new.
 *
 * A GE is several small generators working together (as KARMA's are), each set by the GE and moved by the four
 * realtime knobs (KARMA's Realtime Controls: each GE names what its knobs do):
 *   RHYTHM     a pattern of steps (hits of three weights, rests, ties, long notes) at a resolution; DENSITY thins it
 *              to its strongest hits or fills it with ghosts and new notes in the rests
 *   NOTE SERIES the notes the phrase is made of: the chord held, the chord and its octave, the scale it implies
 *              (major / minor, diatonic or pentatonic), over a RANGE of octaves
 *   INDEX      the path through the series: up, down, up & down, random, a random walk, a spiral (low, high, in), or a
 *              Direct Index pattern (KARMA's: each step names its note)
 *   CLUSTER    notes a hit (a chord stab, a strum: STRUM spreads them down, then up, as a guitar)
 *   MELODIC REPEAT  each note echoed, transposed (KARMA's: cascades, delays in pitch)
 *   VELOCITY   an accent pattern, scaled (ACCENT), with some chance
 *   DURATION   a gate per GE (GATE), ties, long notes
 *   PAN        each note placed (alternate, random, a sweep over the phrase, by pitch: low left), through BASIS
 *   ENV        a shape over the phrase (KARMA's Envelopes) on a parameter of the sound (the ring, the brightness, the
 *              circuit's CHAOS ...), as FREE EG does, given back when it stops
 *
 * KARMA page   ALGORITHM the GE, KNOB 1..4 its four realtime controls (named per GE), SELECT LATCH; SEQ (on the
 *              page) KARMA on / off. The tempo is the instrument's one: the ARP page's BPM, or MIDI clock while it
 *              comes in (START restarts the phrase), the ARP's SWING its groove too.
 * With the ARP: one of them takes the notes held: KARMA on turns the ARP off (and the other way round), the notes
 * held move over. With FREE EG: its LENGTH PHRASE loops with KARMA's phrase (each trigger restarts it), and its
 * TARGETs include the four KARMA knobs: a recorded movement of DENSITY or REPEAT over bars, phrase-locked.
 * The phrase's notes go out on USB MIDI (if on: the GLO page), as KARMA sends what it generates.
 *
 *   karma_add(note, vel), karma_remove(note)   keys.c: a note held, let go (with KARMA on)
 *   karma_block(n)                           keys.c: once a block (n samples), after the ARP */

/* ------------------------------------------------------------------- the GEs --- */
enum { NS_CHORD, NS_OCT, NS_SCALE, NS_PENTA };                     /* note series */
enum { IX_UP, IX_DOWN, IX_UPDOWN, IX_RANDOM, IX_WALK, IX_SPIRAL, IX_DIRECT };   /* index */
enum { PN_NONE, PN_ALT, PN_RANDOM, PN_SWEEP, PN_PITCH };           /* pan */
enum { EV_NONE, EV_UP, EV_DOWN, EV_TRI };                          /* env shapes over the phrase */
enum { ET_RING, ET_BRIGHT, ET_CHAOS, ET_OUTPUT, ET_FXMIX, ET_SHMIX, ET_NOISEX, ET_MODDEPTH };   /* env targets */
enum { TR_ANY, TR_AKR };                                           /* trigger: any new note, all keys released first */
/* the realtime controls (what a knob of a GE does; 64 is the GE as written) */
enum { MC_DENSITY, MC_RANGE, MC_REPEAT, MC_ACCENT, MC_GATE, MC_RANDOM, MC_CLUSTER, MC_TRANSPOSE, MC_ENV, MC_STRUM,
       MC_COUNT };
static const char *const MC_NAME[MC_COUNT] = {"DENSITY", "RANGE", "REPEAT", "ACCENT", "GATE", "RANDOM", "CLUSTER",
                                              "TRANSPOSE", "ENV", "STRUM"};
typedef struct {
    const char *name;
    uint8_t res;                                 /* MIDI clocks a step (24 a quarter: 6 a sixteenth, 8 an eighth triplet) */
    const char *rhy;                             /* the steps: '.' rest, '1' hit, '2' a lighter one, '3' a ghost (only
                                                  * when DENSITY is up), 'L' a long hit, '-' a tie (the last goes on) */
    const char *vel;                             /* per step 0..9 (5 normal, 9 an accent; NULL: all 6) */
    const char *clu;                             /* per step 1..4 notes a hit (NULL: all 1) */
    const char *idx;                             /* IX_DIRECT: per step 0..9 the series' place, 'h' highest, 'l' lowest,
                                                  * 'r' random, '^' the next up, 'v' the next down */
    const char *oct;                             /* per step octave shift '0'..'3', '-' one down (NULL: none) */
    uint8_t series, range, index;
    uint8_t rep_n;                               /* melodic repeat: echoes */
    int8_t rep_iv;                               /* each echo transposed, semitones */
    uint8_t rep_dly, rep_dec;                    /* steps between echoes; level kept per echo, % */
    uint8_t gate, strum;                         /* % of a step; ms between a strum's notes */
    uint8_t pan, env, env_tgt, env_depth;
    int8_t transpose;                            /* octaves */
    uint8_t trig;
    uint8_t knob[4];                             /* its realtime controls */
} ge_t;

static const ge_t KGE[] = {
    {"PULSE ARP", 6, "1213121312131213", "9535753595357535", 0, 0, 0, NS_CHORD, 2, IX_UPDOWN, 0, 12, 2, 50, 50, 0,
     PN_ALT, EV_NONE, 0, 0, 0, TR_AKR, {MC_DENSITY, MC_RANGE, MC_REPEAT, MC_ACCENT}},
    {"CASCADE", 6, "1..21..21.21..2.", "9..69..69.79..6.", 0, 0, 0, NS_OCT, 3, IX_DOWN, 3, -5, 2, 70, 60, 0,
     PN_SWEEP, EV_NONE, 0, 0, 0, TR_AKR, {MC_DENSITY, MC_REPEAT, MC_RANGE, MC_GATE}},
    {"HARP GLISS", 3, "11111111111111112.......3.......", "33445566778899996.......3.......", 0, 0, 0, NS_SCALE, 3,
     IX_UP, 0, 0, 1, 50, 85, 0, PN_PITCH, EV_NONE, 0, 0, 0, TR_AKR, {MC_DENSITY, MC_RANGE, MC_GATE, MC_ACCENT}},
    {"STRUM GUITAR", 6, "1.2.1-2.1.2.1-2.", "9.5.7.5.9.5.7.5.", "4444444444444444", 0, 0, NS_CHORD, 1, IX_UP, 0, 0,
     1, 50, 40, 12, PN_NONE, EV_NONE, 0, 0, 0, TR_ANY, {MC_DENSITY, MC_STRUM, MC_ACCENT, MC_GATE}},
    {"FINGERPICK", 12, "11121112", "86568656", 0, "04150415", "--00--00", NS_CHORD, 2, IX_DIRECT, 0, 0, 1, 50, 90, 0,
     PN_PITCH, EV_NONE, 0, 0, 0, TR_AKR, {MC_DENSITY, MC_RANGE, MC_ACCENT, MC_GATE}},
    {"TECHNO STAB", 6, "..1...1...1..2.1", "..9...8...9..6.8", "3333333333333333", 0, 0, NS_CHORD, 1, IX_UP, 0, 0, 1,
     50, 25, 0, PN_ALT, EV_TRI, ET_RING, 90, 0, TR_AKR, {MC_DENSITY, MC_GATE, MC_ENV, MC_TRANSPOSE}},
    {"ACID LINE", 6, "1.21-1.21.1-2.12", "9.59-5.95.9-5.95", 0, "0.0h-0.2l.0-^.0v", "0.01-0.00.0-0.01", NS_CHORD, 1,
     IX_DIRECT, 0, 0, 1, 50, 70, 0, PN_NONE, EV_TRI, ET_RING, 110, -1, TR_AKR, {MC_DENSITY, MC_ACCENT, MC_RANDOM,
     MC_ENV}},
    {"BASS WALK", 24, "1111", "8676", 0, 0, 0, NS_SCALE, 1, IX_WALK, 0, 0, 1, 50, 90, 0, PN_NONE, EV_NONE, 0, 0, -1,
     TR_AKR, {MC_DENSITY, MC_RANGE, MC_RANDOM, MC_GATE}},
    {"OCTAVE BASS", 12, "11111111", "96869686", 0, "00000000", "01010101", NS_CHORD, 1, IX_DIRECT, 0, 0, 1, 50, 45, 0,
     PN_NONE, EV_NONE, 0, 0, -1, TR_AKR, {MC_DENSITY, MC_ACCENT, MC_GATE, MC_TRANSPOSE}},
    {"TRANCE GATE", 6, "1.1.11.11.1.1.11", "9.6.86.96.7.6.86", "4444444444444444", 0, 0, NS_CHORD, 1, IX_UP, 0, 0, 1,
     50, 40, 0, PN_ALT, EV_UP, ET_RING, 80, 0, TR_AKR, {MC_DENSITY, MC_GATE, MC_ENV, MC_ACCENT}},
    {"RAIN DROPS", 6, "3.2..3.1..2.3..2", "4.6..3.8..5.3..6", 0, 0, 0, NS_PENTA, 3, IX_RANDOM, 2, 12, 3, 45, 60, 0,
     PN_RANDOM, EV_NONE, 0, 0, 1, TR_ANY, {MC_DENSITY, MC_REPEAT, MC_RANGE, MC_RANDOM}},
    {"SPIRAL", 6, "1212121212121212", "9585758595857585", 0, 0, 0, NS_OCT, 3, IX_SPIRAL, 0, 0, 1, 50, 55, 0,
     PN_PITCH, EV_NONE, 0, 0, 0, TR_AKR, {MC_DENSITY, MC_RANGE, MC_REPEAT, MC_ACCENT}},
    {"BROKEN 6/8", 8, "1-2121-21212", "9-5758-57575", 0, "012102012102", 0, NS_CHORD, 2, IX_DIRECT, 0, 0, 1, 50, 80, 0,
     PN_PITCH, EV_NONE, 0, 0, 0, TR_AKR, {MC_DENSITY, MC_RANGE, MC_GATE, MC_ACCENT}},
    {"GAMELAN", 6, "1111111111111111", "9595959595959595", 0, "0314031425142514", 0, NS_PENTA, 2, IX_DIRECT, 1, 12,
     1, 35, 60, 0, PN_ALT, EV_NONE, 0, 0, 1, TR_AKR, {MC_DENSITY, MC_RANGE, MC_REPEAT, MC_ACCENT}},
    {"ECHO CANYON", 6, "1.......2.......", "9.......6.......", 0, 0, 0, NS_CHORD, 2, IX_RANDOM, 4, 0, 3, 60, 70, 0,
     PN_SWEEP, EV_NONE, 0, 0, 0, TR_ANY, {MC_DENSITY, MC_REPEAT, MC_RANDOM, MC_GATE}},
    {"FUNK CLAV", 6, "1.21.2.1-21.2.12", "9.59.5.9-69.5.79", "2.12.1.2-12.1.21", 0, 0, NS_CHORD, 1, IX_RANDOM, 0, 0, 1,
     50, 30, 4, PN_ALT, EV_NONE, 0, 0, 0, TR_ANY, {MC_DENSITY, MC_ACCENT, MC_CLUSTER, MC_GATE}},
    {"POLY 3 OVER 4", 6, "1..2..1..2..1..2..1..2..", "9..6..8..6..9..6..8..6..", 0, 0, 0, NS_OCT, 2, IX_UP, 1, 7,
     2, 40, 60, 0, PN_SWEEP, EV_NONE, 0, 0, 0, TR_AKR, {MC_DENSITY, MC_RANGE, MC_ACCENT, MC_REPEAT}},
    {"SWELL PAD", 24, "L---L---", "7---6---", "4---4---", 0, 0, NS_CHORD, 1, IX_UP, 0, 0, 1, 50, 100, 0, PN_ALT, EV_TRI,
     ET_SHMIX, 100, 0, TR_ANY, {MC_DENSITY, MC_ENV, MC_GATE, MC_TRANSPOSE}},
    {"CHAOS MACHINE", 6, "2323232323232323", "7364736473647364", 0, 0, 0, NS_SCALE, 4, IX_RANDOM, 1, 7, 1, 50, 50, 0,
     PN_RANDOM, EV_UP, ET_CHAOS, 120, 0, TR_AKR, {MC_DENSITY, MC_RANDOM, MC_ENV, MC_RANGE}},
    {"SCROLL HOPPER", 6, "1.121.121.121.12", "9.579.579.579.57", 0, 0, 0, NS_OCT, 2, IX_UPDOWN, 0, 0, 1, 50, 60, 0,
     PN_PITCH, EV_TRI, ET_OUTPUT, 127, 0, TR_AKR, {MC_DENSITY, MC_RANGE, MC_ENV, MC_ACCENT}},
    {"NOISE PERC", 6, "1.2.1.21.12.1.22", "9.5.8.69.75.9.56", 0, "l.h.l.hl.lh.l.hh", 0, NS_CHORD, 1, IX_DIRECT, 0, 0,
     1, 50, 12, 0, PN_RANDOM, EV_NONE, 0, 0, 0, TR_AKR, {MC_DENSITY, MC_ACCENT, MC_GATE, MC_RANDOM}},
    {"BIRD CALLS", 3, "1111111.........11111...........", "9898989.........97979...........", 0,
     "0101010.........02020...........", 0, NS_PENTA, 2, IX_DIRECT, 0, 0, 1, 50, 70, 0, PN_RANDOM, EV_NONE, 0, 0, 1,
     TR_ANY, {MC_DENSITY, MC_RANGE, MC_RANDOM, MC_REPEAT}},
    {"MINIMAL 5/4", 6, "1..2.1..2..1.2..1.2.", "9..6.8..6..9.6..8.6.", 0, "0..1.2..1..0.h..2.1.", 0, NS_OCT, 2,
     IX_DIRECT, 1, 12, 3, 30, 50, 0, PN_ALT, EV_UP, ET_BRIGHT, 70, 0, TR_AKR, {MC_DENSITY, MC_REPEAT, MC_ENV,
     MC_ACCENT}},
    {"LULLABY", 12, "11211121", "75657565", 0, "01210121", 0, NS_CHORD, 2, IX_DIRECT, 1, 12, 2, 35, 100, 0, PN_PITCH,
     EV_NONE, 0, 0, 0, TR_AKR, {MC_DENSITY, MC_RANGE, MC_REPEAT, MC_GATE}},
};
#define KGE_N (sizeof KGE / sizeof KGE[0])

/* ------------------------------------------------------------- the engine --- */
#define K_MAXN 16                                /* notes held */
#define K_SER 72                                 /* the series at most (16 notes x 4 octaves + 1) */
#define K_EVQ 32                                 /* notes to come (strums, echoes, swung steps) */
#define K_ACT 16                                 /* notes sounding (their offs) */

static struct {
    uint8_t note[K_MAXN], vel[K_MAXN], n, keys;  /* the notes held (keys / MIDI), keys still down */
    struct { uint32_t at, dur; uint8_t note, vel; int16_t pan; } ev[K_EVQ];
    uint8_t nev;
    struct { uint32_t off; uint8_t note; } act[K_ACT];
    uint8_t nact;
    uint32_t now;                                /* samples (its clock) */
    uint32_t tq8, gt;                            /* its own MIDI clocks: time to the next (Q8); clocks (the grid) */
    uint32_t clk_seen, starts;                   /* MIDI clock: the last clock counted, STARTs seen */
    uint32_t c, step, pos;                       /* clocks into the step; notes played, steps since the trigger */
    uint32_t steplen, phrase_t, phrase_len;      /* samples: a step; into the phrase, its length (FREE EG PHRASE) */
    uint32_t triggers;                           /* phrase starts (FREE EG PHRASE starts again with them) */
    uint8_t running;                             /* the phrase plays */
    uint8_t wait;                                /* a trigger: 1 at the next sixteenth, 2 at once */
    uint8_t last_idx, strum_up, ge_seen;
    uint32_t rng;
    uint8_t env_on, env_tgt, env_base;           /* its envelope driving a parameter, that parameter's value */
    volatile uint8_t view_step;                  /* the screen: the step playing (255: none) */
} kar = {.rng = 0x6C8E9CF5u, .view_step = 255, .ge_seen = 255};
static struct { uint8_t note[32]; volatile uint32_t w; } kar_view;   /* the notes played (the screen) */
static int feg_owns(volatile uint8_t *f);        /* feg.c: FREE EG drives this parameter */

static uint32_t k_rnd(void) { kar.rng = kar.rng * 1664525u + 1013904223u; return kar.rng >> 8; }

static const ge_t *k_ge(void) { return &KGE[sys.k_ge % KGE_N]; }
/* a realtime control's value: its knob (64: the GE as written), or 64 when the GE has no knob for it */
static int32_t k_mc(uint32_t mc)
{
    const ge_t *G = k_ge();
    uint32_t i;
    for (i = 0; i < 4u; i++)
        if (G->knob[i] == mc)
            return (int32_t)(sys.k_m[i] & 127u);
    return 64;
}

static volatile uint8_t *k_env_field(uint32_t t)
{
    switch (t) {
    case ET_RING: return &sys.ring;
    case ET_BRIGHT: return &sys.fx_bright;
    case ET_CHAOS: return &sys.chaos;
    case ET_OUTPUT: return &sys.output;
    case ET_FXMIX: return &sys.fx_mix;
    case ET_SHMIX: return &sys.sh_mix;
    case ET_NOISEX: return &sys.np_x;
    default: return &sys.mod_depth;
    }
}
static void k_env_release(void)                  /* the parameter given back */
{
    if (kar.env_on)
        *k_env_field(kar.env_tgt) = kar.env_base;
    kar.env_on = 0;
}

static void k_all_off(void)
{
    uint32_t i;
    for (i = 0; i < kar.nact; i++)
        gen_off(kar.act[i].note);
    kar.nact = 0;
    kar.nev = 0;
}

static void k_stop(void)                         /* the phrase stops (what sounds rings out to its gate) */
{
    kar.running = kar.wait = 0;
    kar.nev = 0;
    kar.view_step = 255;
    k_env_release();
}

static void karma_reset(void)
{
    k_all_off();
    k_stop();
    kar.n = kar.keys = 0;
}

static void k_trigger(void)                      /* the phrase from its start: at once from silence (on its own
                                                  * clock), else at the next sixteenth (KARMA's Quantize Trig) */
{
    kar.wait = kar.running ? 1u : 2u;
}

static void karma_add(uint32_t note, uint32_t vel)
{
    uint32_t i;
    int fresh = !kar.keys;
    if (fresh && sys.k_latch)                    /* LATCH: a new chord after all keys were let go replaces the old */
        kar.n = 0;
    kar.keys++;
    for (i = 0; i < kar.n; i++)
        if (kar.note[i] == note)
            return;
    if (kar.n >= K_MAXN)
        return;
    kar.note[kar.n] = (uint8_t)note;
    kar.vel[kar.n++] = (uint8_t)vel;
    if (k_ge()->trig == TR_ANY || fresh || (!kar.running && !kar.wait))
        k_trigger();
}

static void karma_remove(uint32_t note)
{
    uint32_t i, j;
    if (kar.keys)
        kar.keys--;
    if (sys.k_latch)
        return;
    for (i = 0; i < kar.n; i++)
        if (kar.note[i] == note) {
            for (j = i; j + 1u < kar.n; j++) {
                kar.note[j] = kar.note[j + 1u];
                kar.vel[j] = kar.vel[j + 1u];
            }
            kar.n--;
            break;
        }
    if (!kar.n)
        k_stop();
}

static void karma_latch_released(void)           /* LATCH off: the notes no longer held go */
{
    if (!kar.keys) {
        kar.n = 0;
        k_stop();
    }
}

/* the note series: the chord held made into the notes the phrase is built of, low to high */
static uint32_t k_series(uint8_t *ser, uint8_t *svel, const ge_t *G)
{
    uint8_t s[K_MAXN], v[K_MAXN];
    uint32_t n = kar.n, i, j, m = 0, oct;
    int32_t range = (int32_t)G->range + (k_mc(MC_RANGE) - 64) / 21;   /* (RANGE: -3 .. +3 octaves) */
    range = range < 1 ? 1 : range > 4 ? 4 : range;
    for (i = 0; i < n; i++) {
        s[i] = kar.note[i];
        v[i] = kar.vel[i];
    }
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && s[j - 1u] > s[j]; j--) {
            uint8_t t = s[j]; s[j] = s[j - 1u]; s[j - 1u] = t;
            t = v[j]; v[j] = v[j - 1u]; v[j - 1u] = t;
        }
    if (G->series == NS_SCALE || G->series == NS_PENTA) {   /* the scale the chord implies, from its lowest note */
        static const uint8_t MAJ[7] = {0, 2, 4, 5, 7, 9, 11}, MIN[7] = {0, 2, 3, 5, 7, 8, 10};
        static const uint8_t PMAJ[5] = {0, 2, 4, 7, 9}, PMIN[5] = {0, 3, 5, 7, 10};
        uint32_t root = s[0], pcs = 0, minor, k, kn = G->series == NS_PENTA ? 5u : 7u;
        for (i = 0; i < n; i++)
            pcs |= 1u << ((s[i] - root) % 12u);
        minor = (pcs >> 3 & 1u) && !(pcs >> 4 & 1u);   /* (a minor third and no major one) */
        for (oct = 0; oct < (uint32_t)range; oct++)
            for (k = 0; k < kn; k++) {
                uint32_t iv = G->series == NS_PENTA ? (minor ? PMIN[k] : PMAJ[k]) : (minor ? MIN[k] : MAJ[k]);
                ser[m] = (uint8_t)(root + 12u * oct + iv);
                svel[m++] = v[0];
            }
        ser[m] = (uint8_t)(root + 12u * (uint32_t)range);   /* (the top: the root again) */
        svel[m++] = v[0];
        return m;
    }
    for (oct = 0; oct < (uint32_t)range; oct++)
        for (i = 0; i < n; i++) {
            ser[m] = (uint8_t)(s[i] + 12u * oct);
            svel[m++] = v[i];
        }
    if (G->series == NS_OCT) {                   /* (OCT: the root on top) */
        ser[m] = (uint8_t)(s[0] + 12u * (uint32_t)range);
        svel[m++] = v[0];
    }
    return m;
}

static void k_sound(uint32_t note, uint32_t vel, int32_t pan, uint32_t dur)
{
    uint32_t i;
    for (i = 0; i < kar.nact; i++)               /* the same note again: its old off goes, the new one counts */
        if (kar.act[i].note == note) {
            kar.act[i] = kar.act[--kar.nact];
            break;
        }
    if (kar.nact >= K_ACT) {                     /* (full: the oldest ends now) */
        gen_off(kar.act[0].note);
        for (i = 1; i < kar.nact; i++)
            kar.act[i - 1u] = kar.act[i];
        kar.nact--;
    }
    synth_pan_next = (int16_t)pan;
    synth_pan_set = k_ge()->pan != PN_NONE;      /* (a GE without its own: BASIS's cycle, as played by hand) */
    gen_on(note, vel);
    synth_pan_set = 0;
    kar.act[kar.nact].note = (uint8_t)note;
    kar.act[kar.nact++].off = kar.now + (dur ? dur : 1u);
    kar_view.note[kar_view.w & 31u] = (uint8_t)note;
    kar_view.w++;
}

/* a note now or to come (strums, echoes, a swung step) */
static void k_play(uint32_t at, int32_t note, uint32_t vel, int32_t pan, uint32_t dur)
{
    while (note < 12)
        note += 12;
    while (note > 120)
        note -= 12;
    vel = vel > 127u ? 127u : vel;
    if (vel < 4u)
        return;
    pan = pan < -32767 ? -32767 : pan > 32767 ? 32767 : pan;
    if ((int32_t)(at - kar.now) <= 0) {
        k_sound((uint32_t)note, vel, pan, dur);
        return;
    }
    if (kar.nev >= K_EVQ)
        return;
    kar.ev[kar.nev].at = at;
    kar.ev[kar.nev].dur = dur;
    kar.ev[kar.nev].note = (uint8_t)note;
    kar.ev[kar.nev].vel = (uint8_t)vel;
    kar.ev[kar.nev].pan = (int16_t)pan;
    kar.nev++;
}

/* one step of the phrase */
static void k_step(void)
{
    const ge_t *G = k_ge();
    uint8_t ser[K_SER], sv[K_SER];
    uint32_t m, len = (uint32_t)str_len(G->rhy), p = kar.pos % len, c, k, idx0, ncl, vel, dur, t0 = kar.now, strum;
    int32_t dens = k_mc(MC_DENSITY), acc = k_mc(MC_ACCENT), rnd = k_mc(MC_RANDOM), gm = k_mc(MC_GATE);
    int32_t oct = 0, tr = (int32_t)G->transpose + (k_mc(MC_TRANSPOSE) - 64) / 32, nrep;
    char h = G->rhy[p];
    uint32_t tier = h == '1' || h == 'L' ? 1u : h == '2' ? 2u : h == '3' ? 3u : 0u, pr;
    kar.view_step = (uint8_t)p;
    kar.pos++;
    if (kar.pos % len == 0u)                     /* (the next step: the phrase from its start) */
        kar.pos = 0;
    if (h == '-') {                              /* a tie: what sounds goes on a step more */
        for (k = 0; k < kar.nact; k++)
            kar.act[k].off += kar.steplen;
        return;
    }
    /* DENSITY: each weight played with a chance (64: '1' and '2' always, '3' never); above, the ghosts and then
     * notes in the rests (at most half of them); below, the lighter hits thin out first, then the main ones */
    pr = tier == 1u ? (dens >= 32 ? 128u : (uint32_t)dens * 4u) :
         tier == 2u ? (dens >= 64 ? 128u : dens <= 32 ? 0u : (uint32_t)(dens - 32) * 4u) :
         tier == 3u ? (dens <= 64 ? 0u : (uint32_t)(dens - 64) * 2u) :
                      (dens <= 96 ? 0u : (uint32_t)(dens - 96) * 2u);
    if ((k_rnd() & 127u) >= pr)
        return;
    m = k_series(ser, sv, G);
    if (!m)
        return;
    /* which note: the index */
    switch (G->index) {
    case IX_UP: idx0 = kar.step % m; break;
    case IX_DOWN: idx0 = m - 1u - kar.step % m; break;
    case IX_UPDOWN: {
        uint32_t cy = m > 1u ? 2u * m - 2u : 1u, q = kar.step % cy;
        idx0 = q < m ? q : cy - q;
        break;
    }
    case IX_RANDOM: idx0 = k_rnd() % m; break;
    case IX_WALK: {                              /* a step or two up or down, turning at the ends */
        int32_t w = (int32_t)kar.last_idx + ((k_rnd() & 1u) ? 1 : -1) * (k_rnd() % 3u == 0u ? 2 : 1);
        if (w < 0)
            w = -w;
        if (w >= (int32_t)m)
            w = 2 * (int32_t)m - 2 - w;
        idx0 = w < 0 || w >= (int32_t)m ? 0u : (uint32_t)w;
        break;
    }
    case IX_SPIRAL: {                            /* low, high, the next low, the next high ... meeting in the middle */
        uint32_t q = kar.step % m;
        idx0 = q & 1u ? m - 1u - q / 2u : q / 2u;
        break;
    }
    default: {                                   /* Direct Index: the step names the note */
        char d = G->idx ? G->idx[p] : '0';
        idx0 = d >= '0' && d <= '9' ? (uint32_t)(d - '0') % m : d == 'h' ? m - 1u : d == 'l' ? 0u :
               d == '^' ? (kar.last_idx + 1u) % m : d == 'v' ? (kar.last_idx + m - 1u) % m : k_rnd() % m;
        break;
    }
    }
    if (rnd > 64 && (k_rnd() & 127u) < (uint32_t)(rnd - 64) * 2u)   /* RANDOM: now and then another */
        idx0 = k_rnd() % m;
    kar.last_idx = (uint8_t)(idx0 < m ? idx0 : 0u);
    idx0 = kar.last_idx;
    kar.step++;
    if (G->oct) {
        char o = G->oct[p];
        oct = o == '-' ? -1 : o >= '0' && o <= '3' ? o - '0' : 0;
    }
    if (rnd > 96 && (k_rnd() & 7u) == 0u)        /* (very random: an octave leap now and then) */
        oct += (k_rnd() & 1u) ? 1 : -1;
    {   /* velocity: the pattern, ACCENT stretching it around the middle (and above 96 some chance in it) */
        int32_t d = G->vel && G->vel[p] >= '0' && G->vel[p] <= '9' ? G->vel[p] - '0' : 6;
        int32_t v = (int32_t)sv[idx0] * (4 + d) / 13;
        v = 64 + (v - 64) * (16 + acc) / 80;
        if (acc > 96)
            v += (int32_t)(k_rnd() % (uint32_t)(acc - 95)) - (acc - 96) / 2;
        if (tier == 3u || tier == 0u)
            v = v * 2 / 3;                       /* (ghosts and fills: softer) */
        vel = (uint32_t)(v < 8 ? 8 : v > 127 ? 127 : v);
    }
    {   /* duration: the GE's gate x GATE (a long hit three steps) */
        uint32_t g = (uint32_t)G->gate * (uint32_t)(16 + gm) / 80u;
        g = g < 5u ? 5u : g > 100u ? 100u : g;
        dur = kar.steplen / 100u * g * (h == 'L' ? 3u : 1u);
    }
    /* the groove: the ARP's SWING on the off sixteenths (or eighths) */
    if ((G->res == 6u || G->res == 12u) && (p & 1u)) {
        uint32_t sw = 50u + (sys.arp_swing & 127u) * 33u / 127u;
        uint32_t d = kar.steplen / 100u * 2u * (sw - 50u);
        t0 += d;
        dur = dur > d + kar.steplen / 10u ? dur - d : kar.steplen / 10u;
    }
    /* cluster: notes a hit, from the chord at that place (CLUSTER adds or takes away), strummed down, then up */
    ncl = G->clu && G->clu[p] >= '1' && G->clu[p] <= '4' ? (uint32_t)(G->clu[p] - '0') : 1u;
    {
        int32_t cc = (int32_t)ncl + (k_mc(MC_CLUSTER) - 64) / 24;
        ncl = (uint32_t)(cc < 1 ? 1 : cc > 4 ? 4 : cc);
        if (ncl > kar.n)
            ncl = kar.n;
    }
    strum = (uint32_t)G->strum * (uint32_t)k_mc(MC_STRUM) / 64u * 44u;   /* samples between a strum's notes */
    kar.strum_up = (uint8_t)!kar.strum_up;
    nrep = (int32_t)G->rep_n * k_mc(MC_REPEAT) / 64;
    nrep = nrep > 6 ? 6 : nrep;
    for (c = 0; c < ncl; c++) {
        uint32_t ci = kar.strum_up ? ncl - 1u - c : c, idx = idx0;
        int32_t note, pan;
        uint32_t at = t0 + strum * c, rv = vel * (c ? 7u : 8u) / 8u;
        if (ncl > 1u) {                          /* (the chord tones from this octave's lowest) */
            idx = idx0 / kar.n * kar.n + ci;
            idx = idx >= m ? m - 1u : idx;
        }
        note = (int32_t)ser[idx] + 12 * (oct + tr);
        switch (G->pan) {
        case PN_ALT: pan = (kar.step & 1u) ? 24000 : -24000; break;
        case PN_RANDOM: pan = (int32_t)(k_rnd() % 65535u) - 32767; break;
        case PN_SWEEP:
            pan = (int32_t)(kar.phrase_t / 64u * 65534u / (kar.phrase_len / 64u + 1u)) - 32767;
            break;
        case PN_PITCH: pan = (note - 64) * 1200; break;
        default: pan = 0; break;
        }
        k_play(at, note, rv, pan, dur);
        /* melodic repeat: the note again, a few steps later, transposed, softer, on the other side */
        for (k = 1; k <= (uint32_t)nrep; k++) {
            rv = rv * G->rep_dec / 100u;
            k_play(at + k * G->rep_dly * kar.steplen, note + (int32_t)k * G->rep_iv, rv, k & 1u ? -pan : pan, dur);
        }
    }
}

/* its envelope over the phrase, onto a parameter (FREE EG's way: driven, then given back; FREE EG on the same one
 * wins) */
static void k_env(void)
{
    const ge_t *G = k_ge();
    volatile uint8_t *f;
    int32_t depth, ph, sh, v;
    if (G->env == EV_NONE || !kar.running || !kar.phrase_len) {
        k_env_release();
        return;
    }
    if (kar.env_on && kar.env_tgt != G->env_tgt)
        k_env_release();
    f = k_env_field(G->env_tgt);
    if (feg_owns(f)) {
        kar.env_on = 0;                          /* (not ours: nothing to give back) */
        return;
    }
    if (!kar.env_on) {
        kar.env_tgt = G->env_tgt;
        kar.env_base = *f;
        kar.env_on = 1;
    }
    ph = (int32_t)(kar.phrase_t / 64u * 1024u / (kar.phrase_len / 64u + 1u));   /* 0..1023 over the phrase */
    ph = ph > 1023 ? 1023 : ph;
    sh = G->env == EV_UP ? ph : G->env == EV_DOWN ? 1023 - ph : ph < 512 ? ph * 2 : 2047 - ph * 2;
    depth = (int32_t)G->env_depth * k_mc(MC_ENV) / 64;
    depth = depth > 127 ? 127 : depth;
    v = kar.env_base + (sh - 512) * depth / 1024;
    *f = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
}

/* a MIDI clock (its own or MIDI's): the grid, the trigger waiting for a sixteenth, the step */
static void k_tick(uint32_t res)
{
    kar.gt++;
    if (kar.wait == 1u && kar.gt % 6u == 0u) {   /* the trigger: the phrase from its start */
        kar.wait = 0;
        kar.running = 1;
        kar.pos = kar.step = kar.c = 0;
        kar.phrase_t = 0;
        kar.triggers++;
        k_step();
        return;
    }
    if (!kar.running)
        return;
    if (++kar.c >= res) {
        kar.c = 0;
        if (kar.pos == 0u) {                     /* the phrase again */
            kar.phrase_t = 0;
            kar.triggers++;
        }
        k_step();
    }
}

static void karma_block(uint32_t n)
{
    const ge_t *G = k_ge();
    uint32_t i, bpm = sys.arp_bpm < 40u ? 40u : sys.arp_bpm > 250u ? 250u : sys.arp_bpm;
    uint32_t spt = 2646000u / bpm / 24u, res = G->res, plen = (uint32_t)str_len(G->rhy);
    int ext = arp_ext();
    if (ext && arp.tick_len)
        spt = arp.tick_len;
    kar.steplen = spt * res;
    kar.phrase_len = kar.steplen * plen;
    if (sys.k_ge != kar.ge_seen) {               /* another GE: from its start */
        kar.ge_seen = sys.k_ge;
        if (kar.running)
            kar.wait = 1;
        k_env_release();
    }
    if (!sys.k_on) {
        if (kar.nact || kar.nev || kar.running)
            k_all_off();
        k_stop();
        kar.now += n;
        kar.clk_seen = arp.clk;
        kar.starts = arp.starts;
        return;
    }
    if (arp.starts != kar.starts) {              /* MIDI START: the grid from here, the phrase from its start */
        kar.starts = arp.starts;
        kar.clk_seen = arp.clk;
        kar.gt = 0xFFFFFFFFu;
        if (kar.n)
            kar.wait = 1;
    }
    if (kar.wait == 2u) {                        /* from silence: at once (MIDI clock: the next sixteenth) */
        if (ext)
            kar.wait = 1;
        else {
            kar.wait = 1;
            kar.gt = 0xFFFFFFFFu;
            kar.tq8 = 0;
            k_tick(res);
        }
    }
    if (ext) {
        uint32_t d = arp.clk - kar.clk_seen;
        kar.clk_seen = arp.clk;
        for (d = d > 8u ? 8u : d; d; d--)
            k_tick(res);
    } else {
        kar.clk_seen = arp.clk;
        kar.tq8 += n * 256u;
        while (kar.tq8 >= spt * 256u) {
            kar.tq8 -= spt * 256u;
            k_tick(res);
        }
    }
    /* the notes to come, the offs, this block */
    for (i = 0; i < kar.nev;) {
        if ((int32_t)(kar.ev[i].at - (kar.now + n)) < 0) {
            uint32_t d = kar.ev[i].dur;
            int32_t pan = kar.ev[i].pan;
            uint32_t note = kar.ev[i].note, vel = kar.ev[i].vel;
            kar.ev[i] = kar.ev[--kar.nev];
            k_sound(note, vel, pan, d);
        } else
            i++;
    }
    for (i = 0; i < kar.nact;) {
        if ((int32_t)(kar.act[i].off - (kar.now + n)) < 0) {
            gen_off(kar.act[i].note);
            kar.act[i] = kar.act[--kar.nact];
        } else
            i++;
    }
    kar.now += n;
    if (kar.running)
        kar.phrase_t = kar.phrase_t + n < kar.phrase_len ? kar.phrase_t + n : kar.phrase_len - 1u;
    k_env();
}
