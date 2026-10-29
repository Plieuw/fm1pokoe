/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* I2S output (ALNK0 -> external codec) and the audio ISR. Per half buffer, blocks of CTL frames:
 * keys_block (keys, MIDI in) -> synth_render (Q15 stereo) -> USB audio (uac_tap) -> 24-bit for the DAC.
 * This is the place to hook in your own engine: fill out[] with Q15 stereo (L R L R ..). */
/* registers: hal/fm1_audio.h */
#define HALF_WORDS (HALF_FRAMES * 2u)
#define CPU_AVG (4096u / HALF_FRAMES)   /* halves the load meter averages: ~93 ms */
#define OUT_SHIFT 7               /* Q15 -> 24-bit, -6 dBFS ceiling */

static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));

/* diagnostics, kept across resets and UBOOT entry */
#define DBG_MAGIC 0x44424731u                       /* "DBG1" */
struct felucca_dbg {
    uint32_t magic, halves, max_us, nested, in_audio, late, timer_irqs, ui_frames;
    uint32_t last_us, cpu_q8, boots;
    uint32_t stage;                       /* where the main loop is (breadcrumb) */
    uint32_t prev_stage, prev_rst;        /* as found at boot */
} felucca_dbg __attribute__((section(".noinit")));
static volatile uint32_t t5_nested_ticks;              /* TIMER4 ticks TIMER5 spent nested in this ISR (main.c) */
static uint32_t audio_cpu_rem;

static void audio_block(int32_t *out, uint32_t n)
{
    keys_block();
    synth_render(out, n);
#if FELUCCA_UAC
    uac_tap(out, n);                                    /* the USB audio input: the same output */
#endif
}

/* the half is rendered here, then copied into the DMA's half only while that half is still the free one (after
 * M-VAVE's fm1_open, app/board/audio.c): a render that ran late (the CPU full) leaves the half the DMA already
 * plays alone, a short repeat instead of a crackle of old and new samples */
static int32_t arender[HALF_WORDS] __attribute__((aligned(4)));

void fm1_alnk0_irq(void)                       /* via isr_alnk0 (hal/fm1_isr.S) */
{
    uint8_t p;
    uint32_t t0;
    felucca_dbg.in_audio = 1;                   /* first: TIMER5 nests from here on (main.c) */
    p = fm1_audio_pending();
    t0 = fm1_ticks();
    t5_nested_ticks = 0;
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half(), b, us;
        int32_t *o = &abuf[half * HALF_WORDS];
#if FELUCCA_UAC
        uac_render_start();
#endif
        for (b = 0; b < HALF_FRAMES; b += CTL)
            audio_block(arender + 2u * b, CTL);
        if (fm1_audio_free_half() == half) {            /* (still ours: 24-bit, into the DMA's half) */
            for (b = 0; b < HALF_WORDS; b++)
                o[b] = arender[b] * (1 << OUT_SHIFT);
            fm1_audio_fence();
        }
        fm1_audio_ack_half();
        us = (fm1_ticks() - t0 - t5_nested_ticks) / FM1_TICKS_PER_US;
        {
            uint32_t load = sys.cpu_q8 * (CPU_AVG - 1u) + (us * 256u) / (HALF_FRAMES * 1000000u / FS) + audio_cpu_rem;
            sys.cpu_q8 = load / CPU_AVG;
            audio_cpu_rem = load % CPU_AVG;
        }
        if (fm1_audio_free_half() != half)
            felucca_dbg.late++;                         /* the DMA moved on while we rendered */
        if (us * 100u > (HALF_FRAMES * 1000000u / FS) * 80u)
            synth_shed = 1;                             /* (over 80 % of the half: the synth lets a voice go) */
        felucca_dbg.halves++;
        felucca_dbg.last_us = us;
        if (us > felucca_dbg.max_us)
            felucca_dbg.max_us = us;
        felucca_dbg.cpu_q8 = sys.cpu_q8;
    }
    felucca_dbg.in_audio = 0;
}
extern void isr_alnk0(void);

static void audio_init(void)                   /* codec and ALNK0 bring-up (fm1_audio.h) */
{
    uint32_t i;
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
}
