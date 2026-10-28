/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the keyboard (firmware/src/keys.c) and the synth (synth.c), the same sources as the firmware,
 * against stubs of the key scan and the USB-MIDI rings. Checks: a key plays its note at the octave set and sends
 * MIDI, the octave changing under a held key, notes held by several sources, MIDI in (note on, velocity 0, CC 123,
 * a ring overflow), the boot hold, pitch, levels, voice stealing, silence at MASTER 0.
 *   tests/basis_test [OUT.wav]   (OUT.wav: a short render of the keys, to listen to) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define memset felucca_memset              /* (libc.c's own, beside the host's) */
#define memcpy felucca_memcpy
#define memcmp felucca_memcmp
#include "../firmware/src/libc.c"
#undef memset
#undef memcpy
#undef memcmp
static struct { volatile uint32_t notes, buttons; } fm1_in;

#include "../firmware/src/core.h"

/* the USB-MIDI rings of usb.c */
#define MQ 64u
static uint32_t midi_in_q[MQ];
static volatile uint32_t mi_w, mi_r;
static volatile uint8_t midi_in_overflow;
static uint32_t out_log[256], out_n;
static void midi_out_event(uint32_t pkt) { if (out_n < 256u) out_log[out_n++] = pkt; }
static void midi_in(uint32_t st, uint32_t d1, uint32_t d2)
{
    midi_in_q[mi_w % MQ] = (st >> 4) | st << 8 | d1 << 16 | d2 << 24;
    mi_w++;
}

/* the second core: off (0), run at once (1), a real second thread (2: under ThreadSanitizer, a race between the two
 * halves of a block shows), or one that never answers (3: C1_WAIT gives up, this core renders its voices) */
#include <pthread.h>
static int c1_mode;
static void (*c1_fn)(uint32_t);
static uint32_t c1_arg;
static pthread_t c1_thread;
static void *c1_thread_main(void *p) { (void)p; c1_fn(c1_arg); return 0; }
static int c1_run(void (*fn)(uint32_t), uint32_t arg)
{
    c1_fn = fn;
    c1_arg = arg;
    if (c1_mode == 1)
        fn(arg);
    else if (c1_mode == 2)
        pthread_create(&c1_thread, 0, c1_thread_main, 0);
    return 1;
}
static int c1_wait(void)
{
    if (c1_mode == 2)
        pthread_join(c1_thread, 0);
    return c1_mode == 3 ? -1 : 0;
}
#define C1_ON() (c1_mode != 0)
#define C1_RUN(fn, arg) c1_run(fn, arg)
#define C1_WAIT() c1_wait()
#include "../firmware/src/synth.c"
#include "../firmware/src/keys.c"

static int bad;
static void check(const char *what, int ok)
{
    printf("%-62s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

static int32_t buf[2 * CTL];
static int32_t peak;
static FILE *wav;
static uint32_t wav_frames;
static void block(void)                      /* audio.c's audio_block, minus the USB tap and the 24-bit shift */
{
    uint32_t i;
    keys_block();
    synth_render(buf, CTL);
    for (i = 0; i < 2u * CTL; i++) {
        int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > peak)
            peak = a;
    }
    if (wav) {
        for (i = 0; i < CTL; i++) {
            int16_t s[2] = {(int16_t)buf[2 * i], (int16_t)buf[2 * i + 1]};
            fwrite(s, 2, 2, wav);
        }
        wav_frames += CTL;
    }
}
static void run_ms(uint32_t ms)
{
    uint32_t n = ms * FS / 1000u / CTL + 1u;
    while (n--)
        block();
}
static uint32_t sounding(void) { uint32_t i, n = 0; for (i = 0; i < NVOICE; i++) n += voice[i].stage != ST_OFF; return n; }
static uint32_t key_of(uint32_t note) { return note - KEY_BASE_NOTE; }   /* at octave 0 */
static int sent(uint32_t i, uint32_t status, uint32_t note, uint32_t vel)
{
    uint32_t p = out_log[i];
    return i < out_n && ((p >> 8) & 0xFFu) == status && ((p >> 16) & 0x7Fu) == note && ((p >> 24) & 0x7Fu) == vel;
}

/* upward zero crossings of the output over 1 s: the pitch of a periodic regime */
static double measure_hz(uint32_t note)
{
    uint32_t frames = 0, cross = 0, i;
    int32_t prev = 0;
    synth_kill();
    sys.attack = 0;
    synth_note_on(note, 127);
    run_ms(300);                                 /* (onto the cycle, the DC blocker settled) */
    while (frames < FS) {
        synth_render(buf, CTL);
        for (i = 0; i < CTL; i++, frames++) {
            if (prev < 0 && buf[2 * i] >= 0)
                cross++;
            prev = buf[2 * i];
        }
    }
    synth_kill();
    return cross;
}

/* the same solver in double precision: the fixed-point one must follow it */
static void ref_step(double *s, double h, double a, double b)
{
    double m[3], dx, dy, dz;
#define G(x) (fabs(x) < 1 ? -8.0 / 7 * (x) : fabs(x) < 3 ? ((x) < 0 ? -1 : 1) * (-8.0 / 7 - 5.0 / 7 * (fabs(x) - 1)) \
                                                       : ((x) < 0 ? -1 : 1) * (-8.0 / 7 - 10.0 / 7 + 2 * (fabs(x) - 3)))
    dx = h * a * (s[1] - s[0] - G(s[0])); dy = h * (s[0] - s[1] + s[2]); dz = -h * b * s[1];
    m[0] = s[0] + dx / 2; m[1] = s[1] + dy / 2; m[2] = s[2] + dz / 2;
    s[0] += h * a * (m[1] - m[0] - G(m[0])); s[1] += h * (m[0] - m[1] + m[2]); s[2] += -h * b * m[1];
#undef G
}

static void wav_hdr(FILE *f, uint32_t frames)
{
    uint32_t v;
    uint16_t a = 1, ch = 2, ba = 4, bits = 16;
    uint32_t sr = FS, br = FS * 4u;
    fwrite("RIFF", 1, 4, f); v = 36 + frames * 4; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    fwrite(&a, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); v = frames * 4; fwrite(&v, 4, 1, f);
}

int main(int argc, char **argv)
{
    uint32_t k, w;
    keys_init();
    sys.master_q12 = 4096;

    /* a key: its note (octave 0: key 0 = F3 = 53), MIDI out on channel 1, sound, release */
    fm1_in.notes = 1u << key_of(60);
    run_ms(10);
    check("key C4 down: note 60 sounds", sounding() == 1 && voice[0].note == 60 && sys.last_note == 60);
    check("... and goes out as MIDI note on 90 3C 64", out_n == 1 && sent(0, 0x90, 60, KEY_VEL));
    peak = 0;
    run_ms(100);
    check("... at a sane level (peak 2000..32767)", peak > 2000 && peak <= 32767);
    fm1_in.notes = 0;
    run_ms(5);
    check("key up: MIDI note off 80 3C", out_n == 2 && sent(1, 0x80, 60, 0));
    run_ms(2000);
    check("... the release ends the voice", sounding() == 0);

    /* the octave changes while a key is held: the key-up ends the note it started */
    out_n = 0;
    sys.octave = 1;
    fm1_in.notes = 1u << key_of(60);
    run_ms(10);
    sys.octave = -1;
    fm1_in.notes = 0;
    run_ms(10);
    check("octave +1: the key plays 72; changed to -1 while held, 72 still ends",
          sent(0, 0x90, 72, KEY_VEL) && sent(1, 0x80, 72, 0) && voice[0].stage == ST_RELEASE);
    sys.octave = 0;
    run_ms(2000);

    /* the lowest and the highest key */
    out_n = 0;
    fm1_in.notes = 1u | 1u << 26;
    run_ms(10);
    fm1_in.notes = 0;
    run_ms(10);
    check("keys 0 and 26: F3 (53) and G5 (79)",
          out_n == 4 && sent(0, 0x90, 53, KEY_VEL) && sent(1, 0x90, 79, KEY_VEL));
    run_ms(2000);

    /* a note held by a key and by MIDI ends with the last of them */
    fm1_in.notes = 1u << key_of(64);
    midi_in(0x90, 64, 90);
    run_ms(10);
    fm1_in.notes = 0;
    run_ms(10);
    check("key + MIDI on the same note: the key let go, it still sounds", voice[0].note == 64 && voice[0].stage == ST_SUSTAIN);
    midi_in(0x80, 64, 0);
    run_ms(10);
    check("... MIDI note off ends it", voice[0].stage == ST_RELEASE);
    run_ms(2000);

    /* MIDI in: note on, velocity 0 as note off, CC 123 */
    midi_in(0x93, 48, 127);
    midi_in(0x95, 55, 64);
    run_ms(10);
    check("MIDI in, any channel: two notes", sounding() == 2 && sys.last_vel == 64);
    midi_in(0x93, 48, 0);
    run_ms(10);
    check("note on with velocity 0 is a note off", note_held[48] == 0 && note_held[55] == 1);
    midi_in(0xB0, 123, 0);
    run_ms(10);
    check("CC 123 releases everything", note_held[55] == 0 && voice[0].stage != ST_SUSTAIN && voice[1].stage != ST_SUSTAIN);
    run_ms(2000);

    /* a ring overflow: everything let go */
    midi_in(0x90, 50, 100);
    run_ms(10);
    midi_in_overflow = 1;
    run_ms(10);
    check("MIDI ring overflow: held notes released, ring emptied",
          note_held[50] == 0 && mi_r == mi_w && !midi_in_overflow);
    run_ms(2000);

    /* the boot hold: keys pressed under the splash are silent, down and up */
    out_n = 0;
    kb_boot_hold = 1;
    fm1_in.notes = 1u << 3;
    run_ms(10);
    kb_boot_hold = 0;
    run_ms(10);
    fm1_in.notes = 0;
    run_ms(10);
    check("a key held from boot: no note, no MIDI, not even on release", out_n == 0 && sounding() == 0);

    /* every key at once: NVOICE voices, no more, in every regime */
    fm1_in.notes = (1u << NKEYS) - 1u;
    peak = 0;
    for (k = 0; k < 128u; k += 16u) {
        sys.chaos = (uint8_t)k;
        run_ms(100);
    }
    check("27 keys at once: 8 voices sound, the output stays within 16 bits", sounding() == NVOICE && peak <= 32767);
    fm1_in.notes = 0;
    run_ms(3000);
    check("... all released", sounding() == 0);

    /* the circuit: fixed point against double precision, the same midpoint solver, the same start; 2 time units
     * (chaos parts the two exponentially after that: then only the statistics can agree) */
    {
        static const uint8_t KN[][2] = {{4, 64}, {40, 64}, {54, 20}, {85, 76}, {124, 40}, {100, 127}};
        double worst = 0;
        for (k = 0; k < sizeof KN / sizeof KN[0]; k++) {
            chua_t s = {C1 * 3 / 2, C1 / 10, -C1};
            double r[3] = {1.5, 0.1, -1.0}, a, b, h = 0.01;
            int32_t hs = (int32_t)(h * (1 << 24) + 0.5), ahs, bhs;
            sys.chaos = KN[k][0];
            sys.shape = KN[k][1];
            cc.valid = 0;
            chua_params();
            a = cc.alpha / 65536.0;
            b = cc.beta / 65536.0;
            ahs = (int32_t)(((int64_t)cc.alpha * hs) >> 16);
            bhs = (int32_t)(((int64_t)cc.beta * hs) >> 16);
            for (w = 0; w < 200u; w++) {
                double e;
                chua_step(&s, hs, ahs, bhs);
                ref_step(r, h, a, b);
                e = fabs(s.x / 1048576.0 - r[0]) + fabs(s.y / 1048576.0 - r[1]) + fabs(s.z / 1048576.0 - r[2]);
                worst = e > worst ? e : worst;
            }
        }
        printf("   (largest difference over 2 time units: %.2e)\n", worst);
        check("fixed-point circuit follows the double-precision one (< 1e-3)", worst < 1e-3);
    }

    /* every regime, shape and range of notes: the solver stays in range (no restarts), the output in 16 bits */
    {
        static const uint8_t NOTES[] = {24, 48, 60, 72, 96, 120};
        uint32_t c, sh, ni;
        chua_restarts = 0;
        peak = 0;
        sys.attack = 0;
        for (c = 0; c < 128u; c += 8u)
            for (sh = 0; sh < 128u; sh += 42u) {
                sys.chaos = (uint8_t)c;
                sys.shape = (uint8_t)sh;
                for (ni = 0; ni < sizeof NOTES; ni++)
                    synth_note_on(NOTES[ni], 127);
                run_ms(150);
                synth_kill();
            }
        printf("   (restarts %u, peak %d)\n", chua_restarts, peak);
        check("CHAOS x SHAPE x notes 24..120: no solver restart, output within 16 bits", chua_restarts == 0 && peak <= 32767);
    }

    /* pitch: in the CYCLE regime the circuit is periodic, and the tables' time scale puts it on the note */
    {
        double a3, a4, c2;
        sys.chaos = 4;
        sys.output = 64;                         /* y: the cleanest */
        for (k = 0; k < 128u; k += 63u) {
            char what[80];
            sys.shape = (uint8_t)k;
            a3 = measure_hz(57);
            a4 = measure_hz(69);
            c2 = measure_hz(36);
            snprintf(what, sizeof what, "CYCLE regime, SHAPE %3u: A3 %.0f, A4 %.0f, C2 %.1f Hz (within 2 %%)", k, a3, a4, c2);
            check(what, fabs(a3 / 220 - 1) < 0.02 && fabs(a4 / 440 - 1) < 0.02 && fabs(c2 / 65.41 - 1) < 0.02);
        }
        sys.output = 0;
    }

    /* the second core: the same output, bit for bit, whichever core renders which voice */
    {
        static int32_t ref[2 * CTL * 2000];
        static const uint8_t CH[] = {48, 52, 55, 59, 62, 65, 69, 72};
        int mode, same = 1;
        uint32_t blocks = 0;
        for (mode = 0; mode < 4; mode++) {
            uint32_t b, j;
            synth_kill();
            memset(voice, 0, sizeof voice);
            ghost.x = C1 / 10; ghost.y = ghost.z = 0;
            rng_state = 0x1234567u;
            voice_clock = 0;
            c1_blocks = 0;
            memset(rz, 0, sizeof rz); rz_clock = rz_view_r = 0;
            sys.fx_mix = 0; sys.fx_model = RZ_MODAL; sys.fx_struct = 70; sys.fx_damp = 100;
            c1_mode = mode;
            sys.chaos = 85; sys.shape = 76; sys.output = 32; sys.attack = 2; sys.release = 30; sys.osc = 0;
            sys.mod_depth = 0;
            for (b = 0; b < 2000u; b++) {
                if (b % 250u == 0u)
                    for (j = 0; j < sizeof CH; j++)
                        (b / 250u + j) & 1u ? synth_note_on(CH[j] + b / 250u, 100) : synth_note_off(CH[j] + b / 250u - 1u);
                if (b == 1000u)
                    sys.chaos = 120;
                if (b == 300u)
                    sys.fx_mix = 100;            /* the resonators (four: half on the second core), */
                if (b == 800u)
                    sys.fx_model = RZ_SYMP;      /* the sympathetic strings */
                if (b == 1400u)
                    sys.osc = 7;                 /* noise programs under held notes: GRAIN GLITCH (its buffer), */
                if (b == 1700u)
                    sys.osc = 14;                /* SINE FM CLUST, */
                if (b == 1850u) {
                    sys.mod_depth = 90;          /* the FM oscillator on it */
                    sys.mod_wave = 70; sys.mod_ratio = 9;
                }
                synth_render(buf, CTL);
                for (j = 0; j < 2u * CTL; j++) {
                    if (mode == 0)
                        ref[b * 2u * CTL + j] = buf[j];
                    else
                        same &= ref[b * 2u * CTL + j] == buf[j];
                }
            }
            if (mode == 1)
                blocks = c1_blocks;
        }
        c1_mode = 0;
        sys.osc = 0;
        sys.fx_mix = 0;
        sys.mod_depth = 0;
        printf("   (blocks with voices on the second core: %u of 2000)\n", blocks);
        check("second core (at once, a thread, one that gives up): the same output bit for bit", same && blocks > 1000u);
    }

    /* the noise programs (noise.c): each sounds, at about the circuit's level at X = Y = 64, and stays bounded and
     * without DC in every corner of X and Y, chords included; switching under held notes is clean */
    {
        static const uint8_t XY[5][2] = {{64, 64}, {0, 0}, {127, 0}, {0, 127}, {127, 127}};
        static const uint8_t CH[4] = {48, 55, 60, 67};
        uint32_t a, c, j, b, okk = 1;
        double lo = 1e9, hi = 0;
        sys.attack = 0; sys.decay = 64; sys.sustain = 127; sys.release = 20; sys.ring = 50;
        sys.fx_mix = 0; sys.sh_mix = 0; sys.np_level = 64;
        if (getenv("NP_LEVELS")) {                       /* the circuit, the same way: the level to match */
            double e = 0;
            synth_kill(); run_ms(50); sys.osc = 0;
            for (j = 0; j < 4u; j++) synth_note_on(CH[j], 110);
            run_ms(200);
            for (b = 0; b < 400u; b++) { synth_render(buf, CTL); for (j = 0; j < CTL; j++) e += (double)buf[2 * j] * buf[2 * j]; }
            printf("   LEVEL CHUA %.0f\n", sqrt(e / (400.0 * CTL)));
        }
        for (a = 0; a < NP_COUNT; a++) {
            for (c = 0; c < 5u; c++) {
                double e = 0, dc = 0;
                int32_t pk = 0;
                synth_kill();
                run_ms(50);
                sys.osc = (uint8_t)(a + 1u);
                sys.np_x = XY[c][0];
                sys.np_y = XY[c][1];
                for (j = 0; j < 4u; j++)
                    synth_note_on(CH[j], 110);
                run_ms(200);
                for (b = 0; b < 400u; b++) {                /* ~290 ms */
                    synth_render(buf, CTL);
                    for (j = 0; j < CTL; j++) {
                        int32_t v = buf[2 * j], av = v < 0 ? -v : v;
                        e += (double)v * v;
                        dc += v;
                        pk = av > pk ? av : pk;
                    }
                }
                e = sqrt(e / (400.0 * CTL));
                dc /= 400.0 * CTL;
                if (c == 0u && getenv("NP_LEVELS"))
                    printf("   LEVEL %u %.0f\n", a, e);
                if (c == 0u) {
                    lo = e < lo ? e : lo;
                    hi = e > hi ? e : hi;
                    if (e < 2700 || e > 11000) {             /* (the circuit: ~5400) */
                        printf("   %s: RMS %.0f at X = Y = 64\n", NP_NAME[a], e);
                        okk = 0;
                    }
                }
                if (e < 50 || fabs(dc) > 3000 || pk > 32767) {
                    printf("   %s X %u Y %u: RMS %.0f DC %.0f peak %d\n", NP_NAME[a], XY[c][0], XY[c][1], e, dc, pk);
                    okk = 0;
                }
            }
        }
        printf("   (noise programs at X = Y = 64: RMS %.0f .. %.0f)\n", lo, hi);
        check("noise: all 30 programs sound, levelled, bounded, no DC in X / Y corners", okk);
        synth_kill();
        sys.osc = 0;
        sys.np_x = sys.np_y = sys.np_level = 64;
    }

    /* the FM oscillator (MOD): at DEPTH 0 its other knobs change nothing, bit for bit; at full DEPTH, every wave and
     * the lowest and highest ratio, every program changes, sounds and stays bounded without DC */
    {
        static const uint8_t CH[3] = {48, 60, 67};
        static int32_t ref[2 * CTL * 150];
        uint32_t a, wv, rt, j, b, okk = 1, same = 1, moved = 1;
        sys.attack = 0; sys.decay = 64; sys.sustain = 127; sys.release = 20; sys.ring = 50;
        sys.fx_mix = 0; sys.sh_mix = 0; sys.np_level = 64; sys.np_x = sys.np_y = 64;
        for (a = 0; a < NP_COUNT; a++) {
            for (wv = 0; wv < 6u; wv++) {
                double e = 0, dc = 0;
                int32_t pk = 0;
                uint32_t diff = 0;
                synth_kill(); memset(voice, 0, sizeof voice); rng_state = 0x1234567u;
                sys.osc = (uint8_t)(a + 1u);
                sys.mod_depth = wv < 2u ? 0 : 127;               /* 0, 1: off (the other knobs moved); 2..5: on */
                sys.mod_wave = (uint8_t)(wv == 1u ? 127 : (wv - 2u) * 42u + (wv == 5u));
                sys.mod_ratio = (uint8_t)(wv == 1u ? 15 : wv & 1u ? 0 : NP_NRATIO - 1u);
                sys.mod_fine = wv == 1u ? 0 : 64;
                for (j = 0; j < 3u; j++)
                    synth_note_on(CH[j], 110);
                for (b = 0; b < 150u; b++) {
                    synth_render(buf, CTL);
                    for (j = 0; j < CTL; j++) {
                        int32_t v = buf[2 * j], av = v < 0 ? -v : v;
                        if (wv == 0u) ref[b * 2u * CTL + j] = v;
                        else if (wv == 1u) same &= ref[b * 2u * CTL + j] == v;
                        else diff += ref[b * 2u * CTL + j] != v;
                        if (b >= 50u) { e += (double)v * v; dc += v; pk = av > pk ? av : pk; }
                    }
                }
                e = sqrt(e / (100.0 * CTL));
                dc /= 100.0 * CTL;
                if (wv >= 2u && diff < CTL * 10u)
                    moved = 0, printf("   FM: %s wave %u changes only %u samples\n", NP_NAME[a], wv, diff);
                if (e < 50 || fabs(dc) > 3000 || pk > 32767) {
                    printf("   FM: %s wave %u: RMS %.0f DC %.0f peak %d\n", NP_NAME[a], wv, e, dc, pk);
                    okk = 0;
                }
            }
        }
        for (rt = 0; rt < 128u; rt++)                            /* the morph: continuous, bounded */
            for (j = 0; j < 64u; j++) {
                int32_t w0 = np_wave(j << 26, rt), w1 = np_wave(j << 26, rt + 1u);   /* (the knob turned a step) */
                okk &= w0 >= -32768 && w0 <= 32767 && (rt == 127u || abs(w1 - w0) < 3000);
            }
        {   /* MOD ENV: a pluck (attack 0, sustain 0): the FM's effect fades with it; held at full sustain it stays */
            double ed[2][2];
            uint32_t m2;
            for (m2 = 0; m2 < 2u; m2++) {
                uint32_t ph2;
                synth_kill(); memset(voice, 0, sizeof voice);
                sys.osc = 4; sys.mod_depth = 127; sys.mod_wave = 0; sys.mod_ratio = 3; sys.mod_fine = 64;
                sys.menv_a = 0; sys.menv_d = 40; sys.menv_s = m2 ? 127 : 0; sys.menv_r = 40;
                synth_note_on(60, 110);
                for (ph2 = 0; ph2 < 2u; ph2++) {         /* how far the voice's sound is from FM-less: early, late */
                    double dd = 0;
                    for (b = 0; b < 100u; b++) {
                        np_t keep;
                        int32_t o1[CTL], o2[CTL];
                        keep = np_state[0];
                        np_render(0, 3, voice[0].inc, o1, CTL);
                        np_state[0] = keep;
                        sys.mod_depth = 0;
                        np_render(0, 3, voice[0].inc, o2, CTL);
                        sys.mod_depth = 127;
                        np_state[0] = keep;
                        synth_render(buf, CTL);
                        for (j = 0; j < CTL; j++)
                            dd += fabs((double)o1[j] - o2[j]);
                    }
                    ed[m2][ph2] = dd;
                    run_ms(600);
                }
            }
            printf("   (MOD ENV: FM's effect early / late: pluck %.0f / %.0f, held %.0f / %.0f)\n",
                   ed[0][0], ed[0][1], ed[1][0], ed[1][1]);
            okk &= ed[0][1] < ed[0][0] / 20 && ed[1][1] > ed[1][0] / 3;
            sys.menv_a = 0; sys.menv_d = 80; sys.menv_s = 127; sys.menv_r = 70;
        }
        check("FM oscillator: DEPTH 0 bit for bit off", same);
        check("... full DEPTH, every wave and ratio: each program changes, sounds, bounded, no DC; MOD ENV", okk && moved);
        synth_kill();
        sys.osc = 0; sys.mod_depth = 0; sys.mod_wave = 0; sys.mod_ratio = 3; sys.mod_fine = 64;
    }

    /* the ADSR: attack time, the decay settling on the sustain level, the release into the tail and off */
    {
        voice_t v;
        uint32_t f = 0, ok = 1;
        memset(&v, 0, sizeof v);
        sys.attack = 40; sys.decay = 50; sys.sustain = 64; sys.release = 50; sys.ring = 100;
        env_params();
        v.stage = ST_ATTACK;
        while (v.stage == ST_ATTACK && f < 100000u) { env_frame(&v); f++; }
        ok &= f >= ENV_FRAMES[40] - 1u && f <= ENV_FRAMES[40] + 1u;
        for (f = 0; f < 8u * ENV_FRAMES[50]; f++) env_frame(&v);
        ok &= v.stage == ST_SUSTAIN && v.env > eg.sus - ENV_MAX / 100u && v.env < eg.sus + ENV_MAX / 100u;
        v.stage = ST_RELEASE;
        for (f = 0; f < 20u * ENV_FRAMES[50] && v.stage == ST_RELEASE; f++) env_frame(&v);
        ok &= v.stage == ST_TAIL && f > 8u * ENV_FRAMES[50];
        for (f = 0; f < FS && v.stage == ST_TAIL; f++) env_frame(&v);
        ok &= v.stage == ST_OFF;
        check("ADSR: attack on time, decay settles on sustain, release -> tail -> off", ok);
    }

    /* the low pass gate's ring: an impulse into the filter held at ~1 kHz; RING 0 dies at once, RING 127 rings on */
    {
        uint32_t r, len[2];
        for (r = 0; r < 2u; r++) {
            voice_t v;
            uint32_t f, last = 0;
            memset(&v, 0, sizeof v);
            sys.ring = (uint8_t)(r ? 127 : 0);
            env_params();
            v.ctl = (uint32_t)(ENV_MAX * 0.666);          /* index ~170: 25 Hz x 600^(170/255) ~ 1.8 kHz */
            v.amp = 32767;
            for (f = 0; f < 20000u; f++) {
                int32_t y = lpg_frame(&v, f == 0 ? 32767 : 0);
                if (y > 33 || y < -33)                    /* above -60 dB of the impulse */
                    last = f;
            }
            len[r] = last;
        }
        printf("   (impulse at ~1.8 kHz rings for %u frames with RING 0, %u with RING 127)\n", len[0], len[1]);
        check("RING 127 rings (Q 6): an impulse lasts 4..20x longer than with RING 0, under 15 ms",
              len[1] > 4u * (len[0] + 1u) && len[1] < 20u * (len[0] + 1u) && len[1] < FS * 15u / 1000u);
    }

    /* the gate in every corner: every RING, plucks and holds, every regime, low to high notes: bounded, quiet after */
    {
        static const uint8_t NOTES[] = {24, 60, 96, 120};
        uint32_t r, c, ni, b, okk = 1;
        int32_t big = 0;
        peak = 0;
        for (r = 0; r < 128u; r += 21u)
            for (c = 0; c < 128u; c += 32u) {
                sys.ring = (uint8_t)r; sys.chaos = (uint8_t)c; sys.shape = 64; sys.output = 64;
                sys.attack = 0; sys.decay = 40; sys.sustain = (uint8_t)(c & 64u ? 127 : 0); sys.release = 30;
                for (ni = 0; ni < sizeof NOTES; ni++)
                    synth_note_on(NOTES[ni], 127);
                for (b = 0; b < 200u; b++) {
                    block();
                    for (ni = 0; ni < NVOICE; ni++) {
                        int32_t a = voice[ni].ic1 < 0 ? -voice[ni].ic1 : voice[ni].ic1;
                        big = a > big ? a : big;
                    }
                }
                synth_all_off();
                for (b = 0; b < 2000u && sounding(); b++)
                    block();
                okk &= sounding() == 0;
                synth_kill();
            }
        printf("   (largest filter state %d of 2^31, peak %d)\n", big, peak);
        check("low pass gate: every RING x regime x note range bounded, every voice ends", okk && big < (1 << 29) && peak <= 32767);
    }

    /* the FX resonators (Rings' models): every model x STRUCTURE x DAMPING x note range bounded, no NaN-like jumps;
     * MIX 0 changes nothing; MODAL tuned: a lone mode at the note (STRUCTURE 0.25..0.3: harmonic) rings at its pitch */
    {
        static const uint8_t NOTES[] = {24, 48, 72, 96};
        uint32_t md, st, dmp, ni, b2, okk = 1;
        int32_t pk2 = 0;
        sys.chaos = 85; sys.shape = 76; sys.output = 32; sys.attack = 0; sys.decay = 60; sys.sustain = 90;
        sys.release = 60; sys.ring = 50; sys.fx_bright = 100; sys.fx_pos = 40; sys.fx_mix = 127;
        for (md = 0; md < RZ_COUNT; md++)
            for (st = 0; st < 128u; st += 42u)
                for (dmp = 0; dmp < 128u; dmp += 63u) {
                    sys.fx_model = (uint8_t)md; sys.fx_struct = (uint8_t)st; sys.fx_damp = (uint8_t)dmp;
                    for (ni = 0; ni < sizeof NOTES; ni++) {
                        synth_note_on(NOTES[ni], 127);
                        for (b2 = 0; b2 < 120u; b2++) {
                            synth_render(buf, CTL);
                            for (w = 0; w < CTL; w++) {
                                int32_t a2 = buf[2 * w] < 0 ? -buf[2 * w] : buf[2 * w];
                                pk2 = a2 > pk2 ? a2 : pk2;
                            }
                        }
                        synth_kill();
                    }
                }
        printf("   (resonators: peak %d)\n", pk2);
        okk &= pk2 <= 32767;
        {   /* MIX 0: bit for bit the same as never having had a resonator */
            static int32_t ref2[2 * CTL * 300];
            int pass, same = 1;
            for (pass = 0; pass < 2; pass++) {
                synth_kill(); memset(voice, 0, sizeof voice);
                ghost.x = C1 / 10; ghost.y = ghost.z = 0; rng_state = 0x1234567u; voice_clock = 0;
                sys.fx_mix = 0;
                if (pass)
                    sys.fx_model = RZ_STRING;
                synth_note_on(55, 100);
                for (b2 = 0; b2 < 300u; b2++) {
                    synth_render(buf, CTL);
                    for (w = 0; w < 2u * CTL; w++) {
                        if (!pass) ref2[b2 * 2u * CTL + w] = buf[w];
                        else same &= ref2[b2 * 2u * CTL + w] == buf[w];
                    }
                }
            }
            okk &= same;
        }
        synth_kill();
        sys.fx_mix = 0;
        check("FX resonators: every model x STRUCTURE x DAMPING x note bounded; MIX 0 bit for bit dry", okk);
    }

    /* polyphonic resonators: a chord's notes each take their own resonator, tuned to it; the wet output (MIX 127)
     * holds every note of the chord; a fifth note takes the oldest; all bounded (four ringing at once); once quiet
     * none runs */
    {
        static const uint8_t CH[4] = {48, 52, 55, 59};
        uint32_t md, j, b2, okk = 1, dist = 1, tuned = 1, heard = 1;
        int32_t pk2 = 0;
        sys.chaos = 85; sys.shape = 76; sys.output = 32; sys.attack = 0; sys.decay = 70; sys.sustain = 100;
        sys.release = 60; sys.ring = 50; sys.fx_bright = 90; sys.fx_pos = 20; sys.fx_mix = 127; sys.fx_damp = 110;
        sys.osc = 0;
        for (md = 0; md < RZ_COUNT; md++) {
            double re[4] = {0}, im[4] = {0}, tot = 0;
            uint32_t t = 0;
            synth_kill();
            sys.fx_model = (uint8_t)md; sys.fx_struct = md == RZ_MODAL ? 34 : 20;
            sys.fx_mix = 0; run_ms(10); sys.fx_mix = 127;     /* (state anew) */
            for (j = 0; j < 4u; j++)
                synth_note_on(CH[j], 110);
            for (j = 0; j < 4u; j++) {
                uint32_t q;
                for (q = 0; q < j; q++)
                    dist &= voice[j].rz != voice[q].rz;
                tuned &= rz[voice[j].rz].note == CH[j];
            }
            run_ms(100);
            for (b2 = 0; b2 < 600u; b2++) {
                synth_render(buf, CTL);
                for (w = 0; w < CTL; w++, t++) {
                    double v = buf[2 * w];
                    int32_t a2 = buf[2 * w] < 0 ? -buf[2 * w] : buf[2 * w];
                    pk2 = a2 > pk2 ? a2 : pk2;
                    tot += v * v;
                    for (j = 0; j < 4u; j++) {                 /* each note's fundamental */
                        double ph = 2 * M_PI * 440.0 * pow(2, (CH[j] - 69) / 12.0) * t / FS;
                        re[j] += v * cos(ph); im[j] += v * sin(ph);
                    }
                }
            }
            for (j = 0; j < 4u; j++) {                         /* its share of the energy: > 0.5 % */
                double e = 2 * (re[j] * re[j] + im[j] * im[j]) / t / tot;
                if (e < 0.005) {
                    printf("   %s: note %u only %.4f of the energy\n", RZ_NAME[md], CH[j], e);
                    heard = 0;
                }
            }
        }
        synth_note_on(62, 110);                                  /* a fifth note, all four held: the oldest */
        okk &= voice[4].rz == voice[0].rz && rz[voice[4].rz].note == 62;
        sys.fx_damp = 0;                                         /* (the shortest decay: they go quiet) */
        synth_kill();
        run_ms(4000);
        for (j = 0; j < RZ_POLY; j++)
            okk &= rz[j].quiet >= RZ_QUIET;
        printf("   (polyphonic resonators: peak %d)\n", pk2);
        sys.fx_mix = 0;
        check("FX resonators polyphonic: a chord, one tuned resonator a note, all heard", dist && tuned && heard);
        check("... a fifth note takes the oldest; bounded; quiet ones stop running", okk && pk2 <= 32767);
    }

    /* the shimmer: bounded in every corner (SIZE x DECAY x SHIMMER x INTERVAL, held chords), no DC; it rises (the
     * tail's spectral weight climbs); MIX 0 bit for bit dry, and mono (left = right) */
    {
        uint32_t sz, dc, sm, iv, b2, okk = 1;
        int32_t pk2 = 0;
        int64_t dcs = 0;
        sys.chaos = 85; sys.shape = 76; sys.output = 32; sys.attack = 0; sys.decay = 60; sys.sustain = 100;
        sys.release = 60; sys.ring = 50; sys.fx_mix = 0; sys.sh_tone = 127; sys.sh_mix = 127;
        for (sz = 0; sz < 128u; sz += 127u)
            for (dc = 0; dc < 128u; dc += 127u)
                for (sm = 0; sm < 128u; sm += 63u)
                    for (iv = 0; iv < SI_COUNT; iv++) {
                        sys.sh_size = (uint8_t)sz; sys.sh_decay = (uint8_t)dc; sys.sh_shimmer = (uint8_t)sm;
                        sys.sh_interval = (uint8_t)iv;
                        synth_note_on(36, 127); synth_note_on(48, 127); synth_note_on(55, 127); synth_note_on(64, 127);
                        for (b2 = 0; b2 < 400u; b2++) {
                            if (b2 == 200u)
                                synth_all_off();
                            synth_render(buf, CTL);
                            for (w = 0; w < 2u * CTL; w++) {
                                int32_t a2 = buf[w] < 0 ? -buf[w] : buf[w];
                                pk2 = a2 > pk2 ? a2 : pk2;
                                if (b2 >= 300u)
                                    dcs += buf[w];
                            }
                        }
                        synth_kill();
                    }
        printf("   (shimmer: peak %d, mean of the tails %.1f)\n", pk2, (double)dcs / (64.0 * 100 * 2 * CTL));
        okk &= pk2 <= 32767 && dcs / (64 * 100 * 2 * CTL) < 64 && dcs / (64 * 100 * 2 * CTL) > -64;
        {   /* it rises: a pure note's tail with SHIMMER 100, +OCT: its zero crossings a second double and more */
            uint32_t z0 = 0, z1 = 0, j;
            int32_t pv = 0;
            sys.sh_mix = 0;
            fx_shimmer(buf, buf, buf, 0);          /* (clear) */
            sys.chaos = 6; sys.output = 64; sys.sustain = 0; sys.decay = 70; sys.release = 60; sys.ring = 30;
            sys.sh_mix = 127; sys.sh_size = 110; sys.sh_decay = 110; sys.sh_shimmer = 100; sys.sh_interval = SI_OCT;
            sys.sh_tone = 100;
            synth_note_on(48, 110);
            for (b2 = 0; b2 < 1600u; b2++) {
                if (b2 == 20u)
                    synth_note_off(48);
                synth_render(buf, CTL);
                for (j = 0; j < CTL; j++) {
                    int32_t v2 = buf[2 * j];
                    if ((pv < 0) != (v2 < 0)) {
                        if (b2 >= 20u && b2 < 220u) z0++;
                        if (b2 >= 1000u && b2 < 1200u) z1++;
                    }
                    pv = v2;
                }
            }
            printf("   (tail zero crossings: %u at 0.0..0.15 s after the note, %u at 0.7..0.85 s)\n", z0, z1);
            okk &= z1 > 2u * z0;
            synth_kill();
        }
        {   /* MIX 0: the same output as with the shimmer never on, left = right */
            static int32_t ref3[2 * CTL * 300];
            int pass, same = 1;
            for (pass = 0; pass < 2; pass++) {
                synth_kill(); memset(voice, 0, sizeof voice);
                ghost.x = C1 / 10; ghost.y = ghost.z = 0; rng_state = 0x1234567u; voice_clock = 0;
                sys.sh_mix = 0;
                if (pass)
                    sys.sh_interval = SI_2OCT;
                synth_note_on(55, 100);
                for (b2 = 0; b2 < 300u; b2++) {
                    synth_render(buf, CTL);
                    for (w = 0; w < 2u * CTL; w++) {
                        if (!pass) ref3[b2 * 2u * CTL + w] = buf[w];
                        else same &= ref3[b2 * 2u * CTL + w] == buf[w];
                    }
                    for (w = 0; w < CTL; w++)
                        same &= buf[2 * w] == buf[2 * w + 1];
                }
            }
            okk &= same;
        }
        synth_kill();
        check("shimmer: every corner bounded, no DC, the tail rises; MIX 0 bit for bit dry and mono", okk);
    }

    /* MASTER at 0: silence */
    sys.master_q12 = 0;
    peak = 0;
    synth_note_on(60, 127);
    run_ms(50);
    check("MASTER 0: silence", peak == 0);
    synth_kill();
    sys.master_q12 = 4096;

    /* a render to listen to: per preset of ui.c, an arpeggio then a held chord */
    if (argc > 1 && (wav = fopen(argv[1], "wb")) != 0) {
        static const uint8_t ARP[] = {48, 55, 60, 63, 67, 72, 67, 63};
        static const struct {
            uint8_t chaos, shape, output, attack, decay, sustain, release, ring;
            uint8_t fx_model, fx_struct, fx_bright, fx_damp, fx_pos, fx_mix;
            uint8_t sh_size, sh_decay, sh_shimmer, sh_mix, sh_interval, sh_tone;
            uint8_t osc, np_x, np_y;
        } P[] = {   /* ui.c PRESETS */
            {85, 76, 0, 6, 80, 110, 70, 50}, {85, 90, 64, 0, 62, 0, 66, 110}, {6, 64, 64, 10, 70, 120, 64, 30},
            {38, 64, 32, 4, 60, 90, 66, 60}, {54, 56, 64, 2, 64, 80, 72, 70}, {30, 100, 32, 0, 50, 0, 52, 100},
            {110, 64, 96, 2, 76, 100, 74, 60}, {124, 40, 64, 0, 56, 60, 60, 40}, {70, 110, 127, 92, 96, 110, 100, 70},
            {96, 16, 0, 0, 54, 70, 50, 40},
            {54, 90, 64, 0, 60, 0, 70, 40, RZ_MODAL, 70, 100, 110, 12, 100},
            {85, 64, 32, 20, 80, 110, 90, 30, RZ_SYMP, 16, 80, 105, 70, 90},
            {30, 76, 64, 0, 64, 40, 74, 50, RZ_STRING, 100, 70, 96, 20, 85},
            {110, 40, 0, 4, 76, 90, 80, 60, RZ_MODAL, 116, 40, 80, 24, 70},
            {6, 64, 64, 30, 80, 100, 96, 30, 0, 0, 0, 0, 0, 0, 110, 110, 100, 96, SI_OCT, 100},
            {85, 76, 32, 60, 90, 110, 100, 50, 0, 0, 0, 0, 0, 0, 127, 120, 50, 80, SI_OCT, 80},
            {54, 90, 64, 0, 62, 0, 70, 40, RZ_MODAL, 70, 100, 110, 12, 70, 90, 110, 120, 100, SI_FIFTH, 110},
            {85, 76, 0, 0, 70, 100, 70, 40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 40, 90},
            {85, 76, 0, 20, 80, 90, 96, 30, 0, 0, 0, 0, 0, 0, 100, 100, 60, 70, SI_OCT, 90, 7, 70, 50},
            {85, 76, 0, 0, 40, 0, 44, 70, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16, 96, 64},
            {85, 76, 0, 80, 96, 110, 100, 50, 0, 0, 0, 0, 0, 0, 110, 110, 90, 80, SI_FIFTH, 100, 18, 50, 40},
            {85, 76, 0, 0, 62, 0, 70, 40, RZ_MODAL, 70, 100, 110, 12, 80, 0, 0, 0, 0, 0, 0, 11, 30, 80},
            {85, 76, 0, 2, 76, 100, 74, 60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 28, 80, 80},
            {85, 76, 0, 0, 64, 60, 60, 50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 29, 64, 100},
        };
        wav_hdr(wav, 0);
        for (w = 0; w < sizeof P / sizeof P[0]; w++) {
            sys.chaos = P[w].chaos; sys.shape = P[w].shape; sys.output = P[w].output;
            sys.attack = P[w].attack; sys.decay = P[w].decay; sys.sustain = P[w].sustain;
            sys.release = P[w].release; sys.ring = P[w].ring;
            sys.fx_model = P[w].fx_model; sys.fx_struct = P[w].fx_struct; sys.fx_bright = P[w].fx_bright;
            sys.fx_damp = P[w].fx_damp; sys.fx_pos = P[w].fx_pos; sys.fx_mix = P[w].fx_mix;
            sys.sh_size = P[w].sh_size; sys.sh_decay = P[w].sh_decay; sys.sh_shimmer = P[w].sh_shimmer;
            sys.sh_mix = P[w].sh_mix; sys.sh_interval = P[w].sh_interval; sys.sh_tone = P[w].sh_tone;
            sys.osc = P[w].osc; sys.np_x = P[w].np_x; sys.np_y = P[w].np_y;
            for (k = 0; k < sizeof ARP; k++) {
                fm1_in.notes = 1u << key_of(ARP[k]);
                run_ms(170);
                fm1_in.notes = 0;
                run_ms(30);
            }
            fm1_in.notes = 1u << key_of(60) | 1u << key_of(63) | 1u << key_of(67);
            run_ms(1200);
            fm1_in.notes = 0;
            run_ms(900);
        }
        fseek(wav, 0, SEEK_SET);
        wav_hdr(wav, wav_frames);
        fclose(wav);
        printf("wrote %s (%u frames)\n", argv[1], wav_frames);
    }

    printf(bad ? "basis test: %d FAILED\n" : "basis test passed\n", bad);
    return bad != 0;
}
