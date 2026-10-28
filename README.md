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
| KNOB 4 — RING | resonantie van de low pass gate: 0 geen, 127 Q 6 (een korte, houtachtige ring) |
| HOME | de oscillator: de circuitpagina (CHAOS, SHAPE, OUTPUT, RING en de attractor) |
| ENV | wisselt naar de ENV-pagina: KNOB 1–4 = ATTACK, DECAY, SUSTAIN, RELEASE (0,5 ms .. 10 s), met de envelope getekend |
| FX | de FX-pagina's: 1× de resonatoren van Mutable Instruments Rings, 2× de shimmer reverb, 3× terug naar HOME |
| SELECT | RING (circuit- en ENV-pagina), POSITION (resonator), TONE (shimmer) |
| ALGORITHM | weergave: 3D (draaiend), X-Y, X-Z, Y-Z, TIME |
| PRESETS | DOUBLE SCROLL, RING PLUCK, PURE CYCLE, PERIOD TWO, SPIRAL, BONGO GATE, CRISIS EDGE, OUTER LOOP, SLOW BLOOM, GRIT BASS, RINGS BELL, SITAR DRONE, CHAOS PIANO, BURIED BAR, HEAVEN RISER, OCTAVE CLOUD, FIFTH ASCENT |
| OCT- / OCT+ | octaaf −3..+3 |
| SAVE | alle instellingen in flash (terug na herstart) |
| GLO | tweede core aan/uit (vanaf de volgende start) |
| MASTER | volume |
| MIDI in (USB en TRS) | note on/off op elk kanaal, CC 123 = all notes off |
| USB | class-compliant MIDI + USB-audio-ingang (opnemen in je DAW) |
| OCT- + OCT+ 5 s ingedrukt | UBOOT (update-modus) |

**De low pass gate.** De ADSR stuurt per stem een low pass gate aan, zoals een vactrol dat doet: de stuurspanning volgt
de envelope snel omhoog (~2 ms) en trager omlaag (~30 ms), en zet zowel het volume als de cutoff (25 Hz .. 15 kHz). Het
filter is een resonant state-variable low pass (zero-delay feedback, stabiel bij elke cutoff en resonantie). Met RING
hoog rinkelt het: een "ping" als de gate opengaat, en een staart die doorklinkt terwijl hij dichtgaat. Probeer RING
PLUCK of BONGO GATE (sustain 0: elke noot is een aanslag).

**De resonatoren (FX).** De resonatormodellen van Mutable Instruments Rings, gevoed door de Chua-klank zoals de
IN-ingang van Rings zijn resonator voedt, en gestemd op de laatst gespeelde noot. De DSP is de fixed-point port van
Rings en DaisySP (Emilie Gillet, Electrosmith; MIT) die Felucca al had (`phys_dsp.c`, `phys_symp.c`).

| Model (ALGORITHM) | Wat | KNOB 1 STRUCTURE | SELECT |
| --- | --- | --- | --- |
| MODAL | 12 gestemde modes: klokken, staven, platen | rek van de boventonen (34 harmonisch, 70 klok, 116 staaf) | aanslagpositie |
| SYMP STRING | een snaar met drie meeresonerende snaren | het akkoord: OCT 5TH 4TH MAJ MIN SUS 7TH OCTAVES | hoeveel ze meenemen |
| STRING | een Karplus-Strong-snaar | onder ~32 een gebogen kam (sitar), daarboven dispersie (piano) | — |

KNOB 2 BRIGHTNESS, KNOB 3 DAMPING (zoals Rings: rechtsom langer), KNOB 4 MIX (0 = uit, dan rekent er niets). Het
scherm toont de resonanties op een frequentie-as (de meeresonerende snaren blauw) en de uitgang eronder.

**De shimmer reverb (FX, tweede scherm).** Een stereo galm (4 vertragingslijnen met een Householder-matrix) met een
toonhoogteverschuiver in de terugkoppeling. Elke ronde door de galm (60–110 ms) schuift een deel van de staart een
interval omhoog en gaat terug de galm in, met behoud van energie: de staart klimt, de lage octaven vervagen terwijl de
hoge aanzwellen, tot TONE de top dempt. Dat is de riser.

| Bediening | Functie |
| --- | --- |
| KNOB 1 SIZE | de ruimte: 60–110 ms lange lijnen (glijdt bij draaien) |
| KNOB 2 DECAY | nagalmtijd 0,4–30 s |
| KNOB 3 SHIMMER | hoe snel het klimt: 0 is een gewone galm, 127 zo'n 2–3 octaven per seconde |
| KNOB 4 MIX | droog/nat; 0 = uit (dan rekent er niets) |
| ALGORITHM | het interval: +OCT, +5TH, +OCT+5TH, +2 OCT |
| SELECT | TONE: donker (1,5 kHz) tot open (16 kHz) |

Het scherm toont het niveau van de staart over de laatste ~4 seconden (je ziet de riser aanzwellen en wegsterven) en
rechts de trap van het interval. De uitgang is nu stereo; met de shimmer uit blijft hij mono en bit voor bit hetzelfde.

Het scherm toont de toestand (x, y, z) van de nieuwste stem; speelt er niets, dan een "spookcircuit" dat op de huidige
instellingen meedraait. Dat spookcircuit levert ook de beginstand van nieuwe stemmen (al op de attractor, met een kleine
afwijking per stem zodat akkoorden niet in de pas lopen).

## Waar begin je

Alles wordt als één bestand gecompileerd: `firmware/src/felucca.c` bepaalt de volgorde.

| Bestand | Wat |
| --- | --- |
| `firmware/src/synth.c` | het Chua-circuit: diode, RK2-solver in Q20 fixed point, stemmen, uitgang, de trace voor het scherm |
| `firmware/src/shimmer.c` | de shimmer reverb: FDN-galm met octaafverschuiver in de terugkoppeling |
| `firmware/src/resonator.c` | de FX-resonatoren rond de Rings/DaisySP-port (`phys_dsp.c`, `phys_symp.c`, MIT) |
| `firmware/src/lpg_tables.h` | gegenereerd door `tools/gen_lpg_tables.py`: ADSR-tijden, cutoff, resonantie |
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

**CPU en de tweede core.** Met 8 stemmen op één core staat de belasting op ~80 %. De AC79 heeft een tweede core; CHUA
verdeelt de stemmen (om en om) over beide. De tweede core start alleen bij het opstarten, dus **GLO** zet hem aan of
uit voor de volgende start (meteen opgeslagen; standaard aan). Onderaan het scherm staat dan `CPU …% x2`. Op sommige
FM-1's leest de tweede core verkeerd (zo gevonden door X0X en Jangada): dan merkt de firmware dat, gaat verder op één
core en zet de instelling uit (`2ND CORE STOPPED: OFF`). De uitvoer is bit voor bit gelijk, met één of twee cores
(getest, ook als echte thread onder ThreadSanitizer). Is het nog te zwaar, zet dan `NVOICE` in `core.h` op 6.

**Tabellen opnieuw maken** (na een andere diode of ander bereik), ~1 minuut:

```
cc -O2 -o build/gen_chua_tables tools/gen_chua_tables.c -lm && build/gen_chua_tables > firmware/src/chua_tables.h
```

## Installeren vanaf GitHub

Elke push naar `main` bouwt de firmware op GitHub (Actions), draait de tests en zet de installer online:

**https://plieuw.github.io/fm1pokoe/** → open in Chrome of Edge, sluit de FM-1 met een USB-datakabel aan, druk op
*Installeren*. Volgende versies gaan op dezelfde manier; terug naar de officiële firmware kan op dezelfde pagina.

- Het `.fwsc`-bestand zelf staat bij elke run onder *Actions → firmware → Artifacts*.
- Een tag `v1.0` (één cijfer per deel) maakt een release-build en een GitHub Release met het bestand.
- Eenmalig instellen: *Settings → Pages → Build and deployment → Source: GitHub Actions*.

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
