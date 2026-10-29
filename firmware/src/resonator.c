/* SPDX-License-Identifier: GPL-3.0-only */
/* The FX resonators: Mutable Instruments Rings' resonator models, fed by the synth's voices the way Rings' IN jack
 * feeds its resonators (with IN patched Rings' own exciter is off: here too, the oscillators are the only excitation).
 * The DSP is Felucca's fixed-point port of Rings and DaisySP (Emilie Gillet, Electrosmith; MIT): phys_dsp.c,
 * phys_symp.c.
 *
 * Polyphonic, as Rings at polyphony 4: four resonators, each tuned to a note. A new note takes the resonator that
 * already holds it, else one no held note uses (the longest unused), else the oldest; the voice playing that note feeds
 * only its own resonator, so a chord rings as a chord and a resonator handed to a new note keeps its tail, retuned (as
 * Rings). A resonator that has had no input and gone quiet is not run (the cost is per resonator sounding); the
 * second core runs every other one of those sounding.
 *
 *   MODAL        a bank of 12 tuned band-pass modes: bells, bars, plates (Rings' "modal resonator")
 *   SYMP STRING  a string with three sympathetic strings tuned to a chord above the note (Rings' "sympathetic
 *                strings": STRUCTURE picks the chord: OCT, 5TH, 4TH, MAJ, MIN, SUS, 7TH, OCTAVES)
 *   STRING       an extended Karplus-Strong string (Rings' "inharmonic string": STRUCTURE below ~0.25 a curved
 *                bridge, sitar-like, above it more and more dispersion, piano-like)
 *
 *   STRUCTURE    the partials' stretch (MODAL), the chord (SYMP), bridge / dispersion (STRING)
 *   BRIGHTNESS   how long the high partials ring, the loop filter
 *   DAMPING      the decay: 0 short .. 127 rings on (as Rings, clockwise is longer)
 *   POSITION     the excitation point (MODAL), how much the sympathetic strings take (SYMP)
 *   MIX          dry .. wet (0: off, nothing runs)
 *
 *   rz_assign(note)              note on: the resonator for that note (the voice keeps it)
 *   fx_resonator(sum, dl, dr, n) audio ISR, core 0, after the voices: rz_bus[r][] each resonator's input (the voices
 *                                that feed it, Q15), sum[] their total (the dry), the output into sum[]; dl / dr what
 *                                BASIS takes off the left / right (synth.c), each resonator panned as its note */
#include "phys_dsp.c"
#include "phys_symp.c"
#include "waveguide.c"                           /* THROAT, FLUTE, SYRINX, HAWK: this firmware's own waveguides */

enum { RZ_MODAL, RZ_SYMP, RZ_STRING, RZ_THROAT, RZ_FLUTE, RZ_SYRINX, RZ_HAWK, RZ_COUNT };
static const char *const RZ_NAME[RZ_COUNT] = {"MODAL", "SYMP STRING", "STRING", "THROAT", "FLUTE", "SYRINX", "HAWK"};
static const char *const RZ_CHORD_NAME[8] = {"OCT", "5TH", "4TH", "MAJ", "MIN", "SUS", "7TH", "OCTAVES"};
static const int8_t RZ_CHORDS[8][PX_NSYMP] = {   /* semitones above the note (Felucca's PHYS chords) */
    {-12, 12, 24}, {7, 12, 19}, {5, 12, 17}, {4, 7, 12}, {3, 7, 12}, {2, 7, 14}, {4, 7, 10}, {12, 24, 36},
};
/* model 1.0 (Q20) -> Q15: measured so that the wet output is about as loud as the dry at every DAMPING (a longer
 * decay holds more energy: 20x for the modes from DAMPING 0 to 127, 6x for the strings): gain 2^(off - slope d),
 * octaves Q16 (tests/basis_test.c checks it) */
static const int32_t RZ_GAIN[RZ_COUNT] = {9000, 5200, 5200, 9000, 1800, 2600, 2600};   /* (THROAT: per animal, too) */
static const int32_t RZ_OFF[RZ_COUNT] = {229376, 0, 0, 0, 0, 0, 0};          /* 3.5, 0, 0 octaves */
static const int32_t RZ_SLOPE[RZ_COUNT] = {283771, 170394, 170394, 0, 0, 0, 0};   /* 4.33, 2.6, 2.6 octaves over the knob */
#define RZ_IN_SHIFT 5                            /* Q15 -> Q20 */

#define RZ_POLY 4                                /* resonators (Rings: 1, 2 or 4) */
#define RZ_QUIET 64                              /* blocks of no input and a quiet output (~46 ms): not run */
#define RZ_QUIET_Q20 256                         /* "quiet": the model's output under this (Q20, ~ -72 dB) */
typedef struct {
    union {
        px_modal_t m;
        px_string_t s;
        px_symp_t y;                             /* (its .s is the main string) */
        wg_throat_t t;                           /* waveguide.c */
        wg_flute_t f;
        wg_syrinx_t x;
    } u;
    uint8_t held;                                /* the model its state is for + 1 (0: nothing: reset at the next use) */
    uint8_t note;                                /* the note it is tuned to */
    uint16_t quiet;                              /* blocks it has been quiet (RZ_QUIET and up: not run) */
    uint32_t stamp;                              /* when a note last took it */
} rz_t;
static rz_t rz[RZ_POLY] __attribute__((section(".pool")));   /* (zeroed at start: held 0) */
static int32_t rz_bus[RZ_POLY][CTL];             /* each resonator's input this block, Q15 */
static int32_t rz_out[RZ_POLY][CTL];             /* each one's output, the model's units (Q20) */
static uint32_t rz_clock, rz_view_r;             /* note-on count; the resonator the screen shows */

/* for the screen (ui.c FX page; written here, read by the main loop: a torn read only draws one frame wrong) */
#define RZ_WAVE 256u                             /* the wet output, every 2nd sample (a power of two) */
static struct {
    uint32_t f[16];                              /* the resonances: frequencies, cycles Q32 (0: none) */
    int16_t a[16];                               /* their levels, Q15 */
    uint32_t n;
    int16_t wave[RZ_WAVE];
    volatile uint32_t w;
} rz_view;

static void rz_reset(rz_t *R, uint32_t md)
{
    memset(&R->u, 0, sizeof R->u);
    if (md == RZ_MODAL)
        R->u.m.rng = 0x9E3779B9u;
    else if (md == RZ_FLUTE)
        R->u.f.rng = 0x9E3779B9u;
    else if (md == RZ_SYRINX || md == RZ_HAWK) {
        R->u.x.rng = 0x9E3779B9u;
        R->u.x.quiet = 1;
    }
    else if (md == RZ_THROAT)
        R->u.t.ag = 4096;                        /* (the automatic gain at 1) */
    else {
        R->u.s.rng = 0x9E3779B9u;
        R->u.s.src = 1u << 30;
        R->u.s.nz_a = R->u.s.nz_b = 1u;
        if (md == RZ_SYMP)
            R->u.y.key[0] = -1;                  /* (the coefficients worked out at the first block) */
    }
    R->held = (uint8_t)(md + 1u);
}

/* note on (the audio ISR, core 0, before the render): the resonator for the note. The voices feeding it are the
 * notes that hold it: one whose note is held elsewhere is not taken while another is free */
static uint32_t rz_assign(uint32_t note)
{
    uint32_t r, k, best = RZ_POLY, used = 0;
    for (r = 0; r < RZ_POLY; r++)
        if (rz[r].stamp && rz[r].note == note)
            best = r;
    if (best == RZ_POLY) {
        for (k = 0; k < NVOICE; k++)             /* the resonators a held note plays into */
            if (voice[k].stage == ST_ATTACK || voice[k].stage == ST_SUSTAIN)
                used |= 1u << (voice[k].rz % RZ_POLY);
        if (used == (1u << RZ_POLY) - 1u)        /* (all held: the oldest of all) */
            used = 0;
        for (r = 0; r < RZ_POLY; r++)            /* the longest unused of the free ones (never used: stamp 0) */
            if (!(used >> r & 1u) && (best == RZ_POLY || rz[r].stamp < rz[best].stamp))
                best = r;
    }
    rz[best].note = (uint8_t)note;
    rz[best].stamp = ++rz_clock;
    rz_view_r = best;
    return best;
}

static int32_t rz_knee(int32_t s)                /* only peaks saturate: above 24000 a soft knee to 32767 */
{
    int32_t a = s < 0 ? -s : s;
    if (a > 24000) {
        uint32_t e = (uint32_t)(a - 24000) > 262143u ? 262143u : (uint32_t)(a - 24000);   /* (32-bit division: the */
        a = 24000 + (int32_t)(e * 8767u / (8767u + e));                                     /* toolchain has no 64) */
        s = s < 0 ? -a : a;
    }
    return s;
}

/* where each resonator is in the stereo field (synth.c: the pan, BASIS and ORBIT, of the newest voice feeding it,
 * kept while its tail rings on), Q15; the age of that voice */
static int32_t rz_epan[RZ_POLY];
static uint32_t rz_age[RZ_POLY];

/* resonator r over this block: rz_bus[r] in, rz_out[r] out. Each touches only its own state (and the screen's view,
 * the one resonator rz_view_r): the two cores run different ones */
static void rz_run(uint32_t r, uint32_t n)
{
    rz_t *R = &rz[r];
    uint32_t md = sys.fx_model % RZ_COUNT, i, note = R->note < 128u ? R->note : 48u, view = r == rz_view_r;
    int32_t strc = (int32_t)(sys.fx_struct & 127u) * 516, brit = (int32_t)(sys.fx_bright & 127u) * 516;
    int32_t damp = (int32_t)(sys.fx_damp & 127u) * 516, pos = (int32_t)(sys.fx_pos & 127u);
    int32_t in[CTL], ax[CTL], *y = rz_out[r], pk_in = 0, pk = 0;
    uint32_t f0 = note_inc(note);
    if (R->held != md + 1u)
        rz_reset(R, md);
    for (i = 0; i < n; i++) {
        int32_t a = rz_bus[r][i];
        pk_in |= a;
        in[i] = a * (1 << RZ_IN_SHIFT);
    }
    switch (md) {
    case RZ_MODAL: {
        px_modal_blk_t B;
        px_modal_block(&B, &R->u.m, f0, strc, brit, damp, 0, pos * 129, 0);   /* position 0..1/4 */
        px_modal_run(&B, &R->u.m, in, y, ax, n);
        if (view) {
            for (i = 0; i < B.n; i++) {
                int32_t a = B.m[i].a >> 13;          /* Q28 -> Q15 */
                rz_view.f[i] = B.f[i];
                rz_view.a[i] = (int16_t)clamp(a < 0 ? -a : a, 0, 32767);
            }
            rz_view.n = B.n;
        }
        break;
    }
    case RZ_STRING: {
        px_string_blk_t B;
        px_string_block(&B, &R->u.s, f0, strc, brit, damp, 0, 0, 0);
        px_string_run(&B, &R->u.s, in, y, n);
        if (view) {
            for (i = 0; i < 12u && (uint64_t)f0 * (i + 1u) < 0x7F000000u; i++) {   /* its partials (harmonic: */
                rz_view.f[i] = f0 * (i + 1u);                                        /* the screen's sketch) */
                rz_view.a[i] = (int16_t)(32767 / (int32_t)(i + 1u));
            }
            rz_view.n = i;
        }
        break;
    }
    case RZ_THROAT: {
        uint32_t an = (sys.fx_struct & 127u) * WA_COUNT / 128u, ns = WG_ANI[an].n, k = WG_ANI[an].k, m;
        wg_throat_run(&R->u.t, in, y, n, an, pos, brit, damp);
        if (view) {                              /* (the tube's quarter-wave resonances: a sketch of its formants) */
            for (m = 0; m < 8u; m++) {
                rz_view.f[m] = 0xFFFFFFFFu / (4u * ns * k + 2u) * (2u * m + 1u);   /* (32 bits: no 64-bit division) */
                rz_view.a[m] = (int16_t)(32767 / (int32_t)(m + 1u));
            }
            rz_view.n = 8;
        }
        break;
    }
    case RZ_FLUTE:
        wg_flute_run(&R->u.f, in, y, n, f0, strc / 516, pos * 516, brit, damp);
        if (view) {
            for (i = 0; i < 10u && (uint64_t)f0 * (i + 1u) < 0x7F000000u; i++) {
                rz_view.f[i] = f0 * (i + 1u);
                rz_view.a[i] = (int16_t)(32767 / (int32_t)(i * i + 1u));
            }
            rz_view.n = i;
        }
        break;
    case RZ_HAWK:
        wg_hawk_run(&R->u.x, in, y, n, f0, strc / 516, pos, brit, damp);
        if (view) {
            uint64_t a = ((uint64_t)f0 * px_exp2(5 * 32768)) >> 16;
            rz_view.f[0] = a < 0x7F000000u ? (uint32_t)a : 0u;
            rz_view.a[0] = 32767;
            rz_view.n = 1;
        }
        break;
    case RZ_SYRINX:
        wg_syrinx_run(&R->u.x, in, y, n, f0, strc / 516, pos, brit, damp);
        if (view) {
            uint64_t a = (uint64_t)f0 * 4u, b = ((uint64_t)f0 * 4u * px_exp2(pos * 301)) >> 16;
            rz_view.f[0] = a < 0x7F000000u ? (uint32_t)a : 0u;
            rz_view.a[0] = 32767;
            rz_view.f[1] = b < 0x7F000000u ? (uint32_t)b : 0u;
            rz_view.a[1] = -1;
            rz_view.n = 2;
        }
        break;
    default: {                                   /* SYMP: the string, then the strings it drives */
        px_string_blk_t B;
        uint32_t f[PX_NSYMP], k, c = (uint32_t)(sys.fx_struct & 127u) >> 4;
        for (k = 0; k < PX_NSYMP; k++) {
            int32_t nn = (int32_t)note + RZ_CHORDS[c][k];
            uint32_t fk = nn < 0 || nn > 127 ? 0u : note_inc((uint32_t)nn);
            f[k] = k == 0u ? fk + (fk >> 9) : k == 2u ? fk - (fk >> 9) : fk;   /* (+-3 cents: the strings beat) */
        }
        px_string_block(&B, &R->u.y.s, f0, 16384, brit, damp, 0, 0, 0);      /* STRUCTURE 0.25: a plain string */
        px_string_run(&B, &R->u.y.s, in, y, n);
        px_symp_block(&R->u.y, f, brit, damp, pos * 516);
        px_symp_run(&R->u.y, y, y, n);
        if (view) {
            uint32_t m = 0;
            for (i = 0; i < 8u && (uint64_t)f0 * (i + 1u) < 0x7F000000u; i++) {
                rz_view.f[m] = f0 * (i + 1u);
                rz_view.a[m++] = (int16_t)(32767 / (int32_t)(i + 1u));
            }
            for (k = 0; k < PX_NSYMP; k++)      /* the sympathetic strings: full height, the screen marks them */
                if (f[k] && f[k] < 0x7F000000u) {
                    rz_view.f[m] = f[k];
                    rz_view.a[m++] = -1;
                }
            rz_view.n = m;
        }
        break;
    }
    }
    for (i = 0; i < n; i++) {
        int32_t a = y[i] < 0 ? -y[i] : y[i];
        pk = a > pk ? a : pk;
    }
    if (pk_in || pk >= RZ_QUIET_Q20)
        R->quiet = 0;
    else if (R->quiet < 0xFFFFu)
        R->quiet++;
}

static void c1_resonators(uint32_t arg)          /* the second core: arg the resonators (bit r) | n << 8 */
{
    uint32_t r;
    for (r = 0; r < RZ_POLY; r++)
        if (arg >> r & 1u)
            rz_run(r, arg >> 8);
}

static void fx_resonator(int32_t *sum, int32_t *dl, int32_t *dr, uint32_t n)
{
    uint32_t md = sys.fx_model % RZ_COUNT, i, r, run = 0, mine = 0, busy = 0;
    int32_t pl[RZ_POLY], pr[RZ_POLY], basis = 0;
    int32_t mix = sys.fx_mix > 127u ? 127 : (int32_t)sys.fx_mix;
    if (!mix) {
        for (r = 0; r < RZ_POLY; r++) {          /* (off: the next use starts from silence) */
            rz[r].held = 0;
            rz[r].quiet = RZ_QUIET;
        }
        return;
    }
    if (n > CTL)
        n = CTL;
    for (r = 0; r < RZ_POLY; r++) {              /* the ones sounding or fed: every other one to the second core */
        uint32_t fed = 0;
        for (i = 0; i < n && !fed; i++)
            fed = rz_bus[r][i] != 0;
        if (rz[r].held != md + 1u)               /* (a new model: state anew, quiet until fed) */
            rz[r].quiet = fed ? 0u : RZ_QUIET;
        if (!fed && rz[r].quiet >= RZ_QUIET)
            continue;
        if (busy++ & 1u)
            run |= 1u << r;
        else
            mine |= 1u << r;
    }
    if (!C1_ON() || !run || sys.cpu2 == 2u || !C1_RUN(c1_resonators, run | n << 8)) {   /* (2ND CORE VOICES: here) */
        mine |= run;
        run = 0;
    }
    for (r = 0; r < RZ_POLY; r++)
        if (mine >> r & 1u)
            rz_run(r, n);
    if (run && C1_WAIT())                        /* (it stopped answering: its resonators here) */
        c1_resonators(run | n << 8);
    mine |= run;
    for (r = 0; r < RZ_POLY; r++) {              /* BASIS: each resonator where its note is (what it takes off L, R) */
        int32_t p = rz_epan[r];
        pl[r] = p > 0 ? p : 0;
        pr[r] = p < 0 ? -p : 0;
        basis |= p;                              /* (all in the middle: nothing to pan) */
    }
    mix += mix >> 6;                             /* 0..128 */
    {
        int32_t oct = RZ_OFF[md] - (int32_t)(((int64_t)RZ_SLOPE[md] * (sys.fx_damp & 127u)) / 127);
        int32_t g = (int32_t)(((int64_t)RZ_GAIN[md] * px_exp2(oct)) >> 16);
        for (i = 0; i < n; i++) {
            int32_t y = 0, yl = 0, yr = 0, wet;
            for (r = 0; r < RZ_POLY; r++)
                if (mine >> r & 1u) {
                    y += rz_out[r][i];
                    if (basis) {
                        yl += (int32_t)(((int64_t)rz_out[r][i] * pl[r]) >> 15);
                        yr += (int32_t)(((int64_t)rz_out[r][i] * pr[r]) >> 15);
                    }
                }
            wet = rz_knee(px_m(y, g, 20));
            sum[i] = (sum[i] * (128 - mix) + wet * mix) >> 7;
            if (basis) {                         /* the dry's panning faded as the dry, the wet's added */
                int32_t wl = wet - rz_knee(px_m(y - yl, g, 20)), wr = wet - rz_knee(px_m(y - yr, g, 20));
                dl[i] = (dl[i] * (128 - mix) + wl * mix) >> 7;
                dr[i] = (dr[i] * (128 - mix) + wr * mix) >> 7;
            }
            if (i & 1u)
                rz_view.wave[rz_view.w++ & (RZ_WAVE - 1u)] = (int16_t)clamp(wet, -32767, 32767);
        }
    }
}
