/* SPDX-License-Identifier: GPL-3.0-only */
/* The screen without the hardware: firmware/src/ui.c and gfx.c (the real drawing code) against a 240 x 240
 * framebuffer instead of the LCD, with synth.c running (the attractor's trace), a few states drawn and saved as raw
 * RGB565 (tests/ui_shot.py makes PNGs): every view, every regime, the presets, the widest values.
 * Also checks that no line is drawn outside the screen.
 *   tests/ui_shot OUT_DIR      (run_tests.sh: PNGs in build/host/ui) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define memset felucca_memset
#define memcpy felucca_memcpy
#define memcmp felucca_memcmp
#include "../firmware/src/libc.c"
#undef memset
#undef memcpy
#undef memcmp

/* ------------------------------------------------------------ the LCD --- */
static uint16_t fb[240 * 240];
static int off_screen;
static void lcd_sync(void) {}
static void lcd_wake_now(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = c;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    if (x + w > 240u || y + h > 240u)
        off_screen++;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u)
                fb[(y + j) * 240u + x + i] = (uint16_t)(px[j * w + i] >> 8 | px[j * w + i] << 8);   /* (byte-swapped) */
}
#include "../firmware/src/gfx.c"

/* ------------------------------------------------- what ui.c reads --- */
#define FELUCCA_VERSION "v0.1"
#include "../firmware/src/core.h"
static uint32_t fm1_ms_frac;
#define FM1_NCOL 11u
static const int8_t FM1_KEYMAP[6][FM1_NCOL] = {      /* as hal/fm1_input.h */
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    { 5, 11,  4, 10,  3,  9,  2,  8, -1, -1, -1},
    {34, 35, 36, 37, 38, 40, 39, 13,  7,  6, 12},
    {23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},
    { 0,  1, 15, 14, 17, 16, 19, 18, 20, 21, 22},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
};
static struct { volatile uint32_t notes, buttons; } fm1_in;
static uint8_t fm1_led[FM1_NCOL], fm1_led_dim[FM1_NCOL];
static int fm1_led_anim_on(void) { return 0; }
static int32_t enc_steps[7];
static int32_t fm1_enc_take(uint32_t e) { int32_t s = enc_steps[e]; enc_steps[e] = 0; return s; }
static uint32_t edges;
static uint32_t fm1_input_edges(uint32_t *released) { uint32_t p = edges; (void)released; edges = 0; return p; }
static struct { uint8_t config, suspended; } usb = {1, 0};
static uint8_t flash_ok = 1;
enum { OBJ_SETTINGS };
static uint8_t flash_store[64];
static int saved;
static int st_load(uint32_t obj, void *dst, uint32_t max) { (void)obj; if (!saved) return -1; memcpy(dst, flash_store, max); return (int)max; }
static int st_save(uint32_t obj, const void *src, uint32_t len) { (void)obj; memcpy(flash_store, src, len); saved = 1; return 0; }
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
static uint32_t fl_jedec(void) { return 0x856014u; }
#define FL_FAR(f) fl_jedec
#include "../firmware/src/panel.c"
#include "../firmware/src/synth.c"
#include "../firmware/src/ui.c"

static int32_t abuf_[2 * CTL];
static void run_ms(uint32_t ms)                  /* the audio for ms, the clock with it */
{
    uint32_t n = ms * FS / 1000u / CTL + 1u;
    while (n--) {
        synth_render(abuf_, CTL);
        fm1_ms_frac += CTL;
        while (fm1_ms_frac >= FS / 1000u) {
            fm1_ms_frac -= FS / 1000u;
            fm1_ms++;
        }
    }
}

static void frames(uint32_t n)                  /* n UI frames, 40 ms of audio each (the scales settle) */
{
    while (n--) {
        run_ms(40);
        ui_draw();
    }
}

static int shot(const char *dir, const char *name)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.rgb565", dir, name);
    if (!(f = fopen(path, "wb")))
        return 1;
    fwrite(fb, 2, 240 * 240, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    int bad = 0;
    uint32_t i;
    char name[32];
    palette_set(UI_PALETTE);

    draw_splash();
    bad |= shot(dir, "a_splash");

    ui_force = 1;                        /* power-on: the ghost circuit, DOUBLE SCROLL, every view */
    run_ms(1500);
    for (i = 0; i < V_COUNT; i++) {
        sys.view = (uint8_t)i;
        frames(12);
        snprintf(name, sizeof name, "b_view_%u_%s", i, VIEW_NAME[i]);
        for (char *c = name; *c; c++)
            if (*c == '-') *c = '_';
        bad |= shot(dir, name);
    }

    sys.view = V_XY;                     /* every preset, as the PRESETS encoder loads them, a note held */
    synth_note_on(48, 100);
    for (i = 0; i < NPRESETS; i++) {
        enc_steps[panel.enc[EN_PRESET]] = i ? 1 : 0;
        ui_input();
        run_ms(700);
        frames(12);
        snprintf(name, sizeof name, "c_preset_%u", i);
        bad |= shot(dir, name);
    }
    synth_kill();

    sys.page = 1;                        /* the ENV page: a held note (its gate's level and cutoff), then a pluck */
    preset_load(0);
    synth_note_on(55, 100);
    frames(10);
    bad |= shot(dir, "e_env_held");
    preset_load(1);
    sys.release = 100;
    enc_steps[panel.enc[EN_K2]] = 30;    /* (DECAY turned: its cell lit) */
    ui_input();
    synth_note_on(60, 100);
    frames(3);
    bad |= shot(dir, "e_env_pluck");
    frames(12);
    bad |= shot(dir, "e_env_dimming");
    synth_kill();
    frames(2);
    bad |= shot(dir, "e_env_dark");
    synth_kill();
    sys.page = 0;
    ui_force = 1;

    sys.page = 2;                        /* the FX page: off, then each model on a held note */
    preset_load(0);
    frames(3);
    bad |= shot(dir, "f_fx_off");
    for (i = 0; i < RZ_COUNT; i++) {
        preset_load(10u + (i == 0u ? 0u : i == 1u ? 1u : 2u));   /* RINGS BELL, SITAR DRONE, CHAOS PIANO */
        synth_note_on(48, 100);
        frames(14);
        snprintf(name, sizeof name, "f_fx_%u", i);
        bad |= shot(dir, name);
        synth_kill();
    }
    sys.page = 3;                        /* the SHIMMER page: HEAVEN RISER, a note and its tail rising, then off */
    preset_load(14);
    synth_note_on(48, 110);
    frames(30);
    synth_note_off(48);
    frames(80);
    bad |= shot(dir, "g_shimmer");
    sys.sh_mix = 0;
    frames(4);
    bad |= shot(dir, "g_shimmer_off");
    synth_kill();
    sys.page = 0;
    sys.fx_mix = 0;
    ui_force = 1;

    preset_load(0);                      /* the noise programs: ALGORITHM on HOME, then a few of them drawn */
    sys.view = V_XY;
    synth_note_on(48, 100);
    enc_steps[panel.enc[EN_ALGO]] = 6;   /* CHUA -> A5 RESONOISE */
    ui_input();
    frames(10);
    bad |= shot(dir, "h_noise_select");
    {
        static const uint8_t O[] = {6, 14, 21, 28};   /* RESONOISE, SINE FM CLUST, BASURA TOTAL, SATAN WORKOUT */
        for (i = 0; i < sizeof O; i++) {
            sys.osc = O[i];
            ui_event_ms = 0;
            frames(14);
            snprintf(name, sizeof name, "h_noise_%u", O[i]);
            bad |= shot(dir, name);
        }
    }
    sys.view = V_TIME;
    frames(8);
    bad |= shot(dir, "h_noise_time");
    sys.osc = 14;                        /* the MOD page: off, then the waves at a few ratios and depths */
    edges = btn_bit(B_HOME);
    ui_input();
    frames(4);
    bad |= shot(dir, "i_mod_off");
    {
        static const uint8_t M[4][3] = {{0, 6, 60}, {42, 3, 90}, {85, 9, 110}, {127, 1, 127}};
        for (i = 0; i < 4u; i++) {
            sys.mod_wave = M[i][0]; sys.mod_ratio = M[i][1]; sys.mod_depth = M[i][2];
            ui_event_ms = 0;
            frames(6);
            snprintf(name, sizeof name, "i_mod_%u", i);
            bad |= shot(dir, name);
        }
    }
    sys.mod_wave = 64;                   /* a morph between two */
    frames(3);
    bad |= shot(dir, "i_mod_morph");
    sys.osc = 0;
    frames(3);
    bad |= shot(dir, "i_mod_chua");
    sys.mod_depth = 0;
    edges = btn_bit(B_HOME);
    ui_input();
    synth_kill();
    preset_load(0);
    ui_force = 1;

    sys.octave = 2;                      /* the widest values: a knob just turned, a message */
    sys.release = 127;
    sys.cpu_q8 = 255;
    enc_steps[panel.enc[EN_K2]] = 127;
    ui_input();
    edges = btn_bit(B_SAVE);
    ui_input();
    frames(8);
    bad |= shot(dir, "d_saved");
    sys.octave = -3;                     /* (after the save: the round trip below must give +2) */

    {   /* settings: SAVE wrote them, a power-on reads them back */
        uint8_t sh = sys.shape;
        sys.octave = 0; sys.shape = 0; sys.release = 0;
        settings_boot();
        if (sys.octave != 2 || sys.shape != sh || sys.release != 127) {
            printf("settings round trip FAIL\n");
            bad = 1;
        }
    }
    if (off_screen) {
        printf("%d strips drawn outside the screen FAIL\n", off_screen);
        bad = 1;
    }
    printf(bad ? "ui shots: FAILED\n" : "ui shots: ok (%s)\n", dir);
    return bad;
}
