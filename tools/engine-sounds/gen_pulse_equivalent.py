#!/usr/bin/env python3
"""Generates the pulse-equivalent configs for the two-stroke kinds.

Engine Sim only simulates four-strokes, and a valve-ported two-stroke does not
work in it: the single-zone cylinder has no crankcase pumping, so it never
scavenges (tried: negative torque at every port timing and charge pressure).
What a listener hears from a two-stroke is first of all its firing rate, one
combustion pulse per cylinder per revolution. A four-stroke with twice the
cylinders, phased to fire at the same crank angles, gives the same pulse train
with working combustion. Both are derived from the Hayabusa config, which
breathes and burns well at high rpm in Engine Sim:

  twin_2t:   the Hayabusa I4 (flat crank, a pulse every 180 degrees = two per
             revolution, like a 180-degree twin), with 391 cc cylinders
  single_2t: the Hayabusa with cylinders 2 and 3 removed: cylinders 1 and 4
             share a crank angle and fire 360 degrees apart (a 360-degree
             parallel twin = one pulse per revolution, like a single), with
             185 cc cylinders

Both get one exhaust volume for all cylinders and equal sound attenuation (the
Hayabusa file sends cylinders 1/3 and 2/4 to systems at different volumes, which
with its 1-2-4-3 firing order modulates the sound), and r4's impulse response
and synthesizer settings.

Usage: tools/engine-sounds/gen_pulse_equivalent.py (writes single_2t.mr, twin_2t.mr)
"""
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SOURCE = 'assets/engines/atg-video-1/04_hayabusa.mr'


def sub(text, old, new, count=1):
    assert text.count(old) >= count, old
    return text.replace(old, new) if count > 1 else text.replace(old, new, 1)


def common(src, name, bore, stroke, chamber, redline, rev_limit, flywheel_lb, inertia):
    src = sub(src, 'name: "Suzuki Hayabusa I4",', f'name: "{name}",')
    src = sub(src, 'redline: 11000 * units.rpm,', f'redline: {redline} * units.rpm,')
    src = sub(src, '''        hf_gain: 0.00407,
        noise: 0.292,
        jitter: 0.062,''', '''        hf_gain: 0.002,
        noise: 0.253,
        jitter: 0.195,''')
    src = sub(src, 'label stroke(65 * units.mm)', f'label stroke({stroke} * units.mm)')
    src = sub(src, 'label bore(81 * units.mm)', f'label bore({bore} * units.mm)')
    src = re.sub(r'flywheel_mass: [0-9.]+ \* units\.lb,', f'flywheel_mass: {flywheel_lb} * units.lb,', src, count=1)
    src = re.sub(r'moment_of_inertia: 0\.22986844776863666 \* [0-9.]+,',
                 f'moment_of_inertia: 0.22986844776863666 * {inertia},', src, count=1)
    src, n = re.subn(
        r'    exhaust_system exhaust0\([\s\S]*?\)\s*exhaust_system exhaust1\([\s\S]*?\n    \)',
        '''    exhaust_system exhaust0(
        es_params,
        audio_volume: 0.5,
        impulse_response: ir_lib.mild_exhaust_0
    )''', src, count=1)
    assert n == 1
    src = src.replace('exhaust_system: exhaust1,', 'exhaust_system: exhaust0,')
    src = re.sub(r'sound_attenuation: [0-9.]+', 'sound_attenuation: 1.0', src)
    src = sub(src, '''        hayabusa_head(
            intake_camshaft''', f'''        hayabusa_head(
            chamber_volume: {chamber} * units.cc,
            intake_camshaft''')
    src = sub(src, 'rev_limit: 11500 * units.rpm,', f'rev_limit: {rev_limit} * units.rpm,')
    return src


def drop_cylinder(src, wire):
    """Removes the cylinder on `wire` from the bank and its ignition post."""
    pattern = re.compile(
        r'\n        \.add_cylinder\(\n(?:(?!\.add_cylinder)[\s\S])*?ignition_wire: wires\.' + wire
        + r',\n(?:[^\n]*\n)*?        \)')
    src, n = pattern.subn('', src, count=1)
    assert n == 1, wire
    src, n = re.subn(r'\n\s*\.connect_wire\(wires\.' + wire + r', [^\n]*\)', '', src, count=1)
    assert n == 1, wire
    return src


def twin_2t(src):
    src = common(src, 'Jet ski 2T twin, pulse-equivalent (from Hayabusa)',
                 bore=82, stroke=74, chamber=39, redline=7200, rev_limit=7600,
                 flywheel_lb=20, inertia=0.6)
    return src


def single_2t(src):
    src = common(src, 'Paramotor 2T single, pulse-equivalent (from Hayabusa)',
                 bore=66, stroke=54, chamber=16, redline=8500, rev_limit=9000,
                 flywheel_lb=6, inertia=0.15)
    src = drop_cylinder(src, 'wire2')
    src = drop_cylinder(src, 'wire3')
    # The camshaft lists one lobe per cylinder in bank order (cylinders 1-4 at
    # 0, 1/4, 3/4 and 2/4 of the cycle); keep cylinder 1's and cylinder 4's
    for cam in ('exhaust', 'intake'):
        sign = '-' if cam == 'exhaust' else '+'
        lobe = f'        .add_lobe(rot360 {sign} {cam}_lobe_center + ({{}} / 4) * cycle)\n'
        src = sub(src, lobe.format('1.0') + lobe.format('3.0'), '')
    return src


def main():
    base = (ROOT / SOURCE).read_text()
    for kind, transform, title in (
            ('twin_2t', twin_2t, 'jet ski two-stroke twin (2 x 391 cc, 82 x 74 mm)'),
            ('single_2t', single_2t, 'paramotor single-cylinder two-stroke (185 cc, 66 x 54 mm)')):
        src = transform(base)
        src = src.replace('import "engine_sim.mr"',
                          f'// {kind}: {title}, approximated.\n'
                          f'// Generated from {SOURCE} by gen_pulse_equivalent.py (see there for why).\n\n'
                          'import "engine_sim.mr"', 1)
        src = src.rstrip() + '\n\nmain()\n'
        (HERE / f'{kind}.mr').write_text(src)
        print('wrote', HERE / f'{kind}.mr')


if __name__ == '__main__':
    main()
