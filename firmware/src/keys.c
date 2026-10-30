/* SPDX-License-Identifier: GPL-3.0-only */
/* The keyboard and MIDI in, once per audio block (audio ISR: lowest latency, no locking needed).
 *
 * Keys: the 27 note keys (fm1_in.notes, debounced by the TIMER5 scan in hal/fm1_input.h) play the synth at the
 * octave OCT- / OCT+ set, and go out as MIDI (USB, channel 1). A key remembers the note it started, so changing
 * the octave while a key is down never leaves a note hanging.
 * MIDI in (USB and the TRS jack, usb.c / midi_uart.c rings, each on or off, the GLO page): note on / off on the RX
 * channel (or any: OMNI) plays the synth; the keys go out (USB, if on) on the TX channel;
 * CC 123 (all notes off) releases everything.
 * A note held by several sources (two keys at different octaves, a key and MIDI) ends when the last lets go.
 * ARP on (arp.c) or KARMA on (karma.c, the SEQ page): the notes held go to the one on instead (never both: one
 * turned on turns the other off, the notes held move over); MIDI clock in drives them. Their notes go out on USB MIDI,
 * the keys' own then do not.
 *
 *   keys_block()     audio ISR, before the block renders
 *   keys_panic_req   main loop: set to release everything at the next block */
#define KB_SILENT 0xFFu
#define KEY_VEL 100u                             /* the keys have no velocity: a fixed one */

static volatile uint8_t kb_boot_hold;            /* set from power-on to the end of the splash (main.c): keys silent */
static volatile uint8_t keys_panic_req;
static volatile uint8_t keys_prog_req;           /* a MIDI program change: its number + 1 (ui.c loads the preset) */
static uint32_t kb_prev;                         /* fm1_in.notes at the last block */
static uint8_t kb_note[NKEYS];                   /* the note key k started (KB_SILENT: none) */
static uint8_t kb_ch[NKEYS];                     /* the MIDI channel its note on went out on (its off: the same) */
static void kb_out(uint32_t pkt)                 /* a note on to USB MIDI out, if it is on (the GLO page; the offs
                                                  * go always: none left hanging when it is turned off) */
{
    if ((sys.midi_usb & 2u) && !sys.arp_on && !sys.k_on)   /* (ARP, KARMA: what they make goes out instead) */
        midi_out_event(pkt);
}
static uint8_t note_held[128];                   /* sources holding each note */

enum { OWN_SYNTH, OWN_ARP, OWN_KARMA };         /* who the notes held go to */
static uint8_t own_was, arp_was_hold, k_was_latch, k_was_on, arp_was_on;
static uint32_t owner(void) { return sys.k_on ? OWN_KARMA : sys.arp_on ? OWN_ARP : OWN_SYNTH; }

static void note_start(uint32_t note, uint32_t vel)
{
    if (note_held[note] < 255u)
        note_held[note]++;
    switch (own_was) {
    case OWN_ARP: arp_add(note, vel); break;
    case OWN_KARMA: karma_add(note, vel); break;
    default: synth_note_on(note, vel); break;
    }
}

static void note_end(uint32_t note)
{
    if (!note_held[note])
        return;
    if (--note_held[note] == 0u) {
        switch (own_was) {
        case OWN_ARP: arp_remove(note); break;
        case OWN_KARMA: karma_remove(note); break;
        default: synth_note_off(note); break;
        }
    }
}

/* ARP or KARMA turned on or off (or HOLD, LATCH off) under held notes: they move over. The two never at once: the
 * one just turned on wins, the other goes off */
static void arp_follow(void)
{
    uint32_t k, o;
    if (sys.k_on && !k_was_on && sys.arp_on)
        sys.arp_on = 0;
    else if (sys.arp_on && !arp_was_on && sys.k_on)
        sys.k_on = 0;
    k_was_on = sys.k_on;
    arp_was_on = sys.arp_on;
    o = owner();
    if (o != own_was) {
        own_was = (uint8_t)o;
        arp_reset();
        karma_reset();
        synth_all_off();
        for (k = 0; k < 128u; k++)               /* the notes held now: to the one on, or sounding again */
            if (note_held[k]) {
                if (o == OWN_ARP)
                    arp_add(k, KEY_VEL);
                else if (o == OWN_KARMA)
                    karma_add(k, KEY_VEL);
                else
                    synth_note_on(k, KEY_VEL);
            }
    }
    if (sys.arp_hold != arp_was_hold) {
        arp_was_hold = sys.arp_hold;
        if (!sys.arp_hold)
            arp_hold_released();
    }
    if (sys.k_latch != k_was_latch) {
        k_was_latch = sys.k_latch;
        if (!sys.k_latch)
            karma_latch_released();
    }
}

static void keys_all_off(void)
{
    uint32_t k;
    for (k = 0; k < 128u; k++)
        note_held[k] = 0;
    for (k = 0; k < NKEYS; k++)
        if (kb_note[k] != KB_SILENT) {
            midi_out_event(0x08u | (0x80u | kb_ch[k]) << 8 | (uint32_t)kb_note[k] << 16);
            kb_note[k] = KB_SILENT;
        }
    synth_all_off();
    arp_reset();
    karma_reset();
}

static void keyboard_block(void)
{
    uint32_t cur = fm1_in.notes, ch = cur ^ kb_prev, k;
    kb_prev = cur;
    if (!ch)
        return;
    for (k = 0; k < NKEYS; k++) {
        if (!((ch >> k) & 1u))
            continue;
        if ((cur >> k) & 1u) {                   /* down */
            int32_t n = KEY_BASE_NOTE + (int32_t)k + 12 * sys.octave;
            if (kb_boot_hold || n < 0 || n > 127) {
                kb_note[k] = KB_SILENT;
                continue;
            }
            kb_note[k] = (uint8_t)n;
            note_start((uint32_t)n, KEY_VEL);
            kb_ch[k] = (uint8_t)(sys.midi_tx & 15u);
            kb_out(0x09u | (0x90u | kb_ch[k]) << 8 | (uint32_t)n << 16 | KEY_VEL << 24);
        } else if (kb_note[k] != KB_SILENT) {    /* up: the note it started */
            note_end(kb_note[k]);
            midi_out_event(0x08u | (0x80u | kb_ch[k]) << 8 | (uint32_t)kb_note[k] << 16);
            kb_note[k] = KB_SILENT;
        }
    }
}

static void midi_in_block(void)
{
    if (midi_in_overflow) {                      /* the ring overflowed: a note-off may be lost, release all */
        mi_r = mi_w;
        keys_all_off();
        RING_PUBLISH();
        midi_in_overflow = 0;
        return;
    }
    while (mi_r != mi_w) {
        uint32_t pkt = midi_in_q[mi_r % MQ], st = (pkt >> 8) & 0xF0u, src = midi_in_source[mi_r % MQ];
        uint32_t d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu, rx = sys.midi_rx;
        mi_r++;
        if ((src == 2u && !sys.midi_trs) || (src == 1u && !(sys.midi_usb & 1u)))
            continue;                            /* (the GLO page: that input off) */
        if (st < 0xF0u && rx && ((pkt >> 8) & 0x0Fu) != (rx - 1u) % 16u)
            continue;                            /* (another channel than RX) */
        if (st == 0x90u && d2)
            note_start(d1, d2);
        else if (st == 0x80u || st == 0x90u)     /* (note on with velocity 0 is a note off) */
            note_end(d1);
        else if (st == 0xB0u && (d1 == 123u || d1 == 120u))
            keys_all_off();
        else if (st == 0xB0u && d1 == 1u)        /* the mod wheel: its parameter (feg.c mw_block, the EDIT page) */
            sys.mw = (uint8_t)d2;
        else if (st == 0xC0u)                    /* program change: the preset (ui.c, the main loop) */
            keys_prog_req = (uint8_t)(d1 + 1u);
        else if (((pkt >> 8) & 0xFFu) >= 0xF8u)   /* realtime: the clock for the arpeggiator */
            arp_midi_rt((pkt >> 8) & 0xFFu);
        /* add here: CC -> parameters, pitch bend ... */
    }
}

static void keys_init(void)
{
    uint32_t k;
    for (k = 0; k < NKEYS; k++)
        kb_note[k] = KB_SILENT;
}

static void keys_block(void)
{
    if (keys_panic_req) {
        keys_panic_req = 0;
        keys_all_off();
        synth_kill();
    }
    arp_follow();
    keyboard_block();
    midi_in_block();
    arp_block(CTL);
    karma_block(CTL);
    feg_block(CTL, voice_clock);
    mw_block();                 /* (voice_clock: the note ons, the arpeggiator's too) */
}
