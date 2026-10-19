# `.xa` music and speech streams — LiT + TB

Despite the extension these are **not** PS1 CD-XA (no sector headers, no 128-byte sound groups). They are
headerless **4-bit IMA/DVI ADPCM** streams decoded by the PC ports' own track player.

Evidence keys
- `[TR]` BugsDecomp `BugsDecomp/track.c` (`CvtStereoAdpcm` = `bugs.exe` 0x401be0, `InitTrack` 0x401000,
  `ConvertTrackAudio` 0x401af0); tables read from `bugs.exe` v1.6: step table `0x45c0c0` (89 × s32),
  index-adjust table `0x45c080` (16 × s32).
- `[WF]` WAVEFORMATEX constants in the exes. LiT: call `FUN_00401000(&DAT_00553320, …, &DAT_0045c960 /*in*/,
  &DAT_0045c978 /*out*/, …)`. TB `bblit2.exe`: music `FUN_0040d050(…, &DAT_00481cb8, &DAT_00481cd8, …)`,
  speech `FUN_00429fa0 → FUN_0040d050(…, &DAT_00481c98, &DAT_00481cd8, …)`.
- `[DATA]` `tools/re/xa2wav.py` on LiT `TRACK10.xa`, `speeches/speech00.xa`, TB `TRACK/track1.xa`,
  `SPEECH/00/Speech0.xa`.

## Stream parameters — Verified

| file set | channels | rate | bytes/s | layout | source |
|----------|----------|------|---------|--------|--------|
| LiT `Datas/track/*.xa` (music) | 2 | 44100 | 44100 | 1 byte = 1 stereo frame: low nibble = left, high nibble = right | [WF] `0x45c960` = {tag 2, ch 2, 44100, 44100, align 1, bits 4}; [TR] |
| LiT `Datas/speeches/*.xa` | 2 | 44100 | 44100 | same as music (same player, same format) | [WF]; [DATA]: stereo decode clean, mono decode clips 2.5 % |
| TB `Datas/TRACK/*.xa` (music) | 2 | 44100 | 44100 | as LiT | [WF] `0x481cb8` = {2, 2, 44100, 44100, 0, 0} |
| TB `Datas/SPEECH/NN/*.xa` | 1 | 44100 | 22050 | 2 samples per byte, **low nibble first** | [WF] `0x481c98` = {2, 1, 44100, 22050, 0, 0}; nibble order: [DATA] |

Output format for all: 16-bit stereo 44100 (`0x45c978` / `0x481cd8` = {PCM, 2 ch, 44100, 176400, 4, 16}).
The `nBlockAlign`/`wBitsPerSample` fields of TB's input formats are 0 in the file (filled at runtime or unused).
TB speech nibble order (low first) is from data only: high-first decode drifts (75 % of energy < 50 Hz, mean
+1666) while low-first gives a centred signal with 45 % of energy in 300–3400 Hz. LiT's mono converter is a
stub in BugsDecomp, so this is **Verified (data)**, not code-verified.

## Decoder — Verified (code + tables)

No header, no blocks, no resync: one predictor state per channel, initialised to `sample = 0, index = 0`
at stream start (`[TR]`: `track->adpcmState` zeroed; also zero when `track == NULL`). Per nibble `n`:

```
step   = STEP[index];                       // standard IMA table (7 … 32767), 89 entries
index  = clamp(index + ADJ[n], 0, 88);      // ADJ = {-1,-1,-1,-1,2,4,6,8} repeated
delta  = step >> 3;
if (n & 4) delta += step;
if (n & 2) delta += step >> 1;
if (n & 1) delta += step >> 2;
sample = clamp(sample + ((n & 8) ? -delta : delta), -32768, 32767);
```

Tables in `tools/re/xa2wav.py` are asserted equal to the exe bytes. Note the index is updated *before* the
delta uses the old `step` (same as standard IMA).

Validation: 12 s excerpts of LiT `TRACK10.xa` and TB `track1.xa` decode without clipping (rms 2666 / 4487);
spectrograms show rhythmic, band-limited musical structure. TB `Speech0.xa` mono decodes without clipping.

Looping/positioning: the player seeks by byte (`frames * nBlockAlign`) [TR]; since there are no keyframes,
seeking to an arbitrary point restarts with state 0 (the game only seeks to 0 / streams linearly — Inferred).

## Sound effects and in-level speech (not `.xa`)

Short SFX live in BZE section 3 and per-language speech clips in sections 5–10 as standard big-endian
**AIFF** (`FORM`/`AIFF`, `COMM`: 1 ch, 16-bit, rate 44100 (80-bit float `400e ac44…`), with `MARK` "Loop"
markers and an `INST` chunk) — see TEXTURES.md §6. Any AIFF reader (libsndfile is installed) handles them.

## TB `70R4N.MP4`, CD audio

Out of scope here (MP4 = intro video, decode with ffmpeg).
