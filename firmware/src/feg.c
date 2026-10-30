/* SPDX-License-Identifier: GPL-3.0-only */
/* FREE EG (the REC button; after the AN1x's Free EG): a knob's movement recorded and played back onto a parameter.
 *
 *   FREE EG page  KNOB 1 the knob you record (VALUE), KNOB 2 LENGTH: NOTE (each note starts it again, it plays once:
 *                 as long as you recorded, up to ~6 s), 1..8 STEPS (sixteenths), 1..4 BARS (both at the tempo:
 *                 the ARP's BPM, or MIDI clock while it comes in: then START starts it again too), PHRASE
 *                 (KARMA's phrase, the SEQ page: as long as it, from its start with it; a bar without), KNOB 3 DEPTH
 *                 (0: the parameter as it was .. 127: the recording itself), KNOB 4 the TARGET parameter;
 *                 SELECT PLAY on / off; REC (on the page) records: from the start of a cycle, for one length
 *                 (NOTE: until REC again, or full), the target following the knob while it records
 *
 * The recording: 128 points over the length. Playing, the target is driven (base + (curve - base) x DEPTH, base its
 * value when PLAY went on, given back when it goes off or the target changes): its own knob is taken over meanwhile.
 * The targets include KARMA's four realtime controls (KARMA K1..K4): its DENSITY or REPEAT moved over the phrase.
 *
 *   feg_block(n)   keys.c, the audio ISR, after the arpeggiator: once a block */
#define FEG_N 128u
#define FEG_NOTE_STEP 2048u                      /* NOTE: samples a point (128 points ~ 5.9 s) */
#define FEG_NLEN 14
#define FEG_PHRASE 13u                           /* PHRASE: KARMA's phrase (karma.c), each trigger from its start */
static const char *const FEG_LEN_NAME[FEG_NLEN] = {"NOTE", "1 STEP", "2 STEPS", "3 STEPS", "4 STEPS", "5 STEPS",
                                                   "6 STEPS", "7 STEPS", "8 STEPS", "1 BAR", "2 BARS", "3 BARS",
                                                   "4 BARS", "PHRASE"};
static uint32_t feg_ticks(uint32_t len)          /* MIDI clocks a cycle (24 a quarter, a step a sixteenth) */
{
    return len <= 8u ? len * 6u : len <= 12u ? (len - 8u) * 96u : 96u;   /* (PHRASE without KARMA: a bar) */
}

/* the parameters it can drive (all 0..127) */
#define FEG_NT 27
static const char *const FEG_T_NAME[FEG_NT] = {
    "CHAOS", "SHAPE", "OUTPUT", "RING", "NOISE X", "NOISE Y", "NOISE LEVEL", "MOD ENV HEIGHT", "FX STRUCTURE",
    "FX BRIGHTNESS", "FX DAMPING", "FX POSITION", "FX MIX", "SHIMMER SIZE", "SHIMMER DECAY", "SHIMMER", "SHIMMER MIX", "SHIMMER TONE", "BASIS", "ORBIT", "ARP GATE", "ARP SWING", "ENV DECAY",
    "KARMA K1", "KARMA K2", "KARMA K3", "KARMA K4",
};
static volatile uint8_t *feg_field(uint32_t t)
{
    switch (t % FEG_NT) {
    case 0: return &sys.chaos;
    case 1: return &sys.shape;
    case 2: return &sys.output;
    case 3: return &sys.ring;
    case 4: return &sys.np_x;
    case 5: return &sys.np_y;
    case 6: return &sys.np_level;
    case 7: return &sys.menv_height;
    case 8: return &sys.fx_struct;
    case 9: return &sys.fx_bright;
    case 10: return &sys.fx_damp;
    case 11: return &sys.fx_pos;
    case 12: return &sys.fx_mix;
    case 13: return &sys.sh_size;
    case 14: return &sys.sh_decay;
    case 15: return &sys.sh_shimmer;
    case 16: return &sys.sh_mix;
    case 17: return &sys.sh_tone;
    case 18: return &sys.basis;
    case 19: return &sys.orbit;
    case 20: return &sys.arp_gate;
    case 21: return &sys.arp_swing;
    case 23: return &sys.k_m[0];                 /* KARMA's realtime controls (what each does: its GE) */
    case 24: return &sys.k_m[1];
    case 25: return &sys.k_m[2];
    case 26: return &sys.k_m[3];
    default: return &sys.decay;
    }
}

static struct {
    uint8_t curve[FEG_N];                        /* the recording (sys.feg_has: there is one) */
    uint32_t t;                                  /* samples into the cycle (its own clock; NOTE: since the note) */
    uint32_t clk0, starts;                       /* MIDI clock: the clock the cycle counts from; STARTs seen */
    uint32_t notes;                              /* note ons seen (NOTE: each starts it again) */
    uint32_t ktrig;                              /* KARMA's phrase starts seen (PHRASE: each starts it again) */
    uint32_t last_idx;                           /* recording: the last point written */
    uint8_t rec;                                 /* recording */
    uint8_t playing, target, base;               /* driving: which parameter, its value before */
    volatile uint8_t pos;                        /* the play head, 0..127 (the screen); 255: none */
} feg = {.pos = 255};

static int feg_owns(volatile uint8_t *f)        /* (karma.c: its envelope leaves this one alone) */
{
    return (feg.playing || feg.rec) && feg_field(feg.rec ? sys.feg_target : feg.target) == f;
}

static void feg_release(void)                    /* the parameter given back */
{
    if (feg.playing)
        *feg_field(feg.target) = feg.base;
    feg.playing = 0;
}

static void feg_record(int on)                   /* REC: start (from the cycle's start) or stop */
{
    if (on) {
        feg_release();
        feg.rec = 1;
        feg.t = 0;
        feg.clk0 = arp.clk;
        feg.last_idx = 0xFFFFFFFFu;
        if (!sys.feg_len)
            sys.feg_npts = 0;
    } else if (feg.rec) {
        feg.rec = 0;
        if (sys.feg_len && feg.last_idx != 0xFFFFFFFFu)   /* (stopped early: the rest holds the last value) */
            for (uint32_t i = feg.last_idx + 1u; i < FEG_N; i++)
                feg.curve[i] = feg.curve[feg.last_idx];
        sys.feg_has = 1;
    }
}

static volatile uint8_t feg_req;                 /* the main loop's REC: 1 start, 2 stop (done here, in the ISR) */
static void feg_block(uint32_t n, uint32_t note_ons)
{
    uint32_t len = sys.feg_len % FEG_NLEN, L, p, idx, f8;
    volatile uint8_t *f;
    if (feg_req) {
        feg_record(feg_req == 1u);
        feg_req = 0;
    }
    if (note_ons != feg.notes) {                 /* a note: NOTE starts again */
        feg.notes = note_ons;
        if (!len && !feg.rec)
            feg.t = 0;
    }
    if (arp.starts != feg.starts) {              /* MIDI START: the cycle from its start */
        feg.starts = arp.starts;
        feg.clk0 = arp.clk;
        feg.t = 0;
    }
    if (kar.triggers != feg.ktrig) {             /* KARMA's phrase from its start: PHRASE too */
        feg.ktrig = kar.triggers;
        if (len == FEG_PHRASE) {
            if (feg.rec && feg.last_idx != 0xFFFFFFFFu && feg.last_idx + 8u >= FEG_N)
                feg_record(0);                   /* (a whole phrase recorded) */
            else if (feg.rec)
                feg.last_idx = 0xFFFFFFFFu;      /* (recording: from the phrase's start, in step with it) */
            feg.t = 0;
        }
    }
    if (len == FEG_PHRASE) {                     /* PHRASE: KARMA's (while it plays; else a bar at the tempo) */
        uint32_t bpm = sys.arp_bpm < 40u ? 40u : sys.arp_bpm > 250u ? 250u : sys.arp_bpm;
        L = kar.running && kar.phrase_len ? kar.phrase_len : 2646000u / bpm / 24u * 96u;
        if (feg.t >= L)
            feg.t = feg.rec ? L - 1u : feg.t % L;
        p = feg.t;
        if (feg.rec && p + n >= L) {
            feg.t = L - 1u;
        } else
            feg.t = feg.t + n < L || feg.rec ? feg.t + n : (feg.t + n) % L;
    } else if (!len) {                           /* NOTE: points every FEG_NOTE_STEP samples */
        uint32_t pts = feg.rec ? FEG_N : sys.feg_npts;
        L = (pts ? pts : 1u) * FEG_NOTE_STEP;
        p = feg.t < L ? feg.t : L - 1u;
        feg.t = feg.t < 0x7FFFFFFFu ? feg.t + n : feg.t;
    } else if (arp_ext()) {                      /* MIDI clock: the clocks since the cycle's start */
        uint32_t tk = feg_ticks(len), c = (arp.clk - feg.clk0) % tk;
        uint32_t tl = arp.tick_len ? arp.tick_len : 919u;
        L = tk * tl;
        p = c * tl + (arp.tick_t < tl ? arp.tick_t : tl - 1u);
        if (feg.rec && arp.clk - feg.clk0 >= tk)  /* one cycle recorded */
            feg_record(0);
    } else {                                     /* its own: the ARP's BPM */
        uint32_t bpm = sys.arp_bpm < 40u ? 40u : sys.arp_bpm > 250u ? 250u : sys.arp_bpm;
        uint32_t spt = 2646000u / bpm / 24u;     /* samples a MIDI clock */
        L = spt * feg_ticks(len);
        if (feg.rec && feg.t + n >= L) {
            feg.t = L - 1u;
            p = feg.t;
        } else
            p = feg.t % L;
        feg.t = (feg.t + n) % L;
    }
    if (!len) {                                  /* NOTE: a point every FEG_NOTE_STEP samples */
        idx = p / FEG_NOTE_STEP;
        f8 = (p % FEG_NOTE_STEP) * 256u / FEG_NOTE_STEP;
    } else {                                     /* the cycle: 128 points over its length */
        idx = p * FEG_N / L;                     /* (32 bits: p < L < 2^25, x 128 < 2^32) */
        f8 = (p * FEG_N % L) * 256u / L;
    }
    idx = idx >= FEG_N ? FEG_N - 1u : idx;
    if (feg.rec) {                               /* the knob into the points passed since the last block */
        uint32_t i = feg.last_idx == 0xFFFFFFFFu ? 0u : feg.last_idx + 1u;
        uint8_t v = (uint8_t)(sys.feg_val & 127u);
        for (; i <= idx && i < FEG_N; i++)
            feg.curve[i] = v;
        feg.last_idx = idx;
        *feg_field(sys.feg_target) = v;          /* (heard while recorded) */
        feg.pos = (uint8_t)idx;
        if (!len) {
            sys.feg_npts = (uint8_t)(idx + 1u > 127u ? 127u : idx + 1u);
            if (idx + 1u >= FEG_N)               /* full */
                feg_record(0);
        } else if ((len == FEG_PHRASE || !arp_ext()) && p + n >= L)
            feg_record(0);
        return;
    }
    if (!sys.feg_on || !sys.feg_has) {
        feg_release();
        feg.pos = 255;
        return;
    }
    if (!feg.playing || feg.target != sys.feg_target % FEG_NT) {   /* (on, or another target: take it over) */
        feg_release();
        feg.target = (uint8_t)(sys.feg_target % FEG_NT);
        feg.base = *feg_field(feg.target);
        feg.playing = 1;
    }
    {   /* between two points (Q8), then DEPTH around the base */
        int32_t a = feg.curve[idx], b = feg.curve[idx + 1u < FEG_N ? idx + 1u : idx], v;
        if (!len && idx + 1u >= sys.feg_npts)
            b = a;                               /* (NOTE: the last point holds) */
        v = a + (((b - a) * (int32_t)f8) >> 8);
        {   /* (the mod wheel on the same parameter: it sets how deep, 0 still .. DEPTH x the EDIT page's DEPTH) */
            int32_t d = (int32_t)(sys.feg_depth & 127u), md = (int32_t)(sys.mw_depth & 127u);
            if (md && sys.mw_seen && sys.mw_target % FEG_NT == feg.target)
                d = d * (int32_t)(sys.mw & 127u) * md / (127 * 127);
            v = feg.base + (v - feg.base) * d / 127;
        }
        f = feg_field(feg.target);
        *f = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
        feg.pos = (uint8_t)idx;
    }
}

/* the mod wheel (MIDI CC 1, the EDIT page): it moves a parameter of FREE EG's list up from where it is, DEPTH how far
 * (127: the wheel up takes it to 127); the parameter given back when the wheel is down. FREE EG playing the same one: the wheel sets FREE EG's depth instead (feg_block) */
static struct { uint8_t on, target, base; } mwd;
static void mw_block(void)
{
    uint32_t m = sys.mw & 127u, d = sys.mw_depth & 127u, t = sys.mw_target % FEG_NT;
    volatile uint8_t *f;
    if (mwd.on && (mwd.target != t || !m || !d)) {   /* (down, off, or another target: given back) */
        *feg_field(mwd.target) = mwd.base;
        mwd.on = 0;
    }
    if (!m || !d)
        return;
    f = feg_field(t);
    if (feg.playing && feg.target == t)
        return;
    if (!mwd.on) {
        mwd.on = 1;
        mwd.target = (uint8_t)t;
        mwd.base = *f;
    }
    *f = (uint8_t)(mwd.base + (127u - mwd.base) * m * d / (127u * 127u));
}
