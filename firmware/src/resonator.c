/* SPDX-License-Identifier: GPL-3.0-only */
/* The FX resonators: Mutable Instruments Rings' resonator models, fed by the synth's output the way Rings' IN jack
 * feeds its resonator (with IN patched Rings' own exciter is off: here too, the circuit is the only excitation).
 * The DSP is Felucca's fixed-point port of Rings and DaisySP (Emilie Gillet, Electrosmith; MIT): phys_dsp.c,
 * phys_symp.c. One resonator (Rings' polyphony 1), tuned to the newest note.
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
 *   fx_resonator(sum, n)   audio ISR, core 0, after the voices: sum[] Q15 mono, processed in place */
#include "phys_dsp.c"
#include "phys_symp.c"

enum { RZ_MODAL, RZ_SYMP, RZ_STRING, RZ_COUNT };
static const char *const RZ_NAME[RZ_COUNT] = {"MODAL", "SYMP STRING", "STRING"};
static const char *const RZ_CHORD_NAME[8] = {"OCT", "5TH", "4TH", "MAJ", "MIN", "SUS", "7TH", "OCTAVES"};
static const int8_t RZ_CHORDS[8][PX_NSYMP] = {   /* semitones above the note (Felucca's PHYS chords) */
    {-12, 12, 24}, {7, 12, 19}, {5, 12, 17}, {4, 7, 12}, {3, 7, 12}, {2, 7, 14}, {4, 7, 10}, {12, 24, 36},
};
/* model 1.0 (Q20) -> Q15: measured so that the wet output is about as loud as the dry at every DAMPING (a longer
 * decay holds more energy: 20x for the modes from DAMPING 0 to 127, 6x for the strings): gain 2^(off - slope d),
 * octaves Q16 (tests/basis_test.c checks it) */
static const int32_t RZ_GAIN[RZ_COUNT] = {9000, 5200, 5200};
static const int32_t RZ_OFF[RZ_COUNT] = {229376, 0, 0};          /* 3.5, 0, 0 octaves */
static const int32_t RZ_SLOPE[RZ_COUNT] = {283771, 170394, 170394};   /* 4.33, 2.6, 2.6 octaves over the knob */
#define RZ_IN_SHIFT 5                            /* Q15 -> Q20 */

static union {
    px_modal_t m;
    px_string_t s;
    px_symp_t y;                                 /* (its .s is the main string) */
} rz;
static uint8_t rz_model = 0xFF;                  /* what rz holds (0xFF: nothing, reset at the next use) */

/* for the screen (ui.c FX page; written here, read by the main loop: a torn read only draws one frame wrong) */
#define RZ_WAVE 256u                             /* the wet output, every 2nd sample (a power of two) */
static struct {
    uint32_t f[16];                              /* the resonances: frequencies, cycles Q32 (0: none) */
    int16_t a[16];                               /* their levels, Q15 */
    uint32_t n;
    int16_t wave[RZ_WAVE];
    volatile uint32_t w;
} rz_view;

static void rz_reset(uint32_t md)
{
    memset(&rz, 0, sizeof rz);
    if (md == RZ_MODAL)
        rz.m.rng = 0x9E3779B9u;
    else {
        rz.s.rng = 0x9E3779B9u;
        rz.s.src = 1u << 30;
        rz.s.nz_a = rz.s.nz_b = 1u;
        if (md == RZ_SYMP)
            rz.y.key[0] = -1;                    /* (the coefficients worked out at the first block) */
    }
    rz_model = (uint8_t)md;
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

static void fx_resonator(int32_t *sum, uint32_t n)
{
    uint32_t md = sys.fx_model % RZ_COUNT, i, note = sys.last_note < 128u ? sys.last_note : 48u;
    int32_t mix = sys.fx_mix > 127u ? 127 : (int32_t)sys.fx_mix, in[CTL], y[CTL], ax[CTL];
    int32_t strc = (int32_t)(sys.fx_struct & 127u) * 516, brit = (int32_t)(sys.fx_bright & 127u) * 516;
    int32_t damp = (int32_t)(sys.fx_damp & 127u) * 516, pos = (int32_t)(sys.fx_pos & 127u);
    uint32_t f0 = note_inc(note);
    if (!mix) {
        rz_model = 0xFF;                         /* (off: the next use starts from silence) */
        return;
    }
    if (n > CTL)
        n = CTL;
    if (md != rz_model)
        rz_reset(md);
    for (i = 0; i < n; i++)
        in[i] = sum[i] * (1 << RZ_IN_SHIFT);
    switch (md) {
    case RZ_MODAL: {
        px_modal_blk_t B;
        px_modal_block(&B, &rz.m, f0, strc, brit, damp, 0, pos * 129, 0);   /* position 0..1/4 */
        px_modal_run(&B, &rz.m, in, y, ax, n);
        for (i = 0; i < B.n; i++) {
            int32_t a = B.m[i].a >> 13;              /* Q28 -> Q15 */
            rz_view.f[i] = B.f[i];
            rz_view.a[i] = (int16_t)clamp(a < 0 ? -a : a, 0, 32767);
        }
        rz_view.n = B.n;
        break;
    }
    case RZ_STRING: {
        px_string_blk_t B;
        px_string_block(&B, &rz.s, f0, strc, brit, damp, 0, 0, 0);
        px_string_run(&B, &rz.s, in, y, n);
        for (i = 0; i < 12u && (uint64_t)f0 * (i + 1u) < 0x7F000000u; i++) {   /* its partials (harmonic: */
            rz_view.f[i] = f0 * (i + 1u);                                        /* the screen's sketch) */
            rz_view.a[i] = (int16_t)(32767 / (int32_t)(i + 1u));
        }
        rz_view.n = i;
        break;
    }
    default: {                                   /* SYMP: the string, then the strings it drives */
        px_string_blk_t B;
        uint32_t f[PX_NSYMP], k, c = (uint32_t)(sys.fx_struct & 127u) >> 4;
        for (k = 0; k < PX_NSYMP; k++) {
            int32_t nn = (int32_t)note + RZ_CHORDS[c][k];
            uint32_t fk = nn < 0 || nn > 127 ? 0u : note_inc((uint32_t)nn);
            f[k] = k == 0u ? fk + (fk >> 9) : k == 2u ? fk - (fk >> 9) : fk;   /* (+-3 cents: the strings beat) */
        }
        px_string_block(&B, &rz.y.s, f0, 16384, brit, damp, 0, 0, 0);      /* STRUCTURE 0.25: a plain string */
        px_string_run(&B, &rz.y.s, in, y, n);
        px_symp_block(&rz.y, f, brit, damp, pos * 516);
        px_symp_run(&rz.y, y, y, n);
        rz_view.n = 0;
        for (i = 0; i < 8u && (uint64_t)f0 * (i + 1u) < 0x7F000000u; i++) {
            rz_view.f[rz_view.n] = f0 * (i + 1u);
            rz_view.a[rz_view.n++] = (int16_t)(32767 / (int32_t)(i + 1u));
        }
        for (k = 0; k < PX_NSYMP; k++)          /* the sympathetic strings: full height, the screen marks them */
            if (f[k] && f[k] < 0x7F000000u) {
                rz_view.f[rz_view.n] = f[k];
                rz_view.a[rz_view.n++] = -1;
            }
        break;
    }
    }
    mix += mix >> 6;                             /* 0..128 */
    {
        int32_t oct = RZ_OFF[md] - (int32_t)(((int64_t)RZ_SLOPE[md] * (sys.fx_damp & 127u)) / 127);
        int32_t g = (int32_t)(((int64_t)RZ_GAIN[md] * px_exp2(oct)) >> 16);
    for (i = 0; i < n; i++) {
        int32_t wet = rz_knee(px_m(y[i], g, 20));
        sum[i] = (sum[i] * (128 - mix) + wet * mix) >> 7;
        if (i & 1u)
            rz_view.wave[rz_view.w++ & (RZ_WAVE - 1u)] = (int16_t)clamp(wet, -32767, 32767);
    }
    }
}
