# AGENTS.md

Guidance for coding agents in this repository. Humans: see [README.md](README.md).

## What this is

A fork of [Engine Sim](https://github.com/ange-yaghi/engine-sim) by AngeTheGreat (MIT; upstream is archived and moved to engine-sim-community-edition). The interactive app is Windows-only (delta-studio: Direct3D, Win32, DirectSound). This fork adds three things:

1. **A headless macOS/Linux build** (`-DENGINE_SIM_HEADLESS=ON`): the simulation core and script compiler, with no delta-studio and no Discord.
2. **`engine-sim-cli`**: an offline renderer that holds an engine at fixed rpm on the dyno and writes steady on/off-load WAV takes. It made the engine sounds for the game *Nowhere in Particular* (NiP).
3. **`engine-sim.wasm`**: a [gasm](https://gasm.emdzej.pl) guest. It's step 1 of a web and multi-platform port: simulator plus synthesizer, no UI yet.

Remotes: `origin` is the fork `git@github.com:emdzej/engine-sim.git`. The work lives on branch `macos-headless-renderer` (pushed); `master` is upstream's last commit `85f7c3b`.

## Layout (what this fork added)

| Path | What |
|---|---|
| `tools/engine_sim_cli.cpp` | the offline renderer (`--help`-style usage in `usage()`) |
| `tools/build-headless.sh` | applies `tools/patches/`, configures and builds `build-headless/engine-sim-cli` |
| `tools/patches/*.patch` | portability fixes for the submodules (`piranha`, `simple-2d-constraint-solver`); the submodules point at upstream repos we can't push to |
| `tools/engine-sounds/` | per-kind engine configs, `render.sh` (rpm points and flags per kind), `qa.py` (take checks), `gen_pulse_equivalent.py` (two-stroke configs), [README](tools/engine-sounds/README.md) with the kinds table and lessons |
| `tools/gasm/` | `engine_sim_gasm.cpp` (the guest), `CMakeLists.txt` (gasm C SDK), `build.sh` |
| `CMakeLists.txt`, `dependencies/submodules/CMakeLists.txt` | `ENGINE_SIM_HEADLESS` option; `__forceinline` mapped for non-MSVC |

## Build and test

```sh
brew install cmake boost bison flex      # system bison 2.3 is too old; flex's FlexLexer.h must match flex
tools/build-headless.sh                  # -> build-headless/engine-sim-cli
tools/engine-sounds/render.sh r4 /tmp/out
uv run --no-project --with numpy python -I tools/engine-sounds/qa.py /tmp/out r4

tools/gasm/build.sh ../gasm              # -> build-gasm/engine-sim.wasm (needs a gasm checkout: its C SDK + tools/wasi-sdk)
G=../gasm/runners/native/target/release/gasm-run
$G build-gasm/engine-sim.wasm --asset-dir es=es --asset-dir assets=assets --asset-dir tools=tools \
   --param script=tools/engine-sounds/v6.mr --param mode=bench --param rpm=3000 --headless 300 --no-hash
node ../gasm/runners/web/headless.mjs build-gasm/engine-sim.wasm <same options>   # V8
```

**Before claiming a change works:**
- **Native core changes:** rebuild the CLI and re-render the delivered kinds. Check they're byte-identical to the NAS copies, unless the change is meant to alter sound:
  ```sh
  for k in r4 r6 r6b v6 v8_diesel flat2 single_2t twin_2t; do tools/engine-sounds/render.sh $k /tmp/v
    for f in /Volumes/data/nowhereinparticular/generated/engine-sim/$k/*.wav; do cmp -s $f /tmp/v/$k/${f##*/} || echo "DIFF $f"; done; done
  ```
  NiP's `NOTES.md` states that every delivered take reproduces from commit `5181179`.
- **Guest changes:** rebuild the guest and run bench mode on both runners. Capture audio (`--param capture=<frames> --storage-dir <dir>` on the Node runner writes `capture.wav`), then check it doesn't clip.

## Invariants

- **The native build's output is bit-identical** for the delivered takes. Anything that changes float results only in the wasm build goes behind a macro that the wasm CMake defines (`ATG_ENGINE_SIM_FAST_CONVOLUTION`), never unconditionally.
- **Submodule changes go through `tools/patches/`.** Edit inside the submodule, then refresh the patch: `git -C dependencies/submodules/<name> diff > tools/patches/<name>.patch`. Both build scripts apply the patches idempotently. Never commit the submodule pointer to a commit that only exists locally.
- **Piranha keeps Boost natively.** `PIRANHA_NO_BOOST` (lexical paths, `piranha::setFileReader`) is for the guest only. Throws use `PIRANHA_THROW`, which aborts without exceptions; nothing catches them.
- **Renders are deterministic:** fixed noise seed, single-threaded synthesizer, and the CLI drives the dyno itself. Keep it that way: no wall-clock dependence, no audio thread in the CLI or the guest.
- **Engine configs in `assets/` stay unmodified.** Changed engines are modified copies in `tools/engine-sounds/`, with a header comment saying what they come from and what changed.

## Toolchain traps (solved; don't reintroduce)

- **CMake 4** needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` (googletest's old `cmake_minimum_required`).
- **Boost 1.85+** removed `path::is_complete` (now `is_absolute`, in the piranha patch). `malloc.h` doesn't exist on macOS, and `__int64` is MSVC-only (`uint64_t`/`uintptr_t`).
- **`extern constexpr` in headers** defines a symbol per translation unit, which ld64 rejects. `constants.h` and `units.h` use `inline constexpr`.
- **clang, unlike MSVC,** rejects:
  - default arguments `= {}` of a nested struct with member initializers (`GasSystemMix` now lives outside `GasSystem`);
  - `start` for `m_start` in `RingBuffer::overwrite` (a real bug);
  - backslash includes;
  - template parameters shadowed by member templates;
  - members of incomplete types in `sparse_matrix.h`.
- **wasi-libc++** doesn't pull in `<cstdlib>`/`<cmath>` transitively (`rand`, `std::abs`, `malloc`).
- **The shell is zsh:** an unquoted `$VAR` holding several options is *not* word-split (use `${=VAR}` or arrays). macOS `bash` is 3.2: `"${arr[@]}"` of an empty array fails under `set -u`, so use `${arr[@]+"${arr[@]}"}`.
- **The CLI doesn't create `--out`** (`render.sh` does).

## Engine Sim facts that cost time

- **Spin direction.** Starting an engine from rest on the dyno picks the direction from `v_theta = 0` and can run it backwards: valve and ignition timing reversed, no combustion. The CLI and the guest crank with the starter first. The B18C5 config spins the other way (negative `starter_speed`, negative timing), so its crank step is skipped; that's fine.
- **Camshaft units.** `add_lobe` takes **crank** degrees (`setLobeCenterline` halves them); `harmonic_cam_lobe`'s duration is in crank degrees. With the spark at cycle angle 0, the BDCs are at 180 and 540.
- **Ported two-strokes don't work.** The single-zone cylinder can't scavenge without crankcase pumping. A two-stroke spark option and a pressurised intake were tried, made negative torque, and were removed. Use pulse-equivalent four-strokes (`gen_pulse_equivalent.py`).
- **Sound path:** per-cylinder exhaust-port pressure, delayed by and scaled with 1/length² of the primary plus system length. Then the synthesizer: a 1.9 kHz input low-pass, a DC filter, `hf_gain` (derivative mix), `noise`, `jitter`, impulse-response convolution (≤10 000 samples, trimmed below |100|), and a leveler.
  - The leveler is automatic gain control. The CLI pins it at unity and normalises per engine.
  - In the guest its target is 12 000; at the app's 30 000 it clips.
  - There's no muffler model, and exhaust volume and outlet flow barely change the timbre.
  - `jitter` drives the high-frequency hash at high rpm.
- **The convolution is a direct dot product.** It dominated the wasm build (v6: 0.3× real time) until the fast path.
- **Diesel coasting:** `--fuel-cut` sets the ignition off with the throttle open.

## gasm port

**Status:** step 1 is done (`ed2a04b`).
- **Guest:** `engine-sim.wasm` (4 MB) reads scripts and impulse responses from assets. Mount `es/`, `assets/` and `tools/` with `--asset-dir name=name`; Piranha's relative imports normalise to those names.
- **Each frame:** it simulates 1/60 s, runs the synthesizer synchronously and pushes mono f32 audio.
- **Display:** an rpm/throttle bar.
- **Modes:** `mode=play` (pad UP = throttle, A = starter) and `mode=bench rpm=<n>` (dyno hold at wide-open throttle).

Measured on Apple silicon, scalar wasm, engine at its full simulation rate, bench mode:

| engine | V8 (Node runner) | wasmtime (`gasm-run`) |
|---|---|---|
| r4 (20 kHz) | 1.5× real time | 1.2× |
| v6 (10 kHz, 10 000-tap IR) | 1.2× | 0.9× |
| r6 | 1.9× | 1.0× |
| twin_2t | 1.6× | 1.0× |

A `-msimd128` build (`-DENGINE_SIM_GASM_SIMD=ON`) is about 30% faster on wasmtime. **gasm's ABI forbids SIMD, threads and exception handling** (`spec/ABI.md`), so it's off by default; allowing it is the gasm owner's decision. Headless gasm runs use virtual time, so time the whole run from outside (`--no-hash`; the runners print frames/s). The guest hasn't been tried in a real browser page yet, only through the Node runner (the same V8).

**gasm constraints that shaped the guest:**
- There's no filesystem (WASI has no preopens), so all files come through `gasm_asset_*`.
- C++ builds with `-fno-exceptions`.
- Single thread: `std::thread` links on `wasm32-wasip1` but must never be started.
- Storage (`gasm_storage_set`) is the only way to get data out.

**Next (step 2, not started):** the UI.
- Port delta-studio's OpenGL 4 backend (it has GLSL shaders next to the HLSL ones) to GLES 3 / WebGL 2 on `gasm:gl` (`<GLES3/gl3.h>` + `gasm_gl.c` from the C SDK).
- Replace Win32 windowing and input, and DirectSound, with gasm's (or SDL 3 for gasm).
- Package the scripts and impulse responses as assets.
- Then step 3: engine picking, and web and native packaging through gasm's runners.
- Watch real-time headroom for 20 kHz and V10/V12 engines.

Don't change the gasm repository from here; it's the user's separate project with its own AGENTS.md. Read that file before building there.

## Working with the NiP sessions

- **nip-main** (another Claude session) coordinates the game. It takes the takes from the NAS, cuts and levels them, and wires them into `sounds.json`. Requests arrive as cross-session messages; reply with `SendMessage` to `nip-main`.
- **Never edit the game repo** (`/Users/mjaskols/Projects/my/nowhereinparticular`). Don't run `make test`, exports or region builds there without asking nip-main first.
- **Deliveries:**
  - Takes go to `/Volumes/data/nowhereinparticular/generated/engine-sim/<kind>/`, with one shared `NOTES.md` one level up. It covers the source commit, licence, per-engine config and changes, rpm/load method, and gains. Keep it current with every delivery.
  - Re-render a kind into its own folder; variants get their own kind (`r6b`).
  - Verify with `qa.py` before saying they're delivered.
- **Status (2026-10-24):** all kinds are delivered (r4, r6/r6b, v6, v8_diesel, flat2, single_2t, twin_2t).
  - The user hears Engine Sim's bare exhaust as unmuffled. nip-main plans to add a muffler/exterior impulse response in post, and `--dry` takes are on offer.
  - v6 and r4 have deliberate per-cylinder unevenness (`qa.py` WARN). An equal-length v6 variant is offered if the user dislikes it.

## Conventions

- Commit messages: a summary line, a body with the why, and the `Co-Authored-By` trailer. Branch off `master`; don't commit to it.
- Comments explain non-obvious Engine Sim behaviour at the point of use (see the CLI and the configs' header comments). Match the surrounding style (4-space C++, `m_` members, `/* void */` empty bodies).
