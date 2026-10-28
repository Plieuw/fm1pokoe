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
| 27 toetsen | spelen (4 stemmen, elk een eigen circuit en een eigen resonator/waveguide), sturen USB-MIDI uit (kanaal 1) |
| KNOB 1 — CHAOS | α: CYCLE 0 · PERIOD 2 32 · SPIRAL 48 · DBL SCROLL 60 · OUTER 112 |
| KNOB 2 — SHAPE | β 10..34: lage waarden trage, ronde spiralen; hoge waarden snelle, strakke |
| KNOB 3 — OUTPUT | welke spanning je hoort: X (het scrollen, diep), X+Y, Y (de spiraal, helder), Y+Z, Z (rond) |
| KNOB 4 — RING | resonantie van de low pass gate: 0 geen, 127 Q 6 (een korte, houtachtige ring) |
| HOME | de oscillator: het circuit (CHAOS, SHAPE, OUTPUT, RING en de attractor) of een noise-programma (X, Y, LEVEL, RING); nog een keer: de MOD-pagina (de FM-oscillator), nog een keer: terug |
| ENV | 1× de ENV-pagina, 2× de MOD ENV (de AD-envelope van de FM-oscillator), 3× terug naar HOME. ENV-pagina: KNOB 1–4 = ATTACK, DECAY, SUSTAIN, RELEASE (0,5 ms .. 10 s), met de envelope getekend |
| FX | de FX-pagina's: 1× de resonatoren van Mutable Instruments Rings en de waveguides (THROAT, FLUTE, SYRINX), 2× de shimmer reverb, 3× terug naar HOME |
| SELECT | weergave op HOME: 3D (draaiend), X-Y, X-Z, Y-Z, TIME · POSITION (resonator) · TONE (shimmer) |
| ALGORITHM | op HOME de oscillator: CHUA of een van de 30 noise-programma's A0..C9 · het model (resonator of waveguide) · het interval (shimmer) |
| PRESETS | DOUBLE SCROLL, RING PLUCK, PURE CYCLE, PERIOD TWO, SPIRAL, BONGO GATE, CRISIS EDGE, OUTER LOOP, SLOW BLOOM, GRIT BASS, RINGS BELL, SITAR DRONE, CHAOS PIANO, BURIED BAR, HEAVEN RISER, OCTAVE CLOUD, FIFTH ASCENT, RADIO OH NO, GRAIN SHOWER, PRIME HATS, FIBONACCI PAD, CLUSTER BELL, SATAN WORKOUT, BITCRUSH WALK, FM BELL CLUSTER, SQUARE GRAINS, SAW RADIO, TRI WOBBLE PAD, FM PLUCK |
| ARP | de ARP-pagina; nog een keer: de arpeggiator aan/uit (het lampje brandt zolang hij aan staat) |
| REC | de FREE EG-pagina; nog een keer REC: opnemen (bij NOTE nog een keer om te stoppen) |
| SCL | de PAN-pagina (nog een keer: terug naar HOME): KNOB 1 BASIS (stereo-spreiding per noot), KNOB 2 ORBIT (de stemmen bewegen mee met hun plaatje); de stemmen staan getekend waar ze in het stereobeeld klinken |
| SEQ | de KARMA-pagina (de frasegenerator); nog een keer: KARMA aan/uit (het lampje brandt zolang hij aan staat) |
| OCT- / OCT+ | octaaf −3..+3 |
| SAVE | alle instellingen in flash (terug na herstart) |
| GLO | de MIDI-pagina (nog een keer: terug naar HOME): KNOB 1 TRS MIDI IN aan/uit, KNOB 2 USB MIDI (OFF, IN, OUT, IN + OUT), KNOB 3 RX-kanaal (OMNI, 1..16), KNOB 4 TX-kanaal (1..16); SELECT de tweede core aan/uit (vanaf de volgende start) |
| MASTER | volume |
| MIDI in (USB en TRS) | note on/off op het RX-kanaal (of elk: OMNI), CC 123 = all notes off; clock voor de arpeggiator |
| USB | class-compliant MIDI + USB-audio-ingang (opnemen in je DAW) |
| OCT- + OCT+ 5 s ingedrukt | UBOOT (update-modus) |

**De noise-oscillators.** Draai op HOME aan ALGORITHM: na CHUA komen 30 noise-programma's naar de Noise Plethora
van Befaco (drie banken van tien, zoals op de module). Het zijn eigen fixed-point versies van wat de handleiding per
programma beschrijft, gemaakt voor een keyboard: de noot zet de hoofdfrequentie, KNOB 1 en 2 zijn X en Y van het
programma (de cel toont wat ze doen), KNOB 3 het niveau, KNOB 4 de RING van de gate. Elke stem draait een eigen kopie,
door dezelfde ADSR en low pass gate (en de FX) als het circuit. Het scherm tekent de uitgang tegen zichzelf een paar
samples eerder, een "attractor" van de ruis; met SELECT op TIME zie je de golfvorm.

| Bank | Programma's (X / Y) |
| --- | --- |
| A — textures | A0 RADIO OH NO (osc 2 / PWM) · A1 RWALK FLANGE (loopsnelheid / flanger) · A2 XMOD RING SQR · A3 XMOD RING SINE (osc 2 / FM) · A4 CROSS MOD RING (verhoudingen / FM) · A5 RESONOISE (mod-verhouding / fold) · A6–A8 GRAIN GLITCH 1–3 (korrel / FM) · A9 BASURILLA (LFO-snelheid / breedte) |
| B — HH clusters | B0 CLUSTER SAW (spreiding / aantal stemmen) · B1 PW CLUSTER (ontstemming / pulsbreedte) · B2 CR CLUSTER · B3 SINE FM CLUST · B4 TRI FM CLUST (ontstemming / FM) · B5 PRIME CLUSTER (spreiding / FM) · B6 PRIME C NOISE (spreiding / ruis-FM) · B7 FIBONACCI · B8 PARTIAL CLUST (spreiding / toon) · B9 PHASING CLUST (spreiding / drift) |
| C — harsh & wild | C0 BASURA TOTAL (LFSR / galm) · C1 ATARI · C2 WAKING FILOM. (osc 2 of box / FM) · C3 P S H (snelheid / galm) · C4 ARRAY ROCKS (mod-verhouding / husselen) · C5 EXIST. PAIN · C6 WHO KNOWS (sweep-snelheid / diepte) · C7 SATAN WORKOUT (FM / crush) · C8 RWALK BITCRUSH · C9 RWALK LFREE (box / galm) |

Ze zijn op hetzelfde niveau gezet als het circuit (bij X = Y = 64, gemeten in de test) en blijven in elke hoek van X en
Y begrensd en zonder DC.

**De FM-oscillator (MOD).** Druk op HOME terwijl je al op HOME staat: de MOD-pagina. Per stem een extra oscillator
die de toonhoogte van het noise-programma moduleert (lineaire FM): alle oscillators in het programma bewegen mee, dus
ook de clusters. Hij werkt op alle 30 programma's, niet op het Chua-circuit.

| Bediening | Functie |
| --- | --- |
| KNOB 1 WAVE | de golfvorm, morphend: sine (0) → triangle (42) → saw (85) → square (127) |
| KNOB 2 RATIO | de frequentie ten opzichte van de noot, in stappen: 1/4, 1/2, 3/4, 1, 5/4, 3/2, 2, 5/2, 3, 7/2, 4, 5, 6, 7, 8, 11 |
| KNOB 3 DEPTH | hoe diep: 0 = uit (rekent dan niet), 127 = ±150 % van de frequentie (onder nul stopt hij op 1/16) |
| KNOB 4 FINE | ±1 halve toon ontstemd: langzaam zwevende FM |

Het scherm toont links de golfvorm en rechts een sinus die erdoor gemoduleerd wordt (twee periodes van de noot).

**De MOD ENV (ENV, tweede keer).** Druk op ENV terwijl je al op de ENV-pagina staat: een AD-envelope voor de
FM-oscillator, gestart door elke noot (hij loopt zijn vorm af, ook als je de toets eerder loslaat).

| Bediening | Functie |
| --- | --- |
| KNOB 1 LENGTH | de hele duur, 0,5 ms .. 10 s |
| KNOB 2 SKEW | waar de piek valt: 0 = meteen (FALL: een aanslag die wegvalt), 64 = in het midden, 127 = aan het eind (RISE: aanzwellen, dan weg) |
| KNOB 3 HEIGHT | hoeveel FM de envelope toevoegt bovenop DEPTH (0 = geen envelope) |

DEPTH (MOD-pagina) is de vaste FM die er altijd is, HEIGHT het deel dat de envelope er bovenop zet: samen zijn ze
begrensd op het maximum. DEPTH 0 en HEIGHT hoog geeft alleen FM in de envelope (FM PLUCK); DEPTH hoog en HEIGHT 0 is
vaste FM zoals voorheen. Het scherm toont de vorm boven de DEPTH-lijn en rechts de FM-diepte van de nieuwste stem nu.
Nog een keer ENV: terug naar HOME.

**BASIS (SCL, KNOB 1).** De stereo-spreiding van de stemmen. Elke noot die je speelt krijgt de volgende
plek in een vaste cyclus: links, rechts, half links, half rechts, driekwart links, driekwart rechts, kwart links, kwart
rechts, en weer van voren af aan. BASIS schaalt die plekken: 0 is mono (precies zoals voorheen), 127 de volle breedte.
Ook noten die al klinken schuiven mee als je draait. De resonator van een noot klinkt op dezelfde plek als die noot; de
shimmer krijgt het midden en maakt er zijn eigen stereo van. De pan-wet is een balans: het midden blijft op vol niveau,
de andere kant gaat zachter (bij volle breedte uit).

**ORBIT (SCL, KNOB 2).** De tweede pan-parameter naast BASIS: elke stem beweegt ook mee met zijn eigen
plaatje. Bij het Chua-circuit is dat de x van de attractor: in de double scroll springt de stem mee van de ene lus
(links) naar de andere (rechts), in één lus (SPIRAL) blijft hij aan één kant hangen, bij een cyclus staat hij stil.
Het rondcirkelen binnen een lus (op de toonhoogte van de noot) wordt eruit gefilterd (~7 Hz). Een noise-programma (zijn
plaatje is ruis) zwerft elke ~0,2 s naar een nieuwe willekeurige plek. ORBIT bepaalt hoe ver; het komt bovenop de
plek van BASIS (samen hooguit helemaal links of rechts), dus ORBIT werkt ook met BASIS op 0. De resonator van een noot
beweegt mee met zijn stem. De PAN-pagina tekent het stereobeeld van links naar rechts: de plekken van BASIS (de
volgende noot krijgt de hoge streep) en de stemmen die klinken, met hun noot, waar ze nu staan.

**De arpeggiator (ARP).** ARP opent de ARP-pagina; nog een keer ARP zet hem aan of uit (het lampje brandt zolang
hij aan staat). Aan: de noten die je vasthoudt (toetsen en MIDI in) klinken niet zelf, de arpeggiator speelt ze.
Zolang er MIDI clock binnenkomt (USB of TRS, 24 per kwartnoot) loopt hij daarop mee (START begint het patroon
opnieuw); komt er 0,5 s geen clock, dan loopt hij op zijn eigen BPM. Naar de arpeggiators van de Yamaha AN1x en DJX.
Wat hij speelt gaat over USB-MIDI naar buiten (als USB OUT aan staat). ARP en KARMA (SEQ) staan nooit tegelijk aan.

| Bediening | Functie |
| --- | --- |
| KNOB 1 BPM | 40..250 (onder MIDI clock staat er EXT en het gemeten tempo) |
| KNOB 2 SUBDIVIDE | de tien van de AN1x: 3/8, 1/4, 3/16, 1/6, 1/8, 3/32, 1/12, 1/16, 1/24, 1/32 |
| KNOB 3 GATE | hoe lang elke noot klinkt: 8..100 % van de stap |
| KNOB 4 SWING | 50 % (geen) .. 83 % (de meeste), zoals de AN1x: elke tweede stap later |
| ALGORITHM | het patroon (zie hieronder) |
| SELECT | HOLD: rechtsom aan (de noten blijven spelen als je loslaat, een nieuw akkoord vervangt ze), linksom uit |

De patronen: de dertig Arpeggiator Types van de AN1x (uit zijn Data List) en twee van de DJX. 1–15: UP, DOWN,
UPDW A (de uitersten één keer), UPDW B (de uitersten twee keer) en RANDOM, elk over 1, 2 of 4 octaven. 16–30: TECHNO-A
(Euro), TECHNO-B (UK, met velocity), TECHNO-C (Japans), TECHNO-D (Duits), DA HOUSE (bas links, akkoorden rechts),
SYNCOPA A, SYNCOPA B (grote octaafsprongen), SYNCO ECHO, TEKK ECHO A en B, PULSE LINE, BASSLINE A–D. Daarna ECHO en
SWEEP van de DJX. De Data List geeft van 16–30 alleen de naam en één regel commentaar, de handleidingen geen noten: die
stappen zijn mijn invulling van die beschrijvingen ("moving filter" bij TEKK ECHO als een velocity-zwaai). Het scherm
toont de laatste 32 stappen als een rol.

**FREE EG (REC).** Neemt een knopbeweging op en speelt die af op een parameter, zoals de Free EG van de AN1x. REC
opent de pagina; daar zet KNOB 4 het doel (TARGET: CHAOS, SHAPE, RING, NOISE X/Y, MOD DEPTH, FX STRUCTURE/BRIGHTNESS/
DAMPING/MIX, SHIMMER, BASIS, ORBIT, ARP GATE/SWING, KARMA K1..K4, ...), KNOB 2 de lengte en KNOB 3 de diepte. Druk REC en draai aan
KNOB 1: de beweging wordt opgenomen (128 punten) en je hoort hem meteen op het doel. SELECT rechtsom speelt hem af
(linksom uit: de parameter krijgt zijn oude waarde terug).

| LENGTH | Wat |
| --- | --- |
| NOTE | elke noot (ook van de arpeggiator en KARMA) start hem opnieuw; hij speelt één keer, zo lang als je opnam (REC nog een keer stopt, max ~6 s), en houdt dan de laatste waarde |
| 1..8 STEPS | een lus van 1 tot 8 zestiende noten |
| 1..4 BARS | een lus van 1 tot 4 maten (4/4) |
| PHRASE | precies zo lang als de frase van KARMA (5/4, 6/8, 3 tegen 4: wat de GE ook is) en elke keer dat de frase opnieuw begint begint hij mee; zonder KARMA één maat |

STEPS en BARS lopen op de BPM van de ARP-pagina, of op MIDI clock als die binnenkomt (START begint de lus opnieuw);
de opname begint aan het begin van een lus en stopt vanzelf na één lengte. DEPTH 127 speelt de opname precies af, lager
schaalt hij rond de waarde die de parameter had; zolang hij speelt neemt hij de eigen knop van die parameter over.

**KARMA (SEQ).** Een frasegenerator naar KARMA van Korg (Kay Algorithmic Realtime Music Architecture, Stephen
Kay; de KARMA Basic Guide). De noten die je vasthoudt klinken niet zelf: een *Generated Effect* (GE) maakt er een
muzikale frase van, in de maat, zolang je ze vasthoudt (of met LATCH). Speel een ander akkoord en de frase volgt het
meteen. De architectuur is die van KARMA, de code en de frases zijn nieuw (KARMA's eigen GE's zijn niet openbaar).

Een GE is, zoals bij KARMA, een paar kleine generatoren die samenwerken:

| Generator | Wat hij doet |
| --- | --- |
| RHYTHM | een patroon van stappen: zware, lichte en spook-aanslagen, rusten, overbindingen, lange noten; in 1/32, 1/16, 1/8-triolen, 1/8 of 1/4 |
| NOTE SERIES | waar de frase van gemaakt is: het akkoord, het akkoord met de grondtoon erboven, of de toonladder die het akkoord suggereert (majeur of mineur, diatonisch of pentatonisch), over 1–4 octaven |
| INDEX | de weg door die noten: op, neer, op en neer, willekeurig, een *random walk*, een spiraal (laag, hoog, steeds meer naar het midden) of *Direct Index* (elke stap noemt zijn noot) |
| CLUSTER + STRUM | hoeveel noten per aanslag (een akkoordstoot), uitgespreid als een gitaar: omlaag, dan omhoog |
| MELODIC REPEAT | elke noot echoot een paar keer, getransponeerd en zachter (cascades, echo's in toonhoogte), afwisselend links en rechts |
| VELOCITY / GATE | een accentpatroon en een lengte per GE |
| PAN | per noot: afwisselend, willekeurig, een zwaai over de frase, of naar toonhoogte (laag links); BASIS (SCL) zet hoe breed |
| ENV | een vorm over de frase (op, neer, op-en-neer) op een parameter van de klank: RING, FX BRIGHTNESS, CHAOS, OUTPUT, SHIMMER MIX ...; als hij stopt krijgt de parameter zijn waarde terug |

| Bediening (SEQ-pagina) | Functie |
| --- | --- |
| SEQ | nog een keer: KARMA aan/uit |
| ALGORITHM | de GE (24, zie hieronder) |
| KNOB 1..4 | de vier *Realtime Controls* van die GE; de cellen tonen wat ze doen. 64 is de GE zoals hij geschreven is |
| SELECT | LATCH: rechtsom aan (de frase loopt door als je loslaat, een nieuw akkoord vervangt het oude), linksom uit |
| PRESETS | het tempo (BPM 40..250, hetzelfde als KNOB 1 op de ARP-pagina); op deze pagina kies je dus geen preset |

De Realtime Controls: DENSITY (0 niets, laag alleen de zware aanslagen, 64 het patroon, hoger ook de spooknoten en
boven 96 noten in de rusten), RANGE (−3..+3 octaven), REPEAT (meer of minder echo's), ACCENT (dynamiek, boven 96 ook
toeval), GATE, RANDOM (af en toe een andere noot, boven 96 octaafsprongen), CLUSTER, TRANSPOSE (octaven), ENV (diepte
van de envelope) en STRUM.

De GE's: PULSE ARP (de klassieke 16e-arp met accenten en spooknoten), CASCADE (dalende kwarten die echoën), HARP GLISS
(een glissando door de toonladder), STRUM GUITAR, FINGERPICK (Travis-achtig: bas, dan de akkoordtonen), TECHNO STAB
(akkoordstoten, RING zwelt over de frase), ACID LINE (303-achtig, met overbindingen en octaven), BASS WALK (een
wandelende bas door de toonladder), OCTAVE BASS, TRANCE GATE, RAIN DROPS (pentatonisch, willekeurig, met echo's),
SPIRAL, BROKEN 6/8, GAMELAN (een kotekan-achtig pentatonisch patroon met echo een octaaf hoger), ECHO CANYON (twee
noten, vier echo's), FUNK CLAV, POLY 3 OVER 4 (een figuur van drie zestienden tegen de maat), SWELL PAD (lange
akkoorden, SHIMMER MIX zwelt), CHAOS MACHINE (de toonladder over vier octaven terwijl CHAOS oploopt: het circuit raakt
steeds verder uit zijn baan), SCROLL HOPPER (OUTPUT golft over de frase: van de scroll naar de spiraal en terug), NOISE
PERC (voor de noise-programma's: korte tikken laag en hoog), BIRD CALLS (trillers in 1/32 en stilte: mooi met de
SYRINX-waveguide), MINIMAL 5/4 (Reich-achtig, 20 zestienden, met echo een octaaf hoger) en LULLABY. De GE's met een
envelope (TECHNO STAB, ACID LINE, TRANCE GATE, SWELL PAD, CHAOS MACHINE, SCROLL HOPPER, MINIMAL 5/4) spelen dus ook
met de klank, niet alleen met de noten.

Het scherm toont het ritme van de GE (hoogte = gewicht, het spelende stapje licht op) en eronder de laatste 32 noten
als een rol; onderin de klok en de lengte ("20 x 1/16").

**KARMA, de ARP en FREE EG samen.**
- *Eén tegelijk.* KARMA en de ARP pakken allebei de noten die je vasthoudt; zet je de een aan, dan gaat de ander uit
  en de vastgehouden noten gaan mee over. Zet je hem uit, dan klinkt het akkoord gewoon.
- *Eén tempo.* KARMA loopt op de BPM van de ARP-pagina, of op MIDI clock als die binnenkomt (START begint de frase
  opnieuw), en neemt de SWING van de ARP-pagina over voor zijn zestienden (of achtsten). Een nieuwe frase begint op
  de volgende zestiende (KARMA's *Quantize Trigger*); vanuit stilte meteen.
- *FREE EG.* LENGTH PHRASE loopt precies met de frase van KARMA mee (opnemen begint aan het begin van de frase) en de
  TARGETs KARMA K1..K4 zijn de vier Realtime Controls. Zo neem je bijvoorbeeld een DENSITY-beweging op die over de
  frase opbouwt, of een REPEAT die alleen aan het eind uitwaaiert. FREE EG NOTE start bij elke noot die KARMA maakt
  opnieuw: een eigen envelope per noot van de frase. Als FREE EG en de envelope van een GE dezelfde parameter willen,
  wint FREE EG.
- *MIDI uit.* Wat KARMA en de ARP maken gaat over USB-MIDI naar buiten op het TX-kanaal (als USB OUT aan staat op de
  GLO-pagina), zoals KARMA zijn gegenereerde noten uitstuurt; de toetsen zelf dan niet.

**Elke parameter op één plek.** RING staat alleen op HOME (KNOB 4), BASIS en ORBIT alleen op de PAN-pagina (SCL), de
oscillator kies je alleen met ALGORITHM op HOME (op de MOD-pagina niet meer).

**De low pass gate.** Op de ENV-pagina is de gate een rond rood lampje (het LED van een vactrol): het brandt fel als de gate open staat en dimt mee met de envelope; ernaast de cutoff. De ADSR stuurt per stem een low pass gate aan, zoals een vactrol dat doet: de stuurspanning volgt
de envelope snel omhoog (~2 ms) en trager omlaag (~30 ms), en zet zowel het volume als de cutoff (25 Hz .. 15 kHz). Het
filter is een resonant state-variable low pass (zero-delay feedback, stabiel bij elke cutoff en resonantie). Met RING
hoog rinkelt het: een "ping" als de gate opengaat, en een staart die doorklinkt terwijl hij dichtgaat. Probeer RING
PLUCK of BONGO GATE (sustain 0: elke noot is een aanslag).

**De resonatoren (FX).** De resonatormodellen van Mutable Instruments Rings, gevoed door de oscillators zoals de
IN-ingang van Rings zijn resonatoren voedt. Polyfoon, zoals Rings op polyfonie 4: vier resonatoren, elk gestemd op een
noot. Elke stem voedt alleen de resonator van zijn eigen noot, dus een akkoord klinkt als akkoord. Een nieuwe noot
neemt de resonator die die noot al heeft, anders een vrije (de langst ongebruikte), anders de oudste; die klinkt dan
verder op de nieuwe toonhoogte, net als bij Rings. Resonatoren die stil zijn rekenen niet mee, en de tweede core neemt
de helft van de klinkende voor zijn rekening. Het scherm toont de resonator van de nieuwste noot. De DSP is de fixed-point port van
Rings en DaisySP (Emilie Gillet, Electrosmith; MIT) die Felucca al had (`phys_dsp.c`, `phys_symp.c`).

| Model (ALGORITHM) | Wat | KNOB 1 STRUCTURE | SELECT |
| --- | --- | --- | --- |
| MODAL | 12 gestemde modes: klokken, staven, platen | rek van de boventonen (34 harmonisch, 70 klok, 116 staaf) | aanslagpositie |
| SYMP STRING | een snaar met drie meeresonerende snaren | het akkoord: OCT 5TH 4TH MAJ MIN SUS 7TH OCTAVES | hoeveel ze meenemen |
| STRING | een Karplus-Strong-snaar | onder ~32 een gebogen kam (sitar), daarboven dispersie (piano) | — |

KNOB 2 BRIGHTNESS, KNOB 3 DAMPING (zoals Rings: rechtsom langer), KNOB 4 MIX (0 = uit, dan rekent er niets). Het
scherm toont de resonanties op een frequentie-as (de meeresonerende snaren blauw) en de uitgang eronder.

**De waveguides (FX, achter de resonatoren).** Draai op de FX-pagina verder met ALGORITHM: na MODAL, SYMP STRING en
STRING komen drie waveguides, eigen code (naar de klassieke modellen: het vocal tract van Kelly en Lochbaum, de fluit
van Cook / STK). Net als de resonatoren polyfoon (één per noot) en gevoed door de stemmen: de oscillator is de bron.
De luidheid van de stemmen is de "adem": de fluit en de syrinx blazen alleen zolang je speelt.

| Model | Wat | KNOB 1 | SELECT |
| --- | --- | --- | --- |
| THROAT | een strottenhoofd/keel: een buis van tot 24 secties met de oscillator als stembanden. De lengte geeft de formanten van het dier, de mond gaat open met de luidheid (miauw, boe) | ANIMAL: CAT, MONKEY, HUMAN, DOG, WOLF, COW, LION, ELEPHANT | VOWEL: U, O, A, E, I (morphend) |
| FLUTE | een luchtstraal over een buis op de toonhoogte van de noot | JET: laag = grondtoon (buigt iets omhoog), hoger = overblazen naar octaaf en duodecime | BREATH: de ruis in de adem |
| SYRINX | het stemorgaan van een vogel: twee fluitjes twee octaven boven de noot, door de snavel | CHIRP: hoeveel de toon stijgt met de adem | TWO VOICE: de tweede fluit 0..7 halve tonen hoger (zweven .. kwint) |

KNOB 2 BRIGHTNESS (mondopening/snavel/demping van de hoogte), KNOB 3 DAMPING (hoe lang de buis naklinkt), KNOB 4 MIX.
Combineer gerust: een noise-programma als adem in de FLUTE (PAN NOISE FLUTE), de pure cyclus in een kattenkeel (CAT
MEOW), een wolf met shimmer (WOLF HOWL). Nieuwe presets: CAT MEOW, COW MOO, WOLF HOWL, TALKING CHAOS, ELEPHANT, NIGHT
FLUTE, OVERBLOWN, PAN NOISE FLUTE, BIRD SONG, TWO BIRDS.

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
| `firmware/src/noise.c` | de 30 noise-oscillators (naar Befaco's Noise Plethora) |
| `firmware/src/shimmer.c` | de shimmer reverb: FDN-galm met octaafverschuiver in de terugkoppeling |
| `firmware/src/waveguide.c` | de waveguides: THROAT (Kelly-Lochbaum), FLUTE, SYRINX |
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

**CPU en de tweede core.** CHUA speelt 4 stemmen, net zoveel als er FX-resonatoren/waveguides zijn (één per noot):
met de waveguides erbij was 8 te zwaar. Een vijfde noot neemt de oudste stem over. De AC79 heeft een tweede core; CHUA
verdeelt de stemmen en de resonatoren (om en om) over beide. De tweede core start alleen bij het opstarten, dus **SELECT op de GLO-pagina** zet hem aan of
uit voor de volgende start (meteen opgeslagen; standaard aan). Onderaan het scherm staat dan `CPU …% x2`. Op sommige
FM-1's leest de tweede core verkeerd (zo gevonden door X0X en Jangada): dan merkt de firmware dat, gaat verder op één
core en zet de instelling uit (`2ND CORE STOPPED: OFF`). De uitvoer is bit voor bit gelijk, met één of twee cores
(getest, ook als echte thread onder ThreadSanitizer). `NVOICE` in `core.h` zet het aantal stemmen.

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
