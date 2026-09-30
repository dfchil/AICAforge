#!/usr/bin/env python3
"""Selection-policy check without requiring a particular SoundFont fixture."""

import afx_sf2
from pathlib import Path




class Sample:
    def __init__(self, start, end, kind, left, mono=False):
        self.start, self.end, self.sample_type = start, end, kind
        self.is_left, self.is_mono = left, mono
        self.original_pitch = 60
        self.pitch_correction = 0


class Bag:
    def __init__(self, sample, pan=None):
        self.sample = sample
        self.key_range = self.velocity_range = None
        self.base_note = 49
        self.tuning = self.fine_tuning = 0
        self.sample_loop = True
        self.gens = {} if pan is None else {17: type("Generator", (), {"short": pan})()}


class PresetBag:
    velocity_range = None

    def __init__(self, sample):
        self.instrument = type("Instrument", (), {"bags": [Bag(sample)]})()


class Preset:
    bags = []
    def key_bags(self, key):
        assert key == 60
        return [PresetBag(Sample(1, 11, 4, True)), PresetBag(Sample(12, 22, 2, False))]

    def key_samples(self, key):
        assert key == 60
        return [Sample(1, 11, 4, True), Sample(1, 11, 4, True), Sample(12, 22, 2, False)]


class MirroredPreset:
    velocity_range = None
    bags = []

    def key_bags(self, key):
        assert key == 60
        left, right = Sample(1, 11, 4, True, True), Sample(12, 22, 4, True, True)
        first = PresetBag(left); first.instrument.bags = [Bag(left, -500)]
        second = PresetBag(right); second.instrument.bags = [Bag(right, 500)]
        return [first, second]


notes = [{"key": 60, "velocity": 100}]
assert len(afx_sf2.select_regions(notes, Preset(), "left")) == 1
assert len(afx_sf2.select_regions(notes, Preset(), "right")) == 1
assert len(afx_sf2.select_regions(notes, Preset(), "stereo")) == 2
uses = afx_sf2.select_region_uses(notes, Preset(), "left")
assert next(iter(uses.values()))["roots"] == {49}
assert next(iter(uses.values()))["tunes"] == {0}
assert afx_sf2.select_note_uses(notes[0], Preset(), "left")[0]["root_key"] == 49
assert afx_sf2.select_note_uses(notes[0], Preset(), "left")[0]["source_controls"]["pan_centibels"] is None
assert len(afx_sf2.select_note_uses(notes[0], MirroredPreset(), "left")) == 1
assert afx_sf2.select_note_uses(notes[0], MirroredPreset(), "left")[0]["source_controls"]["pan_centibels"] == -500
assert afx_sf2.select_note_uses(notes[0], MirroredPreset(), "right")[0]["source_controls"]["pan_centibels"] == 500
assert len(afx_sf2.select_note_uses(notes[0], MirroredPreset(), "stereo")) == 2
assert afx_sf2.parse_triplet("0:0:42") == (0, 0, 42)
assert afx_sf2.parse_pair("0:0") == (0, 0)
print("afx SoundFont selection checks passed")

from types import SimpleNamespace as NS
def gen(values, **kwargs):
    return NS(gens={key:NS(short=value) for key,value in values.items()}, **kwargs)
pg=gen({34:100,48:20},instrument=None)
ig=gen({34:-8000,48:90},sample=None)
il=gen({34:-6000,48:160},sample=object())
pl=gen({34:200},instrument=NS(bags=[ig,il]))
controls=afx_sf2.source_controls(NS(bags=[pg,pl]),pl,il)
assert controls['attack_timecents']==-5800
assert controls['attenuation_centibels']==180
print('SF2 local-over-global precedence checks passed')

sample=Sample(1,11,4,True,True)
local=Bag(sample);local.sample_loop=None
pb=PresetBag(sample);pb.instrument.bags=[gen({54:1},sample=None),local]
loop_preset=NS(bags=[pb],key_bags=lambda key:[pb])
assert afx_sf2.select_note_uses(notes[0],loop_preset,'left')[0]['loop']
local.gens[54]=NS(short=0)
assert not afx_sf2.select_note_uses(notes[0],loop_preset,'left')[0]['loop']
print('Inherited SF2 sample loop checks passed')

mod=lambda amount:NS(src_oper=1282,dest_oper=48,amount_src_oper=0,trans_oper=0,amount=amount)
pg.mods=[mod(800)];ig.mods=[mod(0)];il.mods=[];pl.mods=[]
assert afx_sf2.modulator_amount(NS(bags=[pg,pl]),pl,il)==800
il.mods=[mod(120)];pl.mods=[mod(300)]
assert afx_sf2.modulator_amount(NS(bags=[pg,pl]),pl,il)==420
print('SoundFont velocity modulator override checks passed')

# Pitch generators follow the same local-over-global, then additive rules.
pitch_global = gen({51:2,52:7},instrument=None)
local = Bag(sample);local.base_note=None;local.gens={52:NS(short=3)}
inst_global=gen({51:1,52:-2,58:48},sample=None)
pb=PresetBag(sample);pb.gens={52:NS(short=-5)};pb.instrument.bags=[inst_global,local]
preset=NS(bags=[pitch_global,pb],key_bags=lambda key:[pb])
use=afx_sf2.select_note_uses(notes[0],preset,'left')[0]
assert use['root_key']==48 and use['tuning_cents']==298
local.gens[58]=NS(short=49);local.gens[51]=NS(short=-1)
use=afx_sf2.select_note_uses(notes[0],preset,'left')[0]
assert use['root_key']==49 and use['tuning_cents']==98
assert 'fine_tune' not in use['source_controls']  # Already lowered into pitch.
print('Inherited SF2 pitch checks passed')

# A preset attack offset is relative to the default -12000 timecents,
# not an absolute multi-second attack (GeneralUser bassoon regression).
inst=gen({},sample=object())
pre=gen({34:3102,8:-1200},instrument=NS(bags=[inst]))
controls=afx_sf2.source_controls(NS(bags=[pre]),pre,inst)
assert controls['attack_timecents']==-8898
assert controls['filter_cutoff_cents']==12300
inst.gens[34]=NS(short=-6000)
assert afx_sf2.source_controls(NS(bags=[pre]),pre,inst)['attack_timecents']==-2898
print('Preset offsets from SF2 instrument defaults checks passed')
