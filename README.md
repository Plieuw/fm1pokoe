# CHUA — een chaotische synthesizer voor de M-VAVE FM-1

Firmware voor de FM-1 waarvan de oscillator **Chua's circuit** is: het eenvoudigste elektronische circuit dat chaotisch
gedrag vertoont. Elke stem is een eigen circuit; het scherm tekent live de attractor die de stem doorloopt. Gebouwd op
een uitgeklede [Felucca](https://github.com/hugelton/Felucca) van Hügelton Instruments (hardwarelaag, USB, update-pad).

## Het circuit

Twee condensatoren (C1, C2), een spoel (L), een weerstand (R) en één niet-lineair element: de **Chua-diode**, een
negatieve weerstand met een stuksgewijs lineaire karakteristiek. Dimensieloos:

```
dx/dt = α (y − x − g(x))      x: spanning over C1
dy/dt = x − y + z             y: spanning over C2
dz/dt = −β y                  z: stroom door L
```

g(x) heeft helling −8/7 binnen |x| < 1, −5/7 tot |x| < 3 en +2 daarbuiten (daar verzadigen de op-amps van de diode,
zoals in een echt circuit; zo blijft elke instelling begrensd). Rond x = ±1,5 liggen twee evenwichtspunten waar de
toestand omheen spiraleert; de energie die de diode bij x = 0 toevoegt, gooit hem van de ene spiraal naar de andere.

Draai je α op, dan doorloopt het circuit de klassieke route naar chaos: rust → een zuivere **limietcyclus** (Hopf) →
**periodeverdubbeling** → een chaotische **spiraal** rond één evenwicht → de **dubbele scroll** (beide evenwichten) →
een **grote buitenste cyclus** (het verzadigde circuit). Waar die overgangen precies liggen hangt af van β; dat is
offline uitgerekend (`tools/gen_chua_tables.c` → `firmware/src/chua_tables.h`), zodat de CHAOS-knop voor elke SHAPE
hetzelfde regime betekent.

**Toonhoogte** is de tijdschaal van het circuit: per sample schuift de solver h = f_noot × periode / 44100 tijdseenheden
op, met de omlooptijd uit dezelfde tabellen. In het cycle-regime klinkt een noot binnen 2 % op zijn toonhoogte (getest);
in de chaotische regimes rond die toonhoogte, met de typische subharmonische ruis van het scrollen.

## Bediening

| Bediening | Functie |
| --- | --- |
| 27 toetsen | spelen (8 stemmen, elk een eigen circuit), sturen USB-MIDI uit (kanaal 1) |
| KNOB 1 — CHAOS | α: CYCLE 0 · PERIOD 2 32 · SPIRAL 48 · DBL SCROLL 60 · OUTER 112 |
| KNOB 2 — SHAPE | β 10..34: lage waarden trage, ronde spiralen; hoge waarden snelle, strakke |
| KNOB 3 — OUTPUT | welke spanning je hoort: X (het scrollen, diep), X+Y, Y (de spiraal, helder), Y+Z, Z (rond) |
| KNOB 4 — RELEASE | uitklinktijd |
| SELECT | attack |
| ALGORITHM | weergave: 3D (draaiend), X-Y, X-Z, Y-Z, TIME |
| PRESETS | DOUBLE SCROLL, PURE CYCLE, PERIOD TWO, SPIRAL, CRISIS EDGE, OUTER LOOP, SLOW BLOOM, GRIT BASS |
| OCT- / OCT+ | octaaf −3..+3 |
| SAVE | alle instellingen in flash (terug na herstart) |
| MASTER | volume |
| MIDI in (USB en TRS) | note on/off op elk kanaal, CC 123 = all notes off |
| USB | class-compliant MIDI + USB-audio-ingang (opnemen in je DAW) |
| OCT- + OCT+ 5 s ingedrukt | UBOOT (update-modus) |

Het scherm toont de toestand (x, y, z) van de nieuwste stem; speelt er niets, dan een "spookcircuit" dat op de huidige
instellingen meedraait. Dat spookcircuit levert ook de beginstand van nieuwe stemmen (al op de attractor, met een kleine
afwijking per stem zodat akkoorden niet in de pas lopen).

## Waar begin je

Alles wordt als één bestand gecompileerd: `firmware/src/felucca.c` bepaalt de volgorde.

| Bestand | Wat |
| --- | --- |
| `firmware/src/synth.c` | het Chua-circuit: diode, RK2-solver in Q20 fixed point, stemmen, uitgang, de trace voor het scherm |
| `firmware/src/chua_tables.h` | gegenereerd door `tools/gen_chua_tables.c`: α, omlooptijd en RMS per (SHAPE, CHAOS) |
| `firmware/src/ui.c` | de attractor tekenen, knoppen, presets, LEDs, instellingen opslaan |
| `firmware/src/keys.c` | toetsen en MIDI in → noten (draait in de audio-interrupt, elke 32 samples) |
| `firmware/src/audio.c` | de audio-interrupt: `out[]` vullen met Q15 stereo |
| `firmware/src/main.c` | opstarten en hoofdlus (met watchdog, boot-guard, UBOOT en update) |
| `firmware/src/panel.c` | welke matrix-knop welke opdruk heeft |
| `firmware/src/core.h` | gedeelde constanten en de `sys`-struct (hoofdlus ↔ audio) |
| `firmware/src/storage.c` | flash-opslag met A/B-kopieën; `OBJ_USER0..7` zijn vrij |
| `firmware/src/gfx.c`, `lcd.c` | tekenen: tekst (Inter Tight), rechthoeken, lijnen; kleuren via `T_BG`, `T_TEXT`, `T_THEME` … |
| `firmware/src/usb.c`, `midi_uart.c` | USB-MIDI/-audio en TRS MIDI (ongewijzigd uit Felucca) |
| `firmware/src/ota.c`, `ota_hw.c`, `firmware/loader/` | het update-pad: **laten staan** |
| `firmware/hal/` | registers: toetsmatrix, LEDs, encoders, LCD, I2S, ADC, flash, tweede core |

Regels van het huis (uit Felucca, en `tools/build.py` controleert ze): registertoegang alleen in `hal/`, alles in de
audio-interrupt is fixed point (geen 64-bit delingen: die heeft de toolchain niet), en de audio-interrupt moet binnen
~2,9 ms klaar zijn.

**CPU.** Niet op hardware gemeten. Op een pc kost 8 stemmen ~250 ns per sample in het middenregister en ~560 ns bij
hoge noten (4 substappen). Geschat komt dat op de FM-1 uit op 20–50 %. Het scherm toont de belasting (CPU onderaan); kom
je boven ~80 %, zet dan `NVOICE` in `core.h` op 6 of 4, of `CHUA_NSUB` in `synth.c` op 2.

**Tabellen opnieuw maken** (na een andere diode of ander bereik), ~1 minuut:

```
cc -O2 -o build/gen_chua_tables tools/gen_chua_tables.c -lm && build/gen_chua_tables > firmware/src/chua_tables.h
```

## Bouwen, testen, installeren

Zie [BUILDING.md](BUILDING.md). Kort:

```
tools/get_toolchain.sh                 # JieLi-toolchain (eenmalig)
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
pip3 install Pillow fonttools mido python-rtmidi
./build.sh                             # -> build/felucca.fwsc
tests/run_tests.sh                     # host-tests, geen FM-1 nodig (+ build/host/ui/sheet.png en basis.wav)
python3 tools/fm1_install.py build/felucca.fwsc
```

Op macOS draait de toolchain in Docker (zie BUILDING.md); op Linux x86-64 direct.

**Veiligheid.** De installers praten met de draaiende firmware (`ota.c`). Haal dat er niet uit, anders kun je de
volgende build alleen nog via UBOOT installeren. Lukt opstarten twee keer achter elkaar niet binnen 30 s, dan gaat de
FM-1 vanzelf naar UBOOT. Herstel na een mislukte installatie: [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).
Terug naar de officiële firmware: `python3 tools/fm1_install.py FM-1.fwsc` met het V15-bestand van M-VAVE.

## Licentie

De interactieve uitleg (circuit, diode, bifurcatiekaart, speelbaar in de browser met dezelfde tabellen) staat in
`docs/chua.html`.

GPL-3.0-only, net als Felucca ([LICENSE](LICENSE), [LICENSING.md](LICENSING.md), [CREDITS.md](CREDITS.md)). Deel je
een build, deel dan ook de broncode. De namen `FELUCCA_*` in de code en de USB-naam "Felucca" zijn nog die van het
origineel; hernoem ze gerust (`firmware/src/usb.c` STR1/STR2 voor de USB-naam, met een eigen VID/PID vóór je iets uitbrengt).
Let op: `tools/fm1_install.py` zoekt de FM-1 op poortnaam (`PORT_RE`); pas die mee aan, of gebruik `--port`.
