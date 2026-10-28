/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Basis core: the sizes and the few shared values every other file uses.
 * Everything here is deliberately small; grow it as your firmware grows. */
#include <stdint.h>

/* ------------------------------------------------------------ audio --- */
#define FS 44100                 /* nominal sample rate (the I2S clock runs at ~44,117.6 Hz) */
#define HALF_FRAMES 128          /* I2S half buffer: 2.9 ms; one audio interrupt renders one half */
#define CTL 32                   /* frames per block: keys and MIDI are handled once a block (~0.7 ms) */
#define NVOICE 4                 /* synth voices (synth.c): 4, as the FX resonators / waveguides (one a note): the CPU */

/* --------------------------------------------------------- keyboard --- */
#define NKEYS 27u                /* the note keys: bit n of fm1_in.notes, 0 = F3 .. 26 = G5 */
#define KEY_BASE_NOTE 53         /* key 0 at octave 0: F3 = MIDI note 53 */
#define OCT_MIN (-3)
#define OCT_MAX 3

/* ----------------------------------------------------------- system --- */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")   /* slot store before the index update */
static volatile uint32_t fm1_ms;  /* milliseconds since boot (TIMER4-based, TIMER5 ISR in main.c) */

/* boot-loop guard (main.c): two boots in a row that die in the first 30 s -> UBOOT */
#define BOOTGUARD_MAGIC 0x42475244u
struct { uint32_t magic, failed, pending; } bootguard __attribute__((section(".noinit")));

static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* What the main loop and the audio interrupt share. Single writer per field:
 *   main loop writes:  octave, chaos, shape, output, attack, decay, sustain, release, ring, master_q12, view, preset,
 *                      cpu2, page, fx_*, sh_*, osc, np_*, mod_*
 *   audio ISR writes:  last_note, last_vel, cpu_q8, voices, env_view, env_stage */
static volatile struct {
    int8_t octave;               /* OCT- / OCT+: -3..+3 */
    uint8_t chaos, shape, output;   /* synth.c: the circuit's knobs, 0..127 */
    uint8_t attack, decay, sustain, release;   /* synth.c: the ADSR, 0..127 */
    uint8_t ring;                /* synth.c: the low pass gate's resonance, 0..127 (Q 0.5 .. 6) */
    uint16_t master_q12;         /* MASTER pot: 0..4096 */
    uint8_t last_note, last_vel; /* the last note started (keys or MIDI); 0xFF = none yet */
    uint8_t voices;              /* voices sounding */
    uint8_t env_view, env_stage; /* the newest voice's gate control (0..255) and stage (ui.c ENV page) */
    uint32_t cpu_q8;             /* audio load, 256 = 100 % */
    int32_t batt_raw;            /* battery ADC, smoothed (main loop) */
    uint8_t view, preset;        /* ui.c: the screen's view, the last preset loaded */
    uint8_t cpu2;                /* 2ND CORE: start the second core at power-on (main.c; GLO toggles it) */
    uint8_t page;                /* ui.c: 0 the oscillator (HOME), 1 ENV, 2 FX: resonator, 3 FX: shimmer, 4 MOD (HOME 2x), 5 MOD ENV (ENV 2x) */
    uint8_t fx_model, fx_struct, fx_bright, fx_damp, fx_pos, fx_mix;   /* resonator.c: the FX resonator, 0..127 */
    uint8_t sh_size, sh_decay, sh_shimmer, sh_mix, sh_interval, sh_tone;   /* shimmer.c: the shimmer reverb */
    uint8_t osc;                 /* the oscillator: 0 the CHUA circuit, 1 .. 30 a noise program (noise.c: osc - 1) */
    uint8_t np_x, np_y, np_level;   /* noise.c: its X, Y and level, 0..127 */
    uint8_t mod_wave, mod_ratio, mod_depth, mod_fine;   /* noise.c: the FM oscillator (HOME again: the MOD page) */
    uint8_t menv_len, menv_skew, menv_height;   /* noise.c: its AD envelope (ENV again: the MOD ENV page) */
    uint8_t basis;               /* synth.c: the voices' stereo spread, 0 (mono) .. 127 (ENV page, SELECT) */
    uint8_t orbit;               /* synth.c: how far each voice's picture moves it in the stereo field (ENV, ALGORITHM) */
    uint16_t menv_view;          /* the newest voice's FM depth now (DEPTH + HEIGHT x envelope), 0..32767 (ui.c) */
} sys = {.octave = 0, .chaos = 85, .shape = 76, .output = 0, .attack = 6, .decay = 80, .sustain = 110, .release = 70,
         .ring = 50, .master_q12 = 2048, .last_note = 0xFF, .cpu2 = 1,
         .fx_model = 0, .fx_struct = 70, .fx_bright = 90, .fx_damp = 100, .fx_pos = 24, .fx_mix = 0,
         .sh_size = 100, .sh_decay = 90, .sh_shimmer = 70, .sh_mix = 0, .sh_interval = 0, .sh_tone = 80,
         .osc = 0, .np_x = 64, .np_y = 64, .np_level = 64,
         .mod_wave = 0, .mod_ratio = 3, .mod_depth = 0, .mod_fine = 64,
         .menv_len = 80, .menv_skew = 10, .menv_height = 0, .basis = 0, .orbit = 0};
