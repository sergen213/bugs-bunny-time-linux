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

### C++ implementation — Implemented, Tested

`engine/audio` (`bb_audio`, `include/bb/audio.h`): `ImaState`/`decodeXa`/`XaStream` (streaming, layout from the
path: `…/SPEECH/<NN>/*.xa` → mono, everything else stereo), `xa2wav` dev tool, OpenAL `AudioDevice`
(streamed looping music, 4 × 0.25 s buffers pumped from the main loop, volume, stop; one-shot SFX).
Tests: `audio.codec` (hand-computed IMA vectors, index floor, s16/index-88 saturation, both layouts, truncated
prefixes, stream rewind, AIFF/sound-bank synthetic cases), `audio.realdata` (`BBTIME_DATA`).

Real data (`audio.realdata`, 2026-10-18): every `.xa` decodes to exactly `xaFrames(bytes)` frames.

| game | files | mono | decoded | clipped samples |
|------|-------|------|---------|-----------------|
| LiT | 35 (32 `track/TRACK2…33.xa` + 3 `speeches/`) | 0 | 5094.4 s (1.42 h) | 0.0005 % |
| TB | 446 (44 `TRACK/track1…44.xa` + 402 `SPEECH/NN/`) | 402 | 14567.4 s (4.05 h) | 0.0007 % |

C++ `xa2wav` output is byte-identical to `tools/re/xa2wav.py` (5 s of LiT `TRACK25.xa`, 3 s of TB
`SPEECH/00/Speech0.xa`); `TRACK25.xa` 1 790 294 B → 1 790 294 frames = 40.596 s (WAV 44 + 4 × frames bytes).

## Music selection

**LiT — Verified (code + data).** `bugs.exe` 0x41d630 `PlayMusic(n)` opens `..\track\Track%d.xa` with `n`
unchanged. `n` comes from a per-level zone table: section-1 chunk `0x05`, tag `0x06` = 3 bytes
`{zone, track, track2}` (loader `case 5` in 0x42e7a0 → 0x41d8b0 appends, max 0x28 entries); 0x41d900 picks the
entry whose zone equals `*(u16*)(DAT_004b324c+0x26)` (the player's current zone — Inferred) and plays `track`
(no entry = stop); tracks above `DAT_004abf78` (= 0x28) are clamped to 39. Examples: `title.bze` {1,25,25},{2,25,25};
`credits.bze` {1,25,25}; `LS01.bze` {1,2},{2,5},{3,9},{4,14},{5,16},{6,29}; `L01a` zones 2–6 → 2; `Merlin` → 31.
`track2` is played by 0x41edb0 (same clamp) for the current zone entry; when it is chosen over `track` is not
decoded — **Research Required** (all levels but `CC4B/CC4C` zone 6 {10, 11} use `track2 == track`).

**TB — partly.** `bblit2.exe` 0x4291e0 `PlayMusic(n)`: `-1` = resume current, `0` = stop, else opens
`Track(n-1).xa`, and if that file is missing retries the hard-coded **`Track14.xa`** (0xe). `n` is set only by
logic action slot 126 (0x432db0: `music = *(u8*)(action+2)`, i.e. low byte of arg1; LiT counterpart is the
zone table above) and reset to 0 at level load (0x407480). Logic opcodes are u16 words: `w & 0x1f` = kind
(7 = action), `(w >> 5) & 7` = operand flags (4: one u16 = two s8 args; 1: u16 arg1; 2: u16 arg2), `w >> 8` =
action id (interpreter 0x45d7a0). Byte scans of section 1 for such action-126 words found no consistent hits,
so the logic blob location in TB levels is still **Research Required**.

**Collection menu (`apps/bbtime/music.cpp`):** LiT → `TRACK25.xa` (the title-level track, verified above).
TB has no title level on disk and its GDI front end (`..\data\menu.men`) issues no track change, so its menu
theme is **UNVERIFIED**: `track14.xa`, the exe's own fallback track. The highlighted game's track loops in
the main menu, the chosen game's in its level list; level screens are silent; `--mute` never opens a device.

## Sound effects and in-level speech (not `.xa`) — Implemented, Tested

Short SFX live in BZE section 3 and per-language speech clips in sections 5–10 as big-endian
**AIFF** (`FORM`/`AIFF`, `COMM`, `SSND`, plus `INST`/`MARK` loop data and, in TB, `APPL` "auFM") — see
TEXTURES.md §6. Section 3 starts with `u32 n, u32 offsets[n]` → FORM blobs (both games) when the level has a
sound bank; cutscene/loader levels start directly with TIMs (`u32 0x10`). Exe readers: LiT 0x41e950, TB 0x42a7c0
(chunk tags compared against `SSND INST COMM MARK`; neither checks SSND against the frame count).

| game | levels with bank | sounds | format | notes |
|------|------------------|--------|--------|-------|
| LiT | 62 / 167 | 3055 (939.9 s) | 1 ch, 16-bit **big-endian PCM**, 44100 Hz | game byte-swaps `ch·frames·2` bytes in place |
| TB | 47 / 58 | 2302 (1703.3 s) | 1 ch, nominal 16-bit, **4-bit IMA ADPCM** in SSND; rates 6000–44100 (1470 × 16000, 536 × 11025, 131 × 22050, …) | 0x42a7c0 decodes `SSND size − 8` bytes with CvtMonoAdpcm 0x40ded0 (stereo: CvtStereoAdpcm 0x40dc90), state 0 |

TB ADPCM SFX: COMM frames = 2 × bytes (1548) or 2 × bytes + 1 (677, odd count, padding nibble); low nibble
first verified by data (Granny bank: |mean|/std 0.002 low-first vs 0.50 high-first).
Quirks, handled bounds-safely by `parseAiff` (`truncated` flag, data clamped to what exists): 7 LiT sounds
(L02A2, L05A3a) declare 1.5× more frames than SSND holds — the game reads past into the next blob; 75 TB sounds
declare more frames than decoded; 4 TB sounds (ARBOSS01) have an SSND 8 bytes longer than their FORM.
Observed in the LiT decompile only (not verified): 0x41e950 takes sample data at SSND body + 2 rather than
body + 8, i.e. it plays the 6 header bytes as 3 zero samples first; `parseAiff` uses the standard body + 8.
Loop markers (`INST`/`MARK`) are not parsed yet (add with the SFX/gameplay wiring).

## TB `70R4N.MP4`, CD audio

Out of scope here (MP4 = intro video, decode with ffmpeg).
