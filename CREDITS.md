# Credits

This firmware is built on **[Felucca](https://github.com/hugelton/Felucca)** by Leo Kuroshita
([@kurogedelic](https://github.com/kurogedelic), [Hügelton Instruments](https://hugelton.com)): the HAL (keys, LEDs,
encoders, display, audio, USB, flash, second core), the boot and safety code, the USB-MIDI and USB audio driver, the
TRS MIDI input, the flash store, the M-UPGRADE update entry, the update loader, the package builder, the installers
and the drawing code all come from Felucca. Each part keeps its own licence (see [LICENSE](LICENSE),
[LICENSING.md](LICENSING.md) and [LICENSES/](LICENSES/)).

- FX resonators: the models of [Rings](https://github.com/pichenettes/eurorack) by Emilie Gillet (Mutable Instruments) and
  [DaisySP](https://github.com/electro-smith/DaisySP) by Electrosmith and Emilie Gillet ([MIT](LICENSES/MIT-Rings.txt),
  [MIT](LICENSES/MIT-DaisySP.txt)), in Felucca's fixed-point port
- Noise oscillators: after the programs of [Befaco](https://www.befaco.org)'s **Noise Plethora** (designed by Jeremy
  Bernstein, Julia Mugica and Ivan Paz with Befaco), written from the descriptions in its
  [manual](https://www.befaco.org/docs/Noise_Plethora/Noise_Plethora.pdf); this firmware's own code (`firmware/src/noise.c`),
  no code of the module's
- Font: [Inter Tight](https://github.com/rsms/inter-tight) by The Inter Project Authors, [SIL OFL 1.1](LICENSES/OFL-InterTight.txt)
- Second core: its start, mailbox and guards from [X0X](https://github.com/charlesvestal/fm1-x0x) by
  [charlesvestal](https://github.com/charlesvestal) with the dual-core findings of [Melodee](https://github.com/keremimo/melodee)
  by [keremimo](https://github.com/keremimo) and the hardening of [Jangada](https://github.com/zednaked/jangada) by
  [zednaked](https://github.com/zednaked) (all GPL-3.0)
- Package format and boot files: [JieLi AC79 SDK](https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK)
  ([Apache-2.0](LICENSES/Apache-2.0.txt); three of its files are in every package, none in this tree)
- Felucca's contributors to the parts kept here: [ChanceTheMaker](https://github.com/ChanceTheMaker) (TRS MIDI),
  [gospodindilly-ops](https://github.com/gospodindilly-ops) (the second core), and everyone listed in Felucca's own CREDITS.md
