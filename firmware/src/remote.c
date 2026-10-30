/* SPDX-License-Identifier: GPL-3.0-only */
/* REMOTE: the FM-1 seen and played from a computer over USB-MIDI SysEx (tools/fm1_remote.py), so a test can look
 * at the screen, read the state and drive the panel without anyone at it. Only over USB (not TRS); harmless
 * otherwise: it answers questions and presses buttons, nothing more.
 *
 * Every message is F0 7D 43 48 cmd ... F7 (7D: the non-commercial ID; "CH": CHUA). From the host:
 *   01            SCREEN  -> the picture (the plot, 240 x PLOT_H, RGB565): messages 41 row data (4 rows a message,
 *                            packed 7 in 8), then 42 rows height
 *   02            STATE   -> 50 text: the page, the screen's text lines, the preset, tempo, load, tape, FX ...
 *   03 b          BUTTON  -> a press of button b (panel.c's order: FX SCL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC
 *                            OCT- OCT+), as the panel's would be
 *   04 e s        TURN    -> encoder e (SELECT ALGORITHM PRESETS KNOB 1..4) turned s - 64 detents
 *   05            PING    -> 51 the firmware's identity
 * Notes, CCs and program changes need nothing new: they are MIDI. */

#if FELUCCA_OTA
#define RM_HDR0 0x7Du
#define RM_HDR1 0x43u
#define RM_HDR2 0x48u
static uint8_t rm_out[2400] __attribute__((section(".pool")));

static uint32_t rm_pack7(const uint8_t *in, uint32_t n, uint8_t *out)   /* 7 bytes -> 8: their tops first */
{
    uint32_t i = 0, o = 0;
    while (i < n) {
        uint32_t k = n - i < 7u ? n - i : 7u, j;
        uint8_t hi = 0;
        for (j = 0; j < k; j++)
            hi |= (uint8_t)((in[i + j] >> 7) << j);
        out[o++] = hi;
        for (j = 0; j < k; j++)
            out[o++] = in[i + j] & 0x7Fu;
        i += k;
    }
    return o;
}
static void rm_send(uint8_t cmd, const uint8_t *body, uint32_t n, int raw)   /* raw: body already 7-bit */
{
    uint32_t o = 0;
    rm_out[o++] = 0xF0; rm_out[o++] = RM_HDR0; rm_out[o++] = RM_HDR1; rm_out[o++] = RM_HDR2; rm_out[o++] = cmd;
    if (raw) {
        uint32_t i;
        for (i = 0; i < n && o + 1u < sizeof rm_out; i++)
            rm_out[o++] = body[i] & 0x7Fu;
    } else
        o += rm_pack7(body, n, rm_out + o);
    rm_out[o++] = 0xF7;
    (void)ota_wire_send(rm_out, o);
}

static void rm_line(char *d, uint32_t n, const char *k, const char *v)
{
    ui_cat(d, n, k);
    ui_cat(d, n, v);
    ui_cat(d, n, "\n");
}
static void rm_state(void)
{
    static char t[900] __attribute__((section(".pool")));
    char v[16];
    uint32_t i;
    t[0] = 0;
    fmt_int(v, sys.page);
    rm_line(t, sizeof t, "PAGE ", v);
    for (i = 0; i < NTEXT; i++)
        rm_line(t, sizeof t, i == T_HEAD ? "HEAD " : i == T_STATUS ? "STATUS " : "CELL ", ui_shown[i]);
    fmt_int(v, sys.preset);
    rm_line(t, sizeof t, "PRESET ", v);
    fmt_int(v, (int32_t)arp_bpm_now());
    rm_line(t, sizeof t, "BPM ", v);
    fmt_int(v, (int32_t)(sys.cpu_q8 * 100u / 256u));
    rm_line(t, sizeof t, "CPU ", v);
    fmt_int(v, sys.voices);
    rm_line(t, sizeof t, "VOICES ", v);
    fmt_int(v, sys.osc);
    rm_line(t, sizeof t, "OSC ", v);
    fmt_int(v, sys.fx_model);
    rm_line(t, sizeof t, "FX_MODEL ", v);
    fmt_int(v, sys.fx_mix);
    rm_line(t, sizeof t, "FX_MIX ", v);
    fmt_int(v, sys.sh_space);
    rm_line(t, sizeof t, "SH_SPACE ", v);
    fmt_int(v, sys.sh_mix);
    rm_line(t, sizeof t, "SH_MIX ", v);
    fmt_int(v, (int32_t)(tape.playing | tape.rec << 1));
    rm_line(t, sizeof t, "TAPE ", v);
    fmt_int(v, (int32_t)tape.speed);
    rm_line(t, sizeof t, "TAPE_SPEED_Q12 ", v);
    fmt_int(v, (int32_t)(tape.has[0] | tape.has[1] << 1 | tape.has[2] << 2 | tape.has[3] << 3));
    rm_line(t, sizeof t, "TAPE_TRACKS ", v);
    fmt_int(v, (int32_t)(fm1_ms / 1000u));
    rm_line(t, sizeof t, "UPTIME_S ", v);
    rm_send(0x50, (const uint8_t *)t, str_len(t), 1);
}
static void rm_screen(void)
{
    static uint8_t raw[1 + 4u * 240u * 2u] __attribute__((section(".pool")));
    uint32_t y, i;
    if (cv_w != 240u || cv_h != (uint32_t)PLOT_H) {             /* (the plot not on the canvas now: nothing) */
        uint8_t h[2] = {0, 0};
        rm_send(0x42, h, 2, 1);
        return;
    }
    for (y = 0; y < (uint32_t)PLOT_H; y += 4u) {
        uint32_t rows = (uint32_t)PLOT_H - y < 4u ? (uint32_t)PLOT_H - y : 4u;
        raw[0] = (uint8_t)y;
        for (i = 0; i < rows * 240u; i++) {
            uint16_t c = cv_px[y * 240u + i];                    /* (stored byte-swapped for the panel) */
            raw[1u + 2u * i] = (uint8_t)(c & 0xFFu);
            raw[2u + 2u * i] = (uint8_t)(c >> 8);
        }
        rm_send(0x41, raw, 1u + rows * 480u, 0);
    }
    {
        uint8_t h[2] = {(uint8_t)(PLOT_H & 0x7F), (uint8_t)(PLOT_H >> 7)};
        rm_send(0x42, h, 2, 1);
    }
}

/* main loop: a frame of ours handled (and released); anyone else's left for ota_service */
static void rm_service(void)
{
    const uint8_t *p;
    uint32_t n;
    if (!ota_frame_get(&p, &n))
        return;
    if (n < 4u || p[0] != RM_HDR0 || p[1] != RM_HDR1 || p[2] != RM_HDR2)
        return;                                  /* (not ours: the update's) */
    {
        uint8_t cmd = p[3], a = n > 4u ? p[4] : 0, b = n > 5u ? p[5] : 64;
        ota_frame_done();                        /* (copied out: the ISR may take the next) */
        switch (cmd) {
        case 0x01: rm_screen(); break;
        case 0x02: rm_state(); break;
        case 0x03:
            if (a < NB)
                rm_btn |= btn_bit(a);
            break;
        case 0x04:
            if (a < NE)
                rm_turn[a] = (int8_t)clamp(rm_turn[a] + (int32_t)b - 64, -64, 63);
            break;
        case 0x05: {
            static const char ID[] = FELUCCA_ID " " FELUCCA_VERSION;
            rm_send(0x51, (const uint8_t *)ID, sizeof ID - 1u, 1);
            break;
        }
        default:
            break;
        }
    }
}
#endif
