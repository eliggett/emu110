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


