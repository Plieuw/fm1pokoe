# FM-1 Basis

Een lege basis voor eigen firmware op de M-VAVE FM-1, uitgekleed uit [Felucca](https://github.com/hugelton/Felucca)
van Hügelton Instruments. Alle synth-engines, de sequencer, de effecten, presets, projecten, de editor, de console en
de browser-emulator zijn weg. Wat overblijft is de hardwarelaag, de veiligheidsnetten en een klein speelbaar
voorbeeld: **het keyboard werkt**.

## Wat het nu doet

| Bediening | Functie |
| --- | --- |
| 27 toetsen | spelen een eenvoudige 8-stemmige synth, sturen USB-MIDI uit (kanaal 1), lichten op |
| OCT- / OCT+ | octaaf −3..+3 (LED aan als het octaaf niet 0 is) |
| KNOB 1 / 2 / 3 | golfvorm (SINE TRI SAW SQUARE) / attack / release |
| KNOB 4, SELECT, ALGORITHM, PRESETS, overige knoppen | worden alleen op het scherm getoond: vrij voor jouw functies |
| SAVE | octaaf, golfvorm, attack en release in flash (terug na herstart) |
| MASTER | volume |
| MIDI in (USB en TRS) | note on/off op elk kanaal, CC 123 = all notes off |
| USB | class-compliant MIDI + USB-audio-ingang (opnemen in je DAW) |
| OCT- + OCT+ 5 s ingedrukt | UBOOT (update-modus) |

## Waar begin je

Alles wordt als één bestand gecompileerd: `firmware/src/felucca.c` bepaalt de volgorde.

| Bestand | Wat |
| --- | --- |
| `firmware/src/keys.c` | toetsen en MIDI in → noten (draait in de audio-interrupt, elke 32 samples) |
| `firmware/src/synth.c` | de klank: **vervang dit door je eigen engine** |
| `firmware/src/audio.c` | de audio-interrupt: `out[]` vullen met Q15 stereo |
| `firmware/src/ui.c` | scherm, LEDs, knoppen, encoders, instellingen opslaan |
| `firmware/src/main.c` | opstarten en hoofdlus (met watchdog, boot-guard, UBOOT en update) |
| `firmware/src/panel.c` | welke matrix-knop welke opdruk heeft |
| `firmware/src/core.h` | gedeelde constanten en de `sys`-struct (hoofdlus ↔ audio) |
| `firmware/src/storage.c` | flash-opslag met A/B-kopieën; `OBJ_USER0..7` zijn vrij |
| `firmware/src/gfx.c`, `lcd.c` | tekenen: tekst (Inter Tight), rechthoeken, lijnen; kleuren via `T_BG`, `T_TEXT`, `T_THEME` … |
| `firmware/src/usb.c`, `midi_uart.c` | USB-MIDI/-audio en TRS MIDI (ongewijzigd uit Felucca) |
| `firmware/src/ota.c`, `ota_hw.c`, `firmware/loader/` | het update-pad: **laten staan** |
| `firmware/hal/` | registers: toetsmatrix, LEDs, encoders, LCD, I2S, ADC, flash, tweede core |

Regels van het huis (uit Felucca, en `tools/build.py` controleert ze): registertoegang alleen in `hal/`, geen
floating point in de audio-interrupt nodig (alles is fixed point), en de audio-interrupt moet binnen ~2,9 ms klaar zijn
(het scherm toont de CPU-belasting).

## Bouwen, testen, installeren

Zie [BUILDING.md](BUILDING.md). Kort:

```
tools/get_toolchain.sh                 # JieLi-toolchain (eenmalig)
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
pip3 install Pillow fonttools mido python-rtmidi
./build.sh                             # -> build/felucca.fwsc
tests/run_tests.sh                     # host-tests, geen FM-1 nodig (+ schermafbeeldingen in build/host/ui)
python3 tools/fm1_install.py build/felucca.fwsc
```

Op macOS draait de toolchain in Docker (zie BUILDING.md); op Linux x86-64 direct.

**Veiligheid.** De installers praten met de draaiende firmware (`ota.c`). Haal dat er niet uit, anders kun je de
volgende build alleen nog via UBOOT installeren. Lukt opstarten twee keer achter elkaar niet binnen 30 s, dan gaat de
FM-1 vanzelf naar UBOOT. Herstel na een mislukte installatie: [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).
Terug naar de officiële firmware: `python3 tools/fm1_install.py FM-1.fwsc` met het V15-bestand van M-VAVE.

## Licentie

GPL-3.0-only, net als Felucca ([LICENSE](LICENSE), [LICENSING.md](LICENSING.md), [CREDITS.md](CREDITS.md)). Deel je
een build, deel dan ook de broncode. De namen `FELUCCA_*` in de code en de USB-naam "Felucca" zijn nog die van het
origineel; hernoem ze gerust (`firmware/src/usb.c` STR1/STR2 voor de USB-naam, met een eigen VID/PID vóór je iets uitbrengt).
Let op: `tools/fm1_install.py` zoekt de FM-1 op poortnaam (`PORT_RE`); pas die mee aan, of gebruik `--port`.
