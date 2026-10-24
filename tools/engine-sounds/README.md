# Engine sound takes

Configs and scripts that render steady engine takes with `engine-sim-cli` (see
the top-level README to build it). They were made for the game *Nowhere in
Particular*. The takes, and a `NOTES.md` with licences, gains and per-engine
details, are on the NAS at `/Volumes/data/nowhereinparticular/generated/engine-sim/<kind>/`.

```sh
tools/engine-sounds/render.sh <kind> <out-root>          # writes <out-root>/<kind>/
uv run --no-project --with numpy python -I tools/engine-sounds/qa.py <out-root> <kind>...
```

`render.sh` holds each kind's rpm points and flags. Run it from anywhere; it
changes into the repository root, because the script compiler finds `es/` there.

## What a take is

- `<kind>_<rpm>_<on|off>.wav`: mono, 16-bit, 44.1 kHz, 3.00 s. All of one engine's takes share one gain, set so its loudest take peaks at −1 dBFS.
- `<kind>_takes.csv`: measured mean/min/max rpm, mean dyno torque, peak and RMS dBFS.
- **on**: wide-open throttle, the dyno absorbing torque at the held rpm.
- **off**: closed throttle, the dyno motoring the engine at the same rpm. With `--fuel-cut` it's the throttle open and the ignition off instead, as a diesel coasts.
- The renderer holds the crank on Engine Sim's dyno, cranks with the starter first, and runs the sweep as one continuous simulation. Before each take the dyno ramps for 0.5 s, then it settles for 1.5 s.
- Output is deterministic: the same commit renders byte-identical files.

## Kinds

| kind | vehicle | config | source | rpm points |
|---|---|---|---|---|
| `r4` | city car | `r4.mr` | Honda B18C5, `assets/engines/atg-video-1/05_honda_vtec.mr`, unmodified | 750 … 6500 |
| `r6`, `r6b` | sedan, wagon | `r6.mr`, `r6b.mr` → `m52b28_fixed.mr` | BMW M52B28, `assets/engines/bmw/M52B28.mr`, modified copy | 700 … 6500 |
| `v6` | Explorer, minivan | `v6.mr` | Generic 60° V6, `assets/engines/atg-video-2/04_60_degree_v6.mr`, unmodified | 750 … 5100 |
| `v8_diesel` | pickup | `v8_diesel.mr` | Chevy 454 truck, `assets/engines/chevrolet/chev_truck_454.mr`, modified copy | 650 … 3200 (`--fuel-cut`) |
| `flat2` | dirt bike | `flat2.mr` | new: R80 G/S-sized boxer twin (EJ25 boxer geometry, TRX520 parts) | 1000 … 7000 |
| `single_2t` | paramotor | `single_2t.mr` (generated) | Hayabusa, cylinders 2 and 3 removed: 360° parallel twin | 2000 … 8000 |
| `twin_2t` | jet ski | `twin_2t.mr` (generated) | Hayabusa I4 with 391 cc cylinders | 1200 … 6800 |

- **r6 / r6b**: the M52B28 file routes cylinders 1/3/5 and 2/4/6 to exhausts at different volumes. The copy uses front/rear manifolds at equal volume, plus r4's impulse response and synthesizer settings. r6b also cuts jitter and noise, which makes high rpm sound like an "electric buzz"; prefer r6.
- **v8_diesel**: ≈16:1 compression, 3300 rpm redline, heavier flywheel, one exhaust, equal per-cylinder blowby, attenuation and primaries. It's still a spark-ignition model: no injection clatter or turbo.
- **single_2t, twin_2t** (*pulse-equivalent*): Engine Sim can't run a ported two-stroke (see `gen_pulse_equivalent.py`). These are four-strokes with twice the cylinders, each sized like the two-stroke's and fired at its crank angles, so the pulse train matches. Regenerate them with `python3 tools/engine-sounds/gen_pulse_equivalent.py`; don't edit the `.mr` files by hand.

## Lessons (read before adding an engine)

- **Check the config's per-cylinder sound parameters.** Look at `audio_volume` per exhaust system, `sound_attenuation`, `primary_length` and `blowby`. The stock configs vary them on purpose. Mild variation is character (r4 has it, and it was accepted). A split that follows the firing order (M52: three loud pulses, then three quiet) or large blowby spreads (454) are heard as a limp. `qa.py` shows it as `limp` above 0 dB.
- **Configs without `hf_gain` / `noise` / `jitter` run on Engine Sim's defaults** (0.01 / 1.0 / 0.5), which sound bright and metallic. r4's 0.002 / 0.253 / 0.195 with `ir_lib.mild_exhaust_0` (the most damped impulse response) is the baseline the user liked.
- **Engine Sim has no muffler.** The sound is exhaust-port pressure, convolved with an impulse-response file of up to 227 ms. Exhaust volume, outlet flow and velocity decay change the gas dynamics, not the timbre. A muffler means a different impulse response, or post-processing (`--dry` renders without Engine Sim's IR).
- **Spin direction and units:** `add_lobe` takes crank degrees (the camshaft halves them). Cam lobes are placed relative to the firing angle. A negative `starter_speed` means the engine spins the other way (B18C5), with negative timing.
- **Natural idle** (`--probe`) depends on `idle_throttle_plate_position`. Takes are held on the dyno, so an engine that stalls on a closed throttle still renders.
- **Exhaust wave tuning** makes loudness non-monotonic in rpm; the v8_diesel is ≈15 dB quieter at 3200 than at 2550. That's expected.
