/* SPDX-License-Identifier: GPL-3.0-only */
/* CHUA: a synth whose oscillator is Chua's circuit, the simplest electronic circuit that is chaotic.
 *
 * The circuit: two capacitors (C1, C2), an inductor (L), a resistor (R) and one nonlinear element, the Chua diode
 * (a negative resistance, piecewise linear). Its three state variables, made dimensionless:
 *   x  the voltage over C1        dx/dt = alpha (y - x - g(x))
 *   y  the voltage over C2        dy/dt = x - y + z
 *   z  the current through L      dz/dt = -beta y
 * g(x): slope M0 inside |x| < 1, M1 out to |x| < E2, M2 beyond (the op-amps that make the diode saturate there: every
 * setting stays bounded, as in a real circuit). The two outer equilibria at x = +-1.5 are spirals; the energy the
 * diode pumps in near x = 0 throws the state from one to the other.
 *
 * The sound: each voice is its own circuit. Pitch is the circuit's time scale: the solver advances the circuit by
 * h = f_note * period / FS per sample, where period is the time of one rotation around an equilibrium in that regime
 * (measured offline: chua_tables.h), so a note sounds at its pitch whatever the regime. The output is a blend of x, y
 * and z (OUTPUT), each normalised by its RMS in that regime (the tables again), then a DC blocker (x sits at +-1.5
 * while it spirals around one equilibrium) and the envelope.
 *
 *   CHAOS   (0..127) alpha, laid on the regimes found for each SHAPE (tools/gen_chua_tables.c):
 *           0 a clean limit cycle, 32 period 2, 48 the spiral (one scroll, chaotic), 60 the double scroll,
 *           112 the boundary crisis, then the large outer cycle of the saturated circuit
 *   SHAPE   (0..127) beta 10..34: low = slow, round spirals; high = tight, fast ones (and more of them per scroll)
 *   OUTPUT  (0..127) X, X+Y, Y, Y+Z, Z: x is the scroll switching (deep, noisy), y the bright spiral, z the round one
 *
 * Solver: the midpoint method (RK2) in Q20 fixed point (the state; +-2048), substeps of at most CHUA_HMAX time units
 * (the outer diode segment is stiff: alpha * (1 + M2) ~ 60), at most CHUA_NSUB per sample: very high notes stop
 * rising instead of going unstable. A voice that leaves the circuit's range anyway is restarted.
 * Display: chua_trace (a ring of x, y, z) follows the newest voice, or a "ghost" circuit that runs at the current
 * settings when nothing plays (it also seeds new voices with a state on the attractor).
 *
 *   synth_note_on(note, vel)    start a note (MIDI note number, velocity 1..127)
 *   synth_note_off(note)        release every voice playing that note
 *   synth_all_off()             release all; synth_kill() silence at once
 *   synth_render(out, n)        n stereo frames, Q15, interleaved L R (written) */
#include "chua_tables.h"

/* ------------------------------------------------------------ circuit --- */
#define CQ 20                                    /* the state: Q20 */
#define C1 (1 << CQ)                             /* 1.0 */
#define CHUA_M0 (-74898)                         /* -8/7, Q16 */
#define CHUA_M1 (-46811)                         /* -5/7, Q16 */
#define CHUA_M2 (131072)                         /*  2.0, Q16 */
#define CHUA_E2 (3 * C1)                         /* the second breakpoint */
#define CHUA_G1 ((int32_t)(((int64_t)CHUA_M0 * C1) >> 16))                       /* g(1) */
#define CHUA_G2 (CHUA_G1 + (int32_t)(((int64_t)CHUA_M1 * (CHUA_E2 - C1)) >> 16))  /* g(E2) */
#define CHUA_HMAX 335544u                        /* 0.02 time units, Q24: the longest substep */
#define CHUA_NSUB 4u                             /* substeps per sample at most */
#define CHUA_LIMIT (16 * C1)                     /* |x|, |y| (|z|: 4x) beyond this: restart the voice (the outer cycle
                                                  * reaches |x| 4.5, |y| 5, |z| 27 at SHAPE 127) */
#define CHUA_TRACE_DT 419430u                    /* 0.025 time units (Q24) between trace points */

typedef struct { int32_t x, y, z; } chua_t;

static int32_t chua_g(int32_t x)                 /* the Chua diode */
{
    int32_t a = x < 0 ? -x : x, g;
    if (a < C1)
        return (int32_t)(((int64_t)CHUA_M0 * x) >> 16);
    if (a < CHUA_E2)
        g = CHUA_G1 + (int32_t)(((int64_t)CHUA_M1 * (a - C1)) >> 16);
    else
        g = CHUA_G2 + (int32_t)(((int64_t)CHUA_M2 * (a - CHUA_E2)) >> 16);
    return x < 0 ? -g : g;
}

/* one substep of hs time units; ahs = alpha * hs, bhs = beta * hs (all Q24) */
static void chua_step(chua_t *s, int32_t hs, int32_t ahs, int32_t bhs)
{
    chua_t m;
    int32_t dx, dy, dz;
    dx = (int32_t)(((int64_t)ahs * (s->y - s->x - chua_g(s->x))) >> 24);   /* k1 */
    dy = (int32_t)(((int64_t)hs * (s->x - s->y + s->z)) >> 24);
    dz = (int32_t)(((int64_t)-bhs * s->y) >> 24);
    m.x = s->x + dx / 2;                                                    /* the midpoint */
    m.y = s->y + dy / 2;
    m.z = s->z + dz / 2;
    s->x += (int32_t)(((int64_t)ahs * (m.y - m.x - chua_g(m.x))) >> 24);   /* k2 */
    s->y += (int32_t)(((int64_t)hs * (m.x - m.y + m.z)) >> 24);
    s->z += (int32_t)(((int64_t)-bhs * m.y) >> 24);
}

/* The saturated circuit's outer cycle coexists with the inner attractors: once a circuit is on it (CHAOS turned up into
 * OUTER), turning CHAOS down would leave it there. Below OUTER a state on it (|y| > 2: the inner attractors stay
 * within |y| < 0.6) is moved back inside, as switching the real circuit off and on would. */
#define CHUA_Y_OUTER (2 * C1)
static int chua_on_outer(const chua_t *s) { return s->y > CHUA_Y_OUTER || s->y < -CHUA_Y_OUTER; }

static int chua_out_of_range(const chua_t *s)
{
    return s->x > CHUA_LIMIT || s->x < -CHUA_LIMIT || s->y > CHUA_LIMIT || s->y < -CHUA_LIMIT ||
           s->z > 4 * CHUA_LIMIT || s->z < -4 * CHUA_LIMIT;
}

/* ------------------------------------------------- the knobs' circuit --- */
enum { REG_CYCLE, REG_PERIOD2, REG_SPIRAL, REG_DOUBLE, REG_OUTER, REG_COUNT };
static const char *const REG_NAME[REG_COUNT] = {"CYCLE", "PERIOD 2", "SPIRAL", "DBL SCROLL", "OUTER"};
static uint32_t chua_regime(uint32_t chaos)      /* what the CHAOS knob's position is (the tables' landmarks) */
{
    return chaos < 32u ? REG_CYCLE : chaos < 48u ? REG_PERIOD2 : chaos < 60u ? REG_SPIRAL :
           chaos < 112u ? REG_DOUBLE : REG_OUTER;
}
static int32_t chua_beta_q16(uint32_t shape) { return (10 << 16) + (int32_t)(shape * (24u << 16) / 127u); }

/* a table at (SHAPE, CHAOS): bilinear */
static int32_t chua_lookup(const int32_t *t, uint32_t shape, uint32_t chaos)
{
    uint32_t rq = shape * ((CHUA_NB - 1u) << 8) / 127u, r = rq >> 8, rf = rq & 255u;
    uint32_t c = chaos >> 3, cf = (chaos & 7u) << 5;           /* 8 knob steps per column, cf /256 */
    const int32_t *a = t + r * CHUA_NK + c, *b = r + 1u < CHUA_NB ? a + CHUA_NK : a;
    int32_t top = a[0] + (int32_t)(((int64_t)(a[1] - a[0]) * cf) >> 8);
    int32_t bot = b[0] + (int32_t)(((int64_t)(b[1] - b[0]) * cf) >> 8);
    return top + (int32_t)(((int64_t)(bot - top) * rf) >> 8);
}
static int32_t chua_lookup16(const uint16_t *t, uint32_t shape, uint32_t chaos)
{
    int32_t tmp[2][2];
    uint32_t rq = shape * ((CHUA_NB - 1u) << 8) / 127u, r = rq >> 8, rf = rq & 255u, c = chaos >> 3, cf = (chaos & 7u) << 5;
    uint32_t r1 = r + 1u < CHUA_NB ? r + 1u : r;
    tmp[0][0] = t[r * CHUA_NK + c]; tmp[0][1] = t[r * CHUA_NK + c + 1u];
    tmp[1][0] = t[r1 * CHUA_NK + c]; tmp[1][1] = t[r1 * CHUA_NK + c + 1u];
    tmp[0][0] += ((tmp[0][1] - tmp[0][0]) * (int32_t)cf) >> 8;
    tmp[1][0] += ((tmp[1][1] - tmp[1][0]) * (int32_t)cf) >> 8;
    return tmp[0][0] + (((tmp[1][0] - tmp[0][0]) * (int32_t)rf) >> 8);
}

/* the per-block circuit for the knobs as they are now */
static struct {
    uint8_t chaos, shape, output, valid;
    int32_t alpha, beta, period;                 /* Q16 */
    int32_t gx, gy, gz;                          /* output gains: Q20 state -> Q15, x2^12 (see synth_render) */
} cc;
#define CHUA_LEVEL 7000                          /* each component's RMS in the output (Q15): about -13 dBFS */

static void chua_params(void)
{
    uint32_t chaos = sys.chaos > 127u ? 127u : sys.chaos, shape = sys.shape > 127u ? 127u : sys.shape;
    uint32_t o = sys.output > 127u ? 127u : sys.output;
    int32_t wx, wy, wz, rx, ry, rz;
    if (cc.valid && cc.chaos == chaos && cc.shape == shape && cc.output == o)
        return;
    cc.chaos = (uint8_t)chaos;
    cc.shape = (uint8_t)shape;
    cc.output = (uint8_t)o;
    cc.valid = 1;
    cc.alpha = chua_lookup(&CHUA_ALPHA[0][0], shape, chaos);
    cc.period = chua_lookup(&CHUA_PERIOD[0][0], shape, chaos);
    cc.beta = chua_beta_q16(shape);
    /* OUTPUT 0 X, 32 X+Y, 64 Y, 96 Y+Z, 127 Z: weights /256, a constant sum */
    wx = 256 - 4 * (int32_t)o;
    wx = wx < 0 ? 0 : wx;
    wz = 4 * (int32_t)o - 256;
    wz = wz < 0 ? 0 : wz > 256 ? 256 : wz;
    wy = 256 - wx - wz;
    rx = chua_lookup16(&CHUA_RMS[0][0][0], shape, chaos);
    ry = chua_lookup16(&CHUA_RMS[1][0][0], shape, chaos);
    rz = chua_lookup16(&CHUA_RMS[2][0][0], shape, chaos);
    /* out_q15 = state / 2^20 * LEVEL / rms * w / 256 = (state_q20 >> 8) * g >> 12 with g = LEVEL / rms * w / 256
     * (rms in CHUA_RMS_Q). 32-bit only (no 64-bit division on the target): LEVEL * 1024 < 2^23 */
    cc.gx = (int32_t)(((uint32_t)(CHUA_LEVEL * CHUA_RMS_Q) / (uint32_t)(rx < 16 ? 16 : rx) * (uint32_t)wx) >> 8);
    cc.gy = (int32_t)(((uint32_t)(CHUA_LEVEL * CHUA_RMS_Q) / (uint32_t)(ry < 16 ? 16 : ry) * (uint32_t)wy) >> 8);
    cc.gz = (int32_t)(((uint32_t)(CHUA_LEVEL * CHUA_RMS_Q) / (uint32_t)(rz < 16 ? 16 : rz) * (uint32_t)wz) >> 8);
}

/* --------------------------------------------------------------- trace --- */
#define TRACE_N 1024u                            /* a power of two */
static struct {
    int16_t x[TRACE_N], y[TRACE_N], z[TRACE_N];  /* Q8 (the state >> 12) */
    volatile uint32_t w;                         /* points written (the audio ISR only) */
    volatile int8_t voice;                       /* whose: a voice, or -1 the ghost */
} chua_trace;
static void trace_put(const chua_t *s)
{
    uint32_t i = chua_trace.w & (TRACE_N - 1u);
    chua_trace.x[i] = (int16_t)(s->x >> 12);
    chua_trace.y[i] = (int16_t)(s->y >> 12);
    chua_trace.z[i] = (int16_t)(s->z >> 12);
    RING_PUBLISH();
    chua_trace.w++;
}

/* --------------------------------------------------------------- voices --- */
static const uint32_t NOTE_INC_TOP[12] = {   /* phase increment (2^32 per cycle) of notes 120..131 */
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u,
};
static uint32_t note_inc(uint32_t note)      /* the note's frequency / FS, Q32 */
{
    if (note > 127u)
        note = 127u;
    return NOTE_INC_TOP[note % 12u] >> (10u - note / 12u);
}

/* ---- the envelope and the low pass gate (lpg_tables.h: tools/gen_lpg_tables.py)
 * ADSR: a linear attack, then an exponential fall towards the sustain level (DECAY), held there while the note is,
 * an exponential RELEASE. The ADSR drives a low pass gate as a vactrol does: its control follows the envelope, fast
 * up (~2 ms) and slower down (~30 ms), and sets both the gate's level and its filter's cutoff (25 Hz .. 15 kHz,
 * exponential). The filter is a resonant state-variable low pass (zero-delay feedback, Simper's form: stable at any
 * cutoff and resonance) with the circuit going in after the gate's level, so with RING up it rings: a ping as the
 * gate opens, and a tail that rings on after the note (TAIL) until the filter is quiet. */
#include "lpg_tables.h"
#define ENV_MAX (1u << 30)
enum { ST_OFF, ST_ATTACK, ST_SUSTAIN, ST_RELEASE, ST_TAIL };   /* (ST_SUSTAIN: the decay towards the sustain level) */
#define LPG_RISE 189813u                         /* the control's one-pole coefficients (Q24): ~2 ms up, */
#define LPG_FALL 12672u                          /* ~30 ms down: 1 - exp(-1 / (t FS)) */
#define LPG_EVERY 8u                             /* frames between filter coefficient updates (0.18 ms) */
#define LPG_QUIET 8192                           /* the TAIL ends when the filter's states are below this (Q23: -60 dB) */
typedef struct {
    chua_t s;
    uint32_t inc;                /* the note's frequency / FS, Q32 */
    uint32_t env;                /* the ADSR, 0..ENV_MAX */
    uint32_t ctl;                /* the gate's control (the vactrol), 0..ENV_MAX */
    uint32_t age;                /* started at (voice clock): the oldest is stolen first */
    int32_t dc;                  /* the DC blocker's state (Q15 << 8) */
    int32_t ic1, ic2;            /* the filter's states (Q23) */
    int32_t a1, a2, a3;          /* its coefficients for the cutoff now (Q28) */
    uint32_t trace_t;            /* time since its last trace point (Q24) */
    uint16_t amp;                /* velocity, Q15 */
    uint8_t note, stage, fresh;  /* fresh: the DC blocker starts at the first sample */
    uint8_t lpg_n;               /* frames until the next coefficient update */
} voice_t;
static voice_t voice[NVOICE];
static uint32_t voice_clock;
static chua_t ghost = {C1 / 10, 0, 0};          /* the circuit at the current settings, one substep per block */
static uint32_t chua_restarts;                   /* voices restarted because the solver left the range (a diagnostic) */

/* the knobs' envelope and ring, per block (read by both cores while they render: written before) */
static struct { uint32_t att, dec, sus, rel; int32_t k, comp; } eg;
static void env_params(void)
{
    eg.att = ENV_MAX / ENV_FRAMES[sys.attack & 127u];
    eg.dec = ENV_COEF[sys.decay & 127u];
    eg.sus = (uint32_t)(sys.sustain & 127u) * (ENV_MAX / 127u);
    eg.rel = ENV_COEF[sys.release & 127u];
    eg.k = (int32_t)LPG_K[sys.ring & 127u];
    eg.comp = LPG_COMP[sys.ring & 127u];
}

static void synth_note_on(uint32_t note, uint32_t vel)
{
    uint32_t i;
    voice_t *v = 0;
    for (i = 0; i < NVOICE && !v; i++)              /* the same note again: retrigger its voice */
        if (voice[i].stage != ST_OFF && voice[i].note == note)
            v = &voice[i];
    for (i = 0; i < NVOICE && !v; i++)              /* a free voice */
        if (voice[i].stage == ST_OFF)
            v = &voice[i];
    if (!v) {                                       /* steal: the oldest ringing tail, else released, else the oldest */
        voice_t *best = 0;
        uint32_t s;
        for (s = ST_TAIL; s >= ST_RELEASE && !best; s--)
            for (i = 0; i < NVOICE; i++)
                if (voice[i].stage == s && (!best || voice[i].age < best->age))
                    best = &voice[i];
        if (!best)
            for (i = 0, best = &voice[0]; i < NVOICE; i++)
                if (voice[i].age < best->age)
                    best = &voice[i];
        v = best;
    }
    if (v->stage == ST_OFF) {                       /* a new circuit: where the ghost is (on the attractor), nudged a */
        v->s = ghost;                               /* little so that voices in a chord do not run in lockstep */
        v->s.x += (int32_t)(rng() & 0x3FFFu) - 0x2000;
        v->s.y += (int32_t)(rng() & 0x0FFFu) - 0x0800;
        v->env = v->ctl = 0;
        v->ic1 = v->ic2 = 0;
        v->lpg_n = 0;
        v->fresh = 1;
    }
    v->note = (uint8_t)note;
    v->inc = note_inc(note);
    v->amp = (uint16_t)(vel * 258u);                /* 127 -> 32766 */
    v->stage = ST_ATTACK;                           /* (a retrigger: the attack from where the envelope is) */
    v->age = ++voice_clock;
    sys.last_note = (uint8_t)note;
    sys.last_vel = (uint8_t)vel;
}

static void synth_note_off(uint32_t note)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (voice[i].note == note && (voice[i].stage == ST_ATTACK || voice[i].stage == ST_SUSTAIN))
            voice[i].stage = ST_RELEASE;
}

static void synth_all_off(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (voice[i].stage == ST_ATTACK || voice[i].stage == ST_SUSTAIN)
            voice[i].stage = ST_RELEASE;
}

static void synth_kill(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        voice[i].stage = ST_OFF;
}

/* the ADSR and the gate's control, one frame; 1 = the voice is done */
static int env_frame(voice_t *v)
{
    switch (v->stage) {
    case ST_ATTACK:
        if (ENV_MAX - v->env <= eg.att) {
            v->env = ENV_MAX;
            v->stage = ST_SUSTAIN;
        } else
            v->env += eg.att;
        break;
    case ST_SUSTAIN:
        v->env = (uint32_t)((int32_t)v->env - (int32_t)(((int64_t)((int32_t)v->env - (int32_t)eg.sus) * eg.dec) >> 24));
        break;
    case ST_RELEASE:
        v->env -= (uint32_t)(((uint64_t)v->env * eg.rel) >> 24) + 1u;
        if (v->env < (ENV_MAX >> 14) || v->env > ENV_MAX) {   /* (-84 dB) */
            v->env = 0;
            v->stage = ST_TAIL;
        }
        break;
    }
    {
        int32_t d = (int32_t)v->env - (int32_t)v->ctl;
        v->ctl = (uint32_t)((int32_t)v->ctl + (int32_t)(((int64_t)d * (d > 0 ? LPG_RISE : LPG_FALL)) >> 24));
    }
    if (v->stage == ST_TAIL && v->ctl < (ENV_MAX >> 4)) {   /* the gate nearly shut (cutoff < ~40 Hz): a ring down there */
        v->ic1 -= v->ic1 >> 8;                          /* is felt, not heard; let it die in ~6 ms */
        v->ic2 -= v->ic2 >> 8;
    }
    if (v->stage == ST_TAIL && v->ctl < (ENV_MAX >> 12) &&
        v->ic1 < LPG_QUIET && v->ic1 > -LPG_QUIET && v->ic2 < LPG_QUIET && v->ic2 > -LPG_QUIET) {
        v->stage = ST_OFF;
        return 1;
    }
    return 0;
}

/* the gate's filter coefficients for its control now: g = tan(pi fc / FS), k = 1 / Q; Q28 (the tables are Q24).
 * Fine enough for the lowest cutoff (g ~ 0.0018: g^2 ~ 850 in Q28), where the ring's decay is slowest */
static void lpg_coefs(voice_t *v)
{
    int64_t g = (int64_t)LPG_G[v->ctl >> 22 > 255u ? 255u : v->ctl >> 22] << 4, k = (int64_t)eg.k << 4;
    uint32_t d = (uint32_t)((1 << 28) + ((g * (g + k)) >> 28));               /* 1 + g (g + k): 1 .. ~8, Q28 */
    v->a1 = (int32_t)(((1u << 31) / (d >> 13)) << 12);                         /* 1 / d (32-bit division) */
    v->a2 = (int32_t)((g * v->a1) >> 28);
    v->a3 = (int32_t)((g * v->a2) >> 28);
}

/* one frame through the gate: x the circuit's output (Q15), the gate's level and filter -> Q15. The states are Q23
 * (a full-scale input rung up 40 times still fits), the products rounded (no bias: a ring decays to nothing) */
#define LPG_R (1ll << 27)
static int32_t lpg_frame(voice_t *v, int32_t x)
{
    int32_t v0, v1, v2, v3, lvl = (int32_t)(((v->ctl >> 15) * v->amp) >> 15);   /* Q15 */
    if (!v->lpg_n--) {
        v->lpg_n = LPG_EVERY - 1u;
        lpg_coefs(v);
    }
    v0 = (int32_t)(((int64_t)x * lvl) >> 7);                                   /* Q23 */
    v3 = v0 - v->ic2;
    v1 = (int32_t)(((int64_t)v->a1 * v->ic1 + (int64_t)v->a2 * v3 + LPG_R) >> 28);
    v2 = v->ic2 + (int32_t)(((int64_t)v->a2 * v->ic1 + (int64_t)v->a3 * v3 + LPG_R) >> 28);
    v->ic1 = 2 * v1 - v->ic1;
    v->ic2 = 2 * v2 - v->ic2;
    return (int32_t)(((int64_t)v2 * eg.comp) >> 20);                         /* Q23 x Q12 -> Q15 */
}

static int32_t soft_clip(int32_t x)              /* Q15 in +-1.5 -> +-1: x - 4/27 x^3 */
{
    int64_t c;
    x = clamp(x, -49151, 49151);
    c = ((int64_t)x * x >> 15) * x >> 15;
    return x - (int32_t)(c * 4 / 27);
}

/* the substeps of a voice: h = its time per sample (Q24) split into n of at most CHUA_HMAX */
static uint32_t voice_h(const voice_t *v, int32_t *hs)
{
    /* inc / 2^32 * period / 2^16, Q24: < 2^25 for every note and table entry (32-bit division from here on) */
    uint32_t h = (uint32_t)(((uint64_t)v->inc * (uint32_t)cc.period) >> 24);
    uint32_t n = (h + CHUA_HMAX - 1u) / CHUA_HMAX;
    if (n > CHUA_NSUB) {                                /* (higher notes stop rising: the solver's limit) */
        n = CHUA_NSUB;
        h = CHUA_HMAX * CHUA_NSUB;
    }
    if (n < 1u)
        n = 1u;
    *hs = (int32_t)(h / n);
    return n;
}

/* one voice's block into sum[0..n-1] (mono); restarts: the counter of the core it runs on */
static voice_t *render_tv;                       /* the voice the trace follows (set before the voices render) */
static void render_voice(voice_t *v, uint32_t n, int32_t *sum, uint32_t *restarts)
{
    int32_t hs, ahs, bhs;
    uint32_t ns, i, j, h_all;
    ns = voice_h(v, &hs);
    ahs = (int32_t)(((int64_t)cc.alpha * hs) >> 16);
    bhs = (int32_t)(((int64_t)cc.beta * hs) >> 16);
    h_all = (uint32_t)hs * ns;
    for (i = 0; i < n; i++) {
        int32_t s;
        int done = env_frame(v);
        for (j = 0; j < ns; j++)
            chua_step(&v->s, hs, ahs, bhs);
        if (chua_out_of_range(&v->s)) {                 /* (the solver lost it: start over on the attractor) */
            (*restarts)++;
            v->s = ghost;
            v->fresh = 1;
        }
        s = (int32_t)(((int64_t)(v->s.x >> 8) * cc.gx + (int64_t)(v->s.y >> 8) * cc.gy +
                       (int64_t)(v->s.z >> 8) * cc.gz) >> 12);
        if (v->fresh) {
            v->fresh = 0;
            v->dc = s * 256;
        }
        v->dc += (s * 256 - v->dc) >> 9;                /* DC blocker, ~14 Hz */
        s -= v->dc >> 8;
        sum[i] += lpg_frame(v, s) >> 1;
        if (v == render_tv) {
            v->trace_t += h_all;
            if (v->trace_t >= CHUA_TRACE_DT) {
                v->trace_t -= CHUA_TRACE_DT;
                if (v->trace_t > CHUA_TRACE_DT)
                    v->trace_t = 0;
                trace_put(&v->s);
            }
        }
        if (done)
            break;
    }
}

/* ---- the second core (hal/fm1_cpu1.h; the job hand-over after Felucca's fx.c mix_block): it renders every other
 * sounding voice into its own sum while this core renders the rest, then this core adds it in. A voice's render
 * touches only that voice; what they share (the knobs' circuit cc, the ghost, the trace's choice) is settled before
 * the split, and only the trace's voice writes the trace. The sums are integers, so the output is bit for bit one
 * core's (tests/basis_test.c checks it). Without a second core (the host tests, 2ND CORE OFF, one that did not answer
 * or stopped: then it is held for good) C1_RUN takes nothing and this core renders them all. */
#ifndef C1_RUN
#define C1_ON() 0                                /* a second core is answering */
#define C1_RUN(fn, arg) 0                        /* hand it fn(arg): 1 = taken */
#define C1_WAIT() 0                              /* wait for it: 0 = done */
#endif
static int32_t c1_sum[CTL];
static uint32_t c1_restarts, c1_blocks;          /* (diagnostics) */
static void c1_voices(uint32_t arg)              /* arg: the voices (bit k = voice k) | n << 8 */
{
    uint32_t k, n = arg >> 8;
    for (k = 0; k < n; k++)
        c1_sum[k] = 0;
    for (k = 0; k < NVOICE; k++)
        if (arg >> k & 1u)
            render_voice(&voice[k], n, c1_sum, &c1_restarts);
}

static void synth_render(int32_t *out, uint32_t n)
{
    uint32_t i, k, busy = 0, newest = 0, mask = 0, own = 0;
    int32_t m = (int32_t)sys.master_q12, sum[CTL];
    voice_t *tv = 0;
    int inner;
    chua_params();
    env_params();
    inner = chua_regime(cc.chaos) != REG_OUTER;
    for (i = 0; i < n; i++)
        sum[i] = 0;
    for (k = 0; k < NVOICE; k++)                        /* the trace follows the newest voice sounding */
        if (voice[k].stage != ST_OFF && voice[k].age >= newest) {
            newest = voice[k].age;
            tv = &voice[k];
        }
    render_tv = tv;
    sys.env_view = tv ? (uint8_t)(tv->ctl >> 22 > 255u ? 255u : tv->ctl >> 22) : 0u;   /* (ui.c: the ENV page's dot) */
    sys.env_stage = tv ? tv->stage : (uint8_t)ST_OFF;
    {   /* the ghost: one substep a block (~34 time units a second, ~20 rotations) */
        int32_t hs = (int32_t)(CHUA_HMAX + CHUA_HMAX / 4u);
        chua_step(&ghost, hs, (int32_t)(((int64_t)cc.alpha * hs) >> 16), (int32_t)(((int64_t)cc.beta * hs) >> 16));
        if (chua_out_of_range(&ghost) || (inner && chua_on_outer(&ghost))) {
            ghost.x = C1 / 10;                          /* (near the origin: it spirals out onto the inner attractor) */
            ghost.y = ghost.z = 0;
        }
        if (!tv)
            trace_put(&ghost);
        chua_trace.voice = tv ? (int8_t)(tv - voice) : -1;
    }
    for (k = 0; k < NVOICE; k++) {
        voice_t *v = &voice[k];
        if (v->stage == ST_OFF)
            continue;
        if (inner && chua_on_outer(&v->s)) {            /* (CHAOS came down from OUTER: back to the inner attractor) */
            v->s = ghost;
            v->fresh = 1;
        }
        if (busy++ & 1u)                                /* every other sounding voice: the second core's */
            mask |= 1u << k;
        else
            own |= 1u << k;
    }
    if (!C1_ON() || !mask || !C1_RUN(c1_voices, mask | n << 8)) {
        own |= mask;
        mask = 0;
    }
    c1_blocks += mask != 0;
    for (k = 0; k < NVOICE; k++)                        /* (decided above: nothing of the other core's voices read) */
        if (own >> k & 1u)
            render_voice(&voice[k], n, sum, &chua_restarts);
    if (mask) {
        if (C1_WAIT())                                  /* it stopped answering (held from now on): its voices here */
            c1_voices(mask | n << 8);
        for (i = 0; i < n; i++)
            sum[i] += c1_sum[i];
    }
    for (i = 0; i < n; i++) {
        int32_t s = clamp(soft_clip(sum[i]) * m >> 12, -32767, 32767);
        out[2u * i] = out[2u * i + 1u] = s;
    }
    sys.voices = (uint8_t)busy;
}
