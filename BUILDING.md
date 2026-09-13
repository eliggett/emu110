# How to build:

Cloning for the first time?  Read [CLONING.md](CLONING.md) instead -- the
submodules and the DPF patches have to be in place before any of this works.

## Check the artwork export:

```bash
plugin/tools/panel_export.py --check
```

## Force artwork regen: 
```bash
plugin/tools/panel_export.py --text-to-path --background
```

## Compile:
```bash
cd plugin && make -j5
```

Products land in `plugin/bin/`: the LV2 bundle, the CLAP, the VST3 bundle and the
JACK standalone.

## Cross-compile for Windows:
```bash
cd plugin && make win -j5
```

Builds the CLAP and the VST3 into `plugin/bin-win/`, and checks that neither needs
a DLL that does not ship with Windows.  Needs `g++-mingw-w64-x86-64`; see the
Windows section of [CLONING.md](CLONING.md) for where the results go on the
target machine.

## Check it:
```bash
cd plugin && make selftest      # everything below, in one pass
```

Individually, when one of them is what you are working on:

| Target | What it proves |
|---|---|
| `make selftest` | the whole suite: LV2 host, state, patches, tones, cards, presets, then the two UI checks |
| `make clapcheck` | the DSP->UI channel under CLAP, with a real CLAP host |
| `make vst3check` | the same under VST3, plus that the plugin is built once per instance and leaks no host messages |
| `make presetcheck` | the preset file format, round trip and refusals.  No ROMs, no DPF, no display |
| `make writecheck` | what the firmware's own WRITE does, by pressing buttons in emulated time |
| `make rtaudit` | what the audio callback allocates.  Reports NOT CLEAN on purpose -- see the plugin README |

`clapcheck` and `vst3check` need a display and will start Xvfb if `DISPLAY` is
unset; they skip rather than fail if there is neither.


