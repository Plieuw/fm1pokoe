/* SPDX-License-Identifier: GPL-3.0-only */
/* TAPE: a four-track reel-to-reel recorder (the LFO button's page). A loop of tape with four tracks side by side, at
 * a quarter of the rate (11 kHz, 8-bit mu-law: a tape's lo-fi), recorded from the synth's own output, played under
 * it. The tape runs at the tempo it was recorded at: turn the BPM (or the MIDI clock changes it) and it runs faster
 * or slower, its pitch going with it, as a tape machine's varispeed does; the loop stays the same number of beats.
 *
 *   LENGTH   (SELECT) the loop in beats: 1, 2, 4 (a bar), 8, 16; as long as the tape holds at the tape's tempo
 *   TRACK    (ALGORITHM) the track REC records (1..4)
 *   LEVELS   KNOB 1..4 each track's level (the mixer's faders)
 *   BPM      (PRESETS) the tempo (the ARP's): the tape's speed and pitch follow it
 *   PLAY     the transport: start (from the loop's start) / stop
 *   REC      record the armed track for one pass of the loop (starting the tape if it stands); again: stop
 *            recording. With PLAY held: erase the armed track
 *
 * The first recording on a blank tape sets its tempo (TEMPO: the samples a beat then); an erased tape (all four
 * tracks) is blank again. MIDI START puts the tape back at the loop's start. Recording replaces what was on the
 * track (an erase head before the record head); the track being recorded is heard live, not from the tape. */

#define TAPE_NT 4u
#define TAPE_N 40000u                            /* cells per track: 3.6 s at 11 kHz (the pool's room) */
#define TAPE_DIV 4u                              /* the tape's rate: FS / 4 */
static uint8_t tape_buf[TAPE_NT][TAPE_N] __attribute__((section(".pool")));
static int16_t tape_mu[256];                     /* mu-law decoding */
static struct {
    uint32_t pos;                                /* the head, cells Q16 */
    uint32_t len;                                /* the loop, cells */
    uint32_t spb;                                /* the tape's tempo: output samples a beat when recorded (0: blank) */
    uint32_t inc;                                /* the head's step an output sample, Q16 (speed / TAPE_DIV) */
    uint32_t speed;                              /* Q12: 4096 = the tempo it was recorded at */
    uint32_t rec_left, cell, starts;
    uint32_t lock_q12, lock_bpm;                 /* a SPEED chosen: exactly this ratio while the BPM stays lock_bpm */
    int32_t acc, accn, lp, lvl[TAPE_NT];
    int32_t meter[TAPE_NT];                      /* each track's level for the screen, 0..32767 */
    uint8_t playing, rec, has[TAPE_NT];
    volatile uint8_t req;                        /* from the UI: 1 play/stop, 2 rec, 3 erase the armed track */
} tape;

/* SPEED (the TAPE page's second menu): the tape's speed in ratios of the harmonic series (5-limit just intonation),
 * each landing on a step of the chromatic scale: -12 .. +12 semitones, 1/2 (half speed) .. 2/1 (double) */
#define TAPE_NSPD 25
static const uint8_t TAPE_SPD[TAPE_NSPD][2] = {
    {1, 2}, {8, 15}, {9, 16}, {3, 5}, {5, 8}, {2, 3}, {45, 64}, {3, 4}, {4, 5}, {5, 6}, {9, 10}, {15, 16},
    {1, 1}, {16, 15}, {9, 8}, {6, 5}, {5, 4}, {4, 3}, {45, 32}, {3, 2}, {8, 5}, {5, 3}, {9, 5}, {15, 8}, {2, 1},
};
/* the pitch of a speed (Q12) in cents: 1200 log2 (log2 of 1 + f ~ f + 0.3466 f (1 - f): within ~5 cents) */
static int32_t tape_cents(uint32_t q12)
{
    int32_t oct = 0, f;
    if (!q12)
        return 0;
    while (q12 >= 8192u) { q12 >>= 1; oct++; }
    while (q12 < 4096u) { q12 <<= 1; oct--; }
    f = (int32_t)q12 - 4096;                                     /* Q12, 0 .. 1 */
    f = f + (f * (4096 - f) >> 12) * 1420 / 4096;
    return oct * 1200 + f * 1200 / 4096;
}
static int32_t tape_beats(void)                  /* the LENGTH in beats */
{
    static const uint8_t B[5] = {1, 2, 4, 8, 16};
    return B[sys.tape_len % 5u];
}
static uint8_t tape_enc(int32_t v)               /* G.711 mu-law, 16-bit in */
{
    int32_t s = v < 0 ? 0x80 : 0, e = 7, m;
    if (v < 0)
        v = -v;
    v = (v > 32635 ? 32635 : v) + 132;
    while (e > 0 && !(v & (0x4000 >> (7 - e))))
        e--;
    m = (v >> (e + 3)) & 15;
    return (uint8_t)~(s | e << 4 | m);
}
static void tape_init(void)
{
    uint32_t i;
    for (i = 0; i < 256u; i++) {
        uint32_t u = ~i & 255u, e = (u >> 4) & 7u, m = u & 15u;
        int32_t v = (int32_t)(((m << 3) + 132u) << e) - 132;
        tape_mu[i] = (int16_t)(u & 0x80u ? -v : v);
    }
    for (i = 0; i < TAPE_NT; i++)
        memset(tape_buf[i], 0xFF, TAPE_N);       /* (0xFF: silence in mu-law) */
}
/* the loop in cells at the tape's tempo: LENGTH beats, as many as fit */
static uint32_t tape_cells(uint32_t spb)
{
    uint32_t b = (uint32_t)tape_beats(), c;
    while ((c = b * spb / TAPE_DIV) > TAPE_N && b > 1u)
        b >>= 1;
    return c > TAPE_N ? TAPE_N : c ? c : 1u;
}
/* the output samples a beat now (the BPM, or the MIDI clock) */
static uint32_t tape_spb_now(void)
{
    uint32_t bpm = sys.arp_bpm < 40u ? 40u : sys.arp_bpm > 250u ? 250u : sys.arp_bpm;
    if (arp_ext() && arp.tick_len)
        return arp.tick_len * 24u;
    return 2646000u / bpm;
}

/* every 32 samples (keys.c, the audio interrupt): the UI's requests, the speed, MIDI START */
static void tape_block(void)
{
    uint32_t now = tape_spb_now(), t, a = sys.tape_arm % TAPE_NT;
    switch (tape.req) {
    case 1:                                      /* PLAY: start from the loop's start / stop */
        tape.playing = (uint8_t)!tape.playing;
        tape.pos = 0;
        tape.rec = 0;
        break;
    case 2:                                      /* REC: one pass of the armed track / stop recording */
        if (tape.rec)
            tape.rec = 0;
        else {
            if (!tape.spb)                       /* (a blank tape: its tempo now) */
                tape.spb = now;
            tape.len = tape_cells(tape.spb);
            if (!tape.playing) {
                tape.playing = 1;
                tape.pos = 0;
            }
            tape.rec = 1;
            tape.rec_left = tape.len;
            tape.cell = tape.pos >> 16;
            tape.acc = tape.accn = 0;
            tape.has[a] = 1;
        }
        break;
    case 3:                                      /* erase the armed track (all erased: the tape blank) */
        memset(tape_buf[a], 0xFF, TAPE_N);
        tape.has[a] = 0;
        tape.rec = 0;
        if (!(tape.has[0] | tape.has[1] | tape.has[2] | tape.has[3]))
            tape.spb = 0;
        break;
    default:
        break;
    }
    tape.req = 0;
    if (arp.starts != tape.starts) {             /* MIDI START: the loop's start */
        tape.starts = arp.starts;
        tape.pos = 0;
    }
    if (tape.spb) {
        tape.len = tape_cells(tape.spb);
        if (tape.lock_q12 && !arp_ext() && sys.arp_bpm == tape.lock_bpm)
            tape.speed = tape.lock_q12;          /* (a SPEED's ratio, exact: in tune) */
        else {
            tape.lock_q12 = 0;                   /* (the BPM turned: free varispeed again) */
            tape.speed = tape.spb * 4096u / (now ? now : 1u);   /* (tape.spb < 2^17: no overflow) */
        }
        tape.speed = tape.speed < 1024u ? 1024u : tape.speed > 16384u ? 16384u : tape.speed;
    } else {
        tape.len = tape_cells(now);
        tape.speed = 4096;
    }
    tape.inc = tape.speed * 16u / TAPE_DIV;      /* (Q12 -> Q16, a quarter a sample) */
    if ((tape.pos >> 16) >= tape.len)
        tape.pos = 0;
    for (t = 0; t < TAPE_NT; t++) {
        tape.lvl[t] = (int32_t)(sys.tape_lv[t] & 127u) * 258;   /* Q15 */
        if (!tape.playing)
            tape.meter[t] -= tape.meter[t] >> 4;
    }
}

/* the output (16-bit, l and r): the armed track recorded from it, the tape played under it */
static void tape_run(int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i, t, a = sys.tape_arm % TAPE_NT, len = tape.len ? tape.len : 1u;
    int32_t pk[TAPE_NT] = {0, 0, 0, 0};
    if (!tape.playing)
        return;
    for (i = 0; i < n; i++) {
        uint32_t c = tape.pos >> 16, c1 = c + 1u < len ? c + 1u : 0u, f = (tape.pos >> 4) & 4095u;
        int32_t mix = 0;
        for (t = 0; t < TAPE_NT; t++) {
            int32_t s0, s1;
            if (!tape.lvl[t] || (tape.rec && t == a))
                continue;
            s0 = tape_mu[tape_buf[t][c]];
            s1 = tape_mu[tape_buf[t][c1]];
            s0 = (s0 + (((s1 - s0) * (int32_t)f) >> 12)) * tape.lvl[t] >> 15;
            mix += s0;
            s0 = s0 < 0 ? -s0 : s0;
            pk[t] = s0 > pk[t] ? s0 : pk[t];
        }
        tape.lp += (mix - tape.lp) * 15 >> 5;    /* (the tape's highs rolling off: ~5 kHz) */
        if (tape.rec) {                          /* the input averaged over the cell, written when it is passed */
            int32_t in = (l[i] + r[i]) / 2;
            tape.acc += in;
            in = in < 0 ? -in : in;
            pk[a] = in > pk[a] ? in : pk[a];
            tape.accn++;
            if (c != tape.cell) {
                tape_buf[a][tape.cell] = tape_enc(tape.accn ? tape.acc / tape.accn : 0);
                tape.acc = tape.accn = 0;
                tape.cell = c;
                if (!--tape.rec_left)
                    tape.rec = 0;
            }
        }
        l[i] += tape.lp;
        r[i] += tape.lp;
        tape.pos += tape.inc;
        if ((tape.pos >> 16) >= len)
            tape.pos -= len << 16;
    }
    for (t = 0; t < TAPE_NT; t++)
        tape.meter[t] = pk[t] > tape.meter[t] ? pk[t] : tape.meter[t] - (tape.meter[t] >> 5);
}
