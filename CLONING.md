# Cloning emu110

This repository is three source trees, not one:

| Tree | What it is | How it is tracked |
| --- | --- | --- |
| `emu110` itself | the emulator work, the analysis, the artwork, and the Voltaire 110 plugin | this repository |
| `mame/` | MAME, on a branch carrying the U-110 driver work | **submodule** → `github.com/eliggett/mame`, branch `u110` |
| `plugin/dpf/` | DISTRHO Plugin Framework, unmodified upstream | **submodule** → `github.com/DISTRHO/DPF`, branch `main` |

Neither submodule is vendored — no copies of MAME or DPF sources live in this
repository's history. That is deliberate, and it is what the rest of this
document exists to support:

- **MAME** is compiled from its own sources, unpatched, against the shim in
  `plugin/compat/`. `plugin/tools/build_core.sh` names the exact `.cpp` files it
  builds out of `mame/src/`. If one of them ever needed editing to compile, the
  shim would be wrong. Vendoring would quietly permit that edit.
- **DPF** is upstream plus two patches, applied at build time and kept in
  `plugin/patches/`. Forking DPF would work, but then "what did we change?" would
  be a `git log` archaeology exercise instead of two readable diffs.

The licence boundary rests on this too: MAME's BSD-3-Clause devices are compiled
but the resulting archive is what links; `plugin/src` and `plugin/tools` are
GPL-3.0-or-later. See `LICENSE`.

---

## 1. Prerequisites

On Ubuntu 24.04 (what this is developed on):

```sh
sudo apt install build-essential git python3 pkg-config \
                 lv2-dev libjack-jackd2-dev \
                 libgl1-mesa-dev libx11-dev libxext-dev libxcursor-dev \
                 libcairo2-dev libdbus-1-dev \
                 inkscape librsvg2-bin
```

What each is for:

- `build-essential` — g++ with C++20. The core and the plugin both need it.
- `lv2-dev` — headers for the LV2 build and for `plugin/tools/lv2_selftest.c`.
- `libjack-jackd2-dev` — the standalone (`bin/Voltaire110`) speaks JACK. Under
  PipeWire this Just Works; you do not need to run `jackd` yourself.
- the X11 / GL / cairo / dbus set — DPF's DGL, which draws the panel.
- **`inkscape` and `librsvg2-bin` (`rsvg-convert`) are regeneration tools only.**
  The artwork exporter uses Inkscape to flatten text to paths and rsvg-convert to
  pre-render the two constructs nanosvg cannot draw. Nothing in `generated/` is
  checked in, so a fresh clone *does* need them on the first build. See
  `plugin/README.md` for why.

Disk: `mame/` is about **2 GB** checked out, plus ~220 MB of history.

---

## 2. The first clone

One command, and the `--recurse-submodules` is not optional:

```sh
git clone --recurse-submodules https://github.com/eliggett/emu110.git
cd emu110
```

**Recursive, not just `--recurse`-the-top-level.** `plugin/dpf` has a submodule of
its own — `dgl/src/pugl-upstream` — and DGL will not build without it. A
`--recurse-submodules` clone gets all three levels; `git submodule update --init`
without `--recursive` gets two and then fails to link.

### If you already cloned without it

```sh
git submodule update --init --recursive
```

Same result. This is also the command to run after any pull that moves a
submodule pointer (see §6).

### Verifying you got everything

```sh
git submodule status --recursive
```

Every line should start with a **space**. A leading `-` means the submodule was
never initialised (empty directory); a leading `+` means it is checked out at a
commit other than the one this repository records.

```
 51af0485f9c0d27a51883ec7582ddff6cf3150f6 mame (heads/u110)
 4238e1c7f0351bbe488d79f0899c540543ac7583 plugin/dpf (heads/main)
 5e2621d714ddf1cb0f86e852f8ba5dffe04aa3a3 plugin/dpf/dgl/src/pugl-upstream (5e2621d)
```

### What you do *not* have to fetch

The wave ROMs and program ROMs are tracked in this repository, under `roms/`.
There is no separate download step. The plugin finds them by searching
`$U110_DATA_DIR/roms`, then `$XDG_DATA_HOME/u110/roms`, then
`~/.local/share/u110/roms`, then `/usr/share/u110/roms` — so for a development
build, `U110_DATA_DIR` pointing at the repository root is all it takes, and the
Makefile's test targets set that for you.

---

## 3. Patching DPF

**You do not run anything. `make` applies the patches.**

`plugin/Makefile` has a `patches` target, and `all` runs it first — before
`generated`, before `core`, before any of the plugin targets:

```make
all:
	@$(MAKE) --no-print-directory patches generated core
	@$(MAKE) --no-print-directory $(TARGETS)
	@$(MAKE) --no-print-directory ttl
```

The target walks `plugin/patches/*.patch` in sorted order and, for each one,
asks git whether it is already applied before applying it:

```make
patches:
	@for p in patches/*.patch; do \
		if git -C dpf apply --check --reverse "$(CURDIR)/$$p" 2>/dev/null; then \
			:; \
		elif git -C dpf apply "$(CURDIR)/$$p" 2>/dev/null; then \
			echo "  applied $$(basename $$p) to dpf"; \
		else \
			echo "  WARNING: $$(basename $$p) does not apply to dpf -- check the submodule"; \
		fi; \
	done
```

`git apply --check --reverse` succeeding means the patch is *already in*, so the
loop does nothing. That is what makes it safe to run on every build. The three
outcomes are distinguishable on purpose:

| Output | Meaning |
| --- | --- |
| (silence) | already applied — the normal case on a rebuild |
| `applied 0001-... to dpf` | first build after a clone, or after a submodule reset |
| `WARNING: 0001-... does not apply to dpf` | the DPF pin moved out from under the patch — §6 |

That warning is **not** fatal to the build. The build will succeed and the
standalone's panel will simply stay blank, which is a much more confusing symptom
than a failed compile — so if you see the warning, stop and fix it.

### The two patches

- **`0001-jack-dsp-to-ui-state.patch`** — the one that matters. DPF implements
  `updateStateValue()`, the DSP→UI channel, for LV2, CLAP, AU and Carla, but not
  for the JACK standalone: `PluginJack`'s constructor passes `nullptr` where the
  callback belongs. Without this patch the standalone builds and runs and makes
  sound, and its panel never updates. Since the standalone is the daily
  development loop, that is not a small gap.
- **`0002-silence-updateStateValueCallback-debug.patch`** — removes a
  `d_stdout()` left in `DistrhoPluginInternal.hpp`. Cosmetic; it fires on every
  state update, which is often.

### Why `plugin/dpf` always shows as modified

```
$ git status --short
 m plugin/dpf
```

**This is correct and expected.** The patches are applied to the submodule's
working tree, so the submodule is permanently dirty. The lower-case `m` means
"the submodule's *contents* differ", not "the submodule points at a different
commit" (that would be `M`, and would be a real change worth committing).

Do not commit a new pointer to try to make it go away — that would mean pushing
the patched tree somewhere, which is exactly the fork we are avoiding.

### Getting back to clean DPF

```sh
git -C plugin/dpf checkout -- .
```

The next `make` re-applies both patches. Useful when a patch has been half
applied, or when you want to confirm a build problem is not ours.

### Adding a patch

```sh
cd plugin/dpf
# ... edit ...
git diff > ../patches/0003-short-description.patch
git checkout -- .          # prove it re-applies from clean
cd .. && make
```

Number the file so sort order equals apply order, and keep each patch to one
idea. Commit the `.patch` file — it is the only record of the change.

---

## 4. Building

```sh
cd plugin
make -j5
```

That is the whole build. In order, it:

1. applies the DPF patches (§3);
2. regenerates `plugin/generated/` from the Inkscape artwork and the LCD font —
   this is the step that needs Inkscape and rsvg-convert;
3. builds `plugin/build/libu110core.a` from MAME's sources via
   `tools/build_core.sh`, which also runs MAME's own `mcs96make.py` opcode
   generator rather than checking generated files in;
4. links the four targets — LV2, VST3, CLAP, and the JACK standalone;
5. generates the LV2 manifest, which has to happen *after* the UI is linked or
   hosts report the plugin has no custom UI.

Products land in `plugin/bin/`. See `BUILDING.md` for the artwork-only commands.

### Check it works

```sh
cd plugin
make selftest      # drives the LV2 build through a minimal host
make rtaudit       # proves run() reaches no malloc, free, or getenv
./bin/Voltaire110  # the standalone
```

`make selftest` should report four passes per suite; `make rtaudit` should report
matched malloc/free counts. Both build first, so they are also a fine way to say
"build and tell me it is not broken".

---

## 5. The one thing a clone cannot fix for you

`git clone --recurse-submodules` can only fetch commits that have been **pushed**.
If `mame` or `plugin/dpf` is pinned to a commit that exists only on the machine
where the pin was made, the clone fails with:

```
fatal: remote error: upload-pack: not our ref <sha>
Errors during submodule fetch/checkout
```

Before publishing this repository, or before asking anyone to clone it, check
that both pins are reachable from the remotes:

```sh
git -C mame ls-remote origin | grep "$(git rev-parse :mame)"
git -C plugin/dpf ls-remote origin | grep "$(git rev-parse :plugin/dpf)"
```

Each should print a line. No output means that submodule's pinned commit has not
been pushed, and nobody else can build this checkout.

---

## 6. Keeping up to date

Pulling changes that move a submodule pointer:

```sh
git pull
git submodule update --init --recursive
```

Without the second line, git leaves the submodule at the old commit and
`git status` shows it as modified. To have git do it automatically:

```sh
git config submodule.recurse true
```

### Moving a pin deliberately

```sh
cd mame                       # or plugin/dpf
git fetch origin
git checkout <commit-or-branch>
cd ..
git add mame
git commit -m "mame: move to <what changed>"
```

For `plugin/dpf`, moving the pin is the moment the patches can break. Do it
deliberately:

```sh
git -C plugin/dpf checkout -- .        # drop our patches
cd plugin/dpf && git fetch origin && git checkout <new-commit> && cd ..
cd plugin && make patches              # re-apply against the new tree
```

If a patch no longer applies, regenerate it by hand against the new DPF rather
than forcing it — the surrounding code has moved, and a fuzzy apply is how you
get a build that compiles and misbehaves.

---

## 7. Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| `fatal: no submodule mapping found in .gitmodules for path 'mame'` | a gitlink was committed without a `.gitmodules` entry | the entry is present as of this document; re-pull |
| `plugin/dpf` is an empty directory | cloned without `--recurse-submodules` | `git submodule update --init --recursive` |
| DGL fails to compile, missing pugl headers | recursed one level, not all the way down | `git submodule update --init --recursive` |
| `upload-pack: not our ref` during clone | a submodule pin was never pushed | §5 — the fix belongs to whoever made the pin |
| `WARNING: 0001-... does not apply to dpf` | the DPF pin moved | §6 |
| standalone runs, panel never updates | patch `0001` is not in | check for the warning above; `make patches` |
| build stops in `panel_export.py` | Inkscape or rsvg-convert missing | §1 |
| `FileNotFoundError` in `render_raster` under `-j` | fixed in `50c6412` (grouped target `&:`) | pull |
| plugin loads but reports no ROMs | ROM search path | run from the repository, or set `U110_DATA_DIR` to it |

---

## See also

- `BUILDING.md` — the short version, and the artwork-only commands.
- `plugin/README.md` — the artwork pipeline, the Inkscape labelling conventions,
  and the design notes behind the build's ordering rules.
- `RUNNING.md` — running the MAME build of the emulator, which is a separate
  thing from the plugin.
- `AGENTS.md` — repository conventions.
