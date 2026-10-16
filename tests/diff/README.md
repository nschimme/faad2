# Differential harness

Configure `FAAD_BUILD_TESTS=ON` and `FAAD2_ORACLE_ROOT=/path/to/stock-2.11.4`,
then build and run CTest. The optional `shim_differential` test fails on
unregistered differences or LC SNR below 35 dB. `shim_test` needs no oracle.
The oracle is linked only into `diff_oracle`, using its own header.

```
python3 tests/diff/compare.py build-codex/diff_shim build-codex/diff_oracle tests/data
build-codex/diff_shim tests/data/stereo44_lc.aac 0 4 build-codex/output.pcm
```

Runner arguments: fixture, RAW mode (0/1), output format (1/4), PCM destination.
JSONL output contains ASC (RAW), Init, every frame's metadata, and IEEE CRC32 of
concatenated native-endian PCM. CRC inequality never fails. SNR uses the complete
stock PCM as reference, without alignment or gain adjustment. LC at matching
rates requires >=35 dB: on stereo44_lc FAAD2, libfaad3 and FFmpeg agree pairwise only to about 38 dB. HE SNR is diagnostic. PS compares metadata only.
Every .aac fixture runs ADTS and RAW+ASC, both formats; length-prefixed .dab
fixtures run Init2. Other extensions are ignored. Fixtures are trusted inputs.

Policies are pre-registered in compare.py. Low-rate LC permits exact factor-two
rate/sample-count changes and NO_SBR_UPSAMPLED versus NO_SBR. Implicit-SBR Init2
permits only exact factor-two rate differences on an SBR stream. Explicit HE ASC
2/5 differences require an AOT 5 wrapper. 32-bit positive-full-scale clipping is
registered but unexercised: required PCM formats here are 16-bit and float.
No fixture-specific exceptions are used.

`generate.py` recreates the four new synthetic LC fixtures using the named FAAC
encoder, explicitly forcing LC. Both -a and -r are run; raw output is verified
against stripped ADTS packets. Redundant raw data is not checked in. Each new
fixture is below 20 KB; existing fixtures are retained verbatim.

`layout_probe.c` compiled against stock 2.11.4 produced layout64.inc on LP64.
layout32.inc is derived by compiling the same stock header with unsigned long
replaced by uint32_t. All structure members are char, ushort, ulong or char
arrays, with ILP32 alignments 1/2/4. This is a field-type model, not native 32-bit
execution. The tests guard those constants by pointer and unsigned-long size.
