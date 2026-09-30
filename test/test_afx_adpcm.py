"""Checks for the host YA2BEAM encoder selection."""

import os
import struct

import afx_adpcm


saved_cc = os.environ.get("CC")
saved_host_cc = os.environ.get("AICAFLOW_HOST_CC")
try:
    os.environ["CC"] = "kos-cc"
    os.environ.pop("AICAFLOW_HOST_CC", None)
    assert afx_adpcm._host_compiler() == "cc"
    os.environ["AICAFLOW_HOST_CC"] = "clang"
    assert afx_adpcm._host_compiler() == "clang"
finally:
    if saved_cc is None:
        os.environ.pop("CC", None)
    else:
        os.environ["CC"] = saved_cc
    if saved_host_cc is None:
        os.environ.pop("AICAFLOW_HOST_CC", None)
    else:
        os.environ["AICAFLOW_HOST_CC"] = saved_host_cc

raw = struct.pack("<64h", *(index * 123 - 4096 for index in range(64)))
afx_adpcm._pcm16_to_adpcm_cached.cache_clear()
assert afx_adpcm.pcm16_to_adpcm(raw) == afx_adpcm.pcm16_to_adpcm(raw)
assert afx_adpcm._pcm16_to_adpcm_cached.cache_info().hits == 1

print("AICA ADPCM host-compiler checks passed")
