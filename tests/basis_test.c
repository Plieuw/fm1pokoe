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
            c1_mode = mode;
            sys.chaos = 85; sys.shape = 76; sys.output = 32; sys.attack = 2; sys.release = 30;
            for (b = 0; b < 2000u; b++) {
                if (b % 250u == 0u)
                    for (j = 0; j < sizeof CH; j++)
                        (b / 250u + j) & 1u ? synth_note_on(CH[j] + b / 250u, 100) : synth_note_off(CH[j] + b / 250u - 1u);
                if (b == 1000u)
                    sys.chaos = 120;
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
        printf("   (blocks with voices on the second core: %u of 2000)\n", blocks);
        check("second core (at once, a thread, one that gives up): the same output bit for bit", same && blocks > 1000u);
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
        static const struct { uint8_t chaos, shape, output, attack, release; } P[] = {
            {85, 76, 0, 2, 40}, {6, 64, 64, 4, 30}, {38, 64, 32, 2, 36}, {54, 56, 64, 2, 44},
            {110, 64, 96, 2, 50}, {124, 40, 64, 0, 30}, {70, 110, 127, 70, 100}, {96, 16, 0, 0, 18},
        };
        wav_hdr(wav, 0);
        for (w = 0; w < sizeof P / sizeof P[0]; w++) {
            sys.chaos = P[w].chaos; sys.shape = P[w].shape; sys.output = P[w].output;
            sys.attack = P[w].attack; sys.release = P[w].release;
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
