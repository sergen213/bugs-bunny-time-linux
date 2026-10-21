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

**LiT — Verified Against Original Behavior (code + data + Wine oracle).** `bugs.exe` 0x41d630 `PlayMusic(n)` opens
`..\track\Track%d.xa` with `n` unchanged. `n` comes from a per-level zone table: section-1 chunk `0x05`, tag `0x06` =
3 bytes `{zone, track, track2}` (loader `case 5` in 0x42e7a0: 0x41d890 clears, 0x41d8b0 appends, max 0x28 entries);
0x41d900 picks the entry whose zone equals the player's zone `*(u16*)(DAT_004b324c+0x26)` and plays `track` (no
entry = stop); tracks above `DAT_004abf78` (= 0x28) are clamped to 39. Examples: `title.bze` {1,25,25},{2,25,25};
`credits.bze` {1,25,25}; `LS01.bze` {1,2},{2,5},{3,9},{4,14},{5,16},{6,29}; `L01a` zones 2–6 → 2; `L01d1` → 4;
`Merlin` → 31. Every playable level has entries; the cut-scenes CC2A–CC3D have none and are silent in bbtime (were
their chunk 0x05 absent rather than empty, the exe would keep the previous level's table — not checked).

Runtime rules (O; `bb::game::LitWorld::musicTick`, `ctest -R game.sfx` `testMusic`):
* 0x41d900 runs at load (0x430f80, after the load-time actor update) and every unpaused tick; it switches only when
  the player's zone changes (`DAT_004b3b76`, 0xffff at level init), and not when the new entry's two tracks equal the
  current one (`DAT_004b39d4`). A switch to the track already streaming keeps streaming (0x41d630 compares
  `DAT_004abf94`).
* 0x41d630 stops the stream instead of playing while a fade (`0x4b3232`) or speech (`S[0x17]` = `0x4b2277`) runs;
  0x41edb0 (every tick, before 0x41d900) reopens the current entry's `track2` once the stream is stopped or ended —
  so music restarts from its start after every fade and speech, and `track2` follows `track` (CC4B/CC4C zone 6:
  10 then 11; elsewhere the track loops).
* Pause / help (S[12] 1 or 3): 0x41d5c0 stops the DirectSound buffer keeping its position, 0x41d5e0 resumes.
* The config music volume `DAT_009ca854` (0–100, ±5 steps) below 50 disables music; the buffer volume is
  (v − 100) · 100 hundredths of a dB. bbtime keeps its own `music_volume` slider (not ported).

Wine oracle (Proton-GE, `AUDIO=1` into a PulseAudio null sink; `local/wine/litmusic/capture.sh`, `trace.csv`,
`fds.log`): `/b04` L01A holds `track/TRACK2.xa` open (`DAT_004abf94` = 2, playing); a poked level change to 7 (L01D1)
stops it on the fade-out tick, loads L01D1 and opens `TRACK4.xa` immediately (plays over the rest of the 3000 ms
loading screen), the fade in stops it (counters 13–37) and it restarts at counter 39; L01D1's intro speech
(`S[0x17]` 2 → 4 → 0, counters 111–359) silences it again and it restarts at 361. bbtime matches these rules
(the loading screen hold is timed from the load in the exe: load at +0.53 s after the fade, fade in 3.0 s later).

**TB — partly.** `bblit2.exe` 0x4291e0 `PlayMusic(n)`: `-1` = resume current, `0` = stop, else opens
`Track(n-1).xa`, and if that file is missing retries the hard-coded **`Track14.xa`** (0xe). `n` is set only by
logic action slot 126 (0x432db0: `music = *(u8*)(action+2)`, i.e. low byte of arg1; LiT counterpart is the
zone table above) and reset to 0 at level load (0x407480). Logic opcodes are u16 words: `w & 0x1f` = kind
(7 = action), `(w >> 5) & 7` = operand flags (4: one u16 = two s8 args; 1: u16 arg1; 2: u16 arg2), `w >> 8` =
action id (interpreter 0x45d7a0). Byte scans of section 1 for such action-126 words found no consistent hits,
so the logic blob location in TB levels is still **Research Required**.

**Collection menu (`apps/bbtime/music.cpp`):** LiT → `TRACK25.xa` (the title-level track, verified above). TB has no
title level on disk and its GDI front end (`..\data\menu.men`) issues no track change; the collection menu uses the
Granny hub track `Track2.xa` (design choice). The highlighted game's track loops in the main menu, the chosen game's in
its level list; LiT levels (TITLE included) follow `LitWorld::music`, TB levels action 126; `--mute` never opens a
device.

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
Loop data (`AiffSound::loopStart/loopEnd`, frames): needs ≥ 2 `MARK` markers; LiT (0x41e950) loops between the
markers named by the `INST` sustain begin/end ids, TB (0x42a7c0) between the first two markers in `MARK` order
(INST ignored). L01a: 3 of 53 blobs loop, ARCA01: 5 of 51.

### Playback rules (both exes; `engine/game` `bb/game/sfx.h`) — Implemented, Tested; LiT levels + frequency **Verified** (live oracle)

O = decompile, I = inferred. LiT addresses first, TB second.

* **Tables** (level loader chunk 0): tag 0x01 program table in section 4 (`u32 n`, n × 16 programs × 8 B
  `{u16 blob 1-based, ?, ?, u8 volume, ?, u8 note, ?}`, 0x41e030), 0x02 bank in section 3 (0x41e0e0 / 0x429e50),
  0x04 sound entry (0x41de40 / 0x406ff0: LiT 8 B `{u16 id, u8 bank, u8 program, u16 flags, u8 add, u8 chance}`,
  max 0x80; TB 12 B with u16 add/chance/maxDist, max 0xa2), 0x4c/0x4d (TB 0x4b/0x4c) per-language speech programs /
  bank in the language section = the file entry right after section 3 (language 0: section 6; buffer index = blob +
  section-3 count, 0x4b1c80). Entry flags: 2 non-positional, 0x10 released when the emitter is freed, 0x20 a looping
  voice follows its emitter, 0x80 chance, 0x100 part offset (**not applied**: layout of `*(actor+0x78)+0x84`
  unknown), 0x200 add to levels, 0x400 speech, TB 0x4000 own max distance. O.
* **Request** 0x41deb0 / 0x407080 `(emitter, id, mode, view)`: mode 1 takes the first free of **24 voices**
  (0x4b3260 20 B / 0x63b740 28 B; none → nothing, no rand), then the first entry with that id, then chance
  `rand() >= chance·0x80`; voice = `0x8001`. Mode 2 marks the emitter's voice of that id (else any voice of it)
  `0x8002`. Release-owned 0x41f5d0 / 0x407220 at actor free (flag-0x10 voices → 0x8002; LiT drops a never-started
  one). C85/C87 0x42b930/0x42b990 (TB 0x4308a0/0x430900): a voice with `state & 5` and the id (C87 also emitter). O.
* **Mixer** 0x41edb0 (every tick right after 0x447d40) / 0x42ae60 (top of the tick, before the entities). Each
  blob has 10 DirectSound buffer copies; looping blobs use 5 pairs (intro + loop region, a list of ≤ 10 looping
  instances, one per blob and emitter). Start: no emitter → voice freed; levels (below), volume, pan
  `(l − r)·3000 >> 7`, LiT frequency `rate · 2^(5 − note/12)` (every LiT AIFF says 44100: the note is the PS1
  centre note, so most sounds play at 15592 Hz — the old native-rate playback was ~2.8× too high; outside
  100..100000 Hz SetFrequency fails, I), TB plays at the AIFF rate; first free copy, none → silent but the voice
  holds. Playing: one-shots free the voice when **copy 0** stops (the exes poll copy 0 only); flag-0x20 loops
  re-level every pass (volume only, all copies of the blob): LiT zero levels or no emitter → voice freed while
  the instance loops on at its last volume until a release; TB → −10000 and the voice re-requests its start
  (no-op). Release: one-shot stops all 10 copies, loop stops its instance. The loop region's buffer gets **−pan**
  (both exes). O.
* **Levels** 0x41f620 / 0x407250: players (Bugs; TB Bugs and Taz) fixed 100/100; else distance
  `max + (mid + min)/4` from the camera **target**, ≥ 4096 (TB flag 0x4000: maxDist) → 0/0; `v = 127 − d·128/max`;
  angle = FPATAN(dx, dz)·2048/π of the emitter from the eye minus the target's, folded to ±0x400; `a < 0`: right
  `v + a·v >> 10`, left v; else left `v − a·v >> 10`, right v; flag 0x200 adds `add` (≤ 127). Non-positional
  (flag 2) start levels = add/add. O.
* **Volume** (hundredths of dB): LiT `((min(prog.volume + 100, 127) · (avg·cfg/100) >> 7) − 127)·3000 >> 7`
  (cfg 0x9ca850 0..100; out of range still −29.8 dB); TB `((avg · (prog.volume·100/127 · (cfg·100 >> 7)/100))/127
  − 100)·30`, ≤ 0, −10000 when levels or cfg (GD+0x97, 0..127) are 0. O.

Port: `bb::game::Sfx` simulates the voices, copies and loop list on the 30 Hz tick (a copy plays `frames·30/rate`
ticks), so C85/C87 and voice exhaustion are deterministic; `sfx.device = false` reproduces the audio-less Wine
oracle (mixer never runs: voices never free, started voices stay pending). The app replays the mixer's
`SfxCmd`s on OpenAL (`AudioDevice::sfx`: pan baked into a stereo copy with direct channels, gain 10^(dB/20),
loop points via `AL_SOFT_loop_points`); the config volume follows the device SFX volume (settings.ini). Bugs'
glide (0xd) / dig (0x1b) loops 0x437ab0, item flags 0x200/0x400, A34/A82, zone triggers, rope swing, frame
sounds; TB '`' frame sounds, transition 0x20/0x40, actions 20/63, call-friend refusal 0x5f. Test `game.sfx`.
**TB runtime check (R, 2026-10-21).** Oracle `local/wine/tbnpc/sfx/requests.csv` (script `sfx_capture.py` there,
RUNNER=wine, ARCA01 `/b28`, walk/run/jump/kick/switch/Taz walk/jump/spin; pending voices of 0x63b740 per tick —
Wine provides a DirectSound object even without `AUDIO`, so the mixer runs): 65 requests. Most player and NPC sounds
are `` ` `` items of the **animation-asset logic** (chunk 0x22 'M' blocks, run by 0x469770 on each frame step),
which bbtime did not run (2 requests in 400 ticks). Now ported (`TbWorld::animLogic_`): Bugs walk (state 37) 11/10
every ~10 ticks, run (30) a rand pick of 12/13/14 every 10 ticks, jump 19, 2 (39), 22 (54); Taz walk 71/72 every 4
ticks, jump 74, spin 82 start in 135 and release (mode 2) by 140 after ~14–15 ticks; guards 266–268 sound 60 every
32 ticks — the same ids, states and cadences as the capture (inputs differ, so not tick-aligned). The '`' gate
needs the hold countdown at 0, so the list runs before the hold reload (I). Its rand() draws (`W` chance 0x45a6df,
`S` 7/8 0x430f76) use the shared stream like the exe (§ RUNTIME_REFERENCE 4, `local/wine/tbrand/`). Open: TB
speech-table sounds (flag 0x400) need the per-language XA mapping check.
**LiT runtime check (R, 2026-10-21) — Verified.** Oracle `local/wine/sfxcheck/lit_starts.csv` (scripts
`lit_sfx_hook.py` + `lit_hook.s` there; L01A `/b04`, RUNNER=wine: under Proton-GE without `AUDIO` LiT gets no
DirectSound buffer and the 24 voices stay 0x8001 forever). A code cave (`.text` slack 0x45b260, ring in the `.idata`
page tail 0x9ccc60) hooked at 0x41f094, just before the start path's SetFrequency, logs the frequency argument, the
voice (state 0x8004, levels +0xc/+0xd, entry), the emitter position and the eye/target 0x4b38c0..d4 the mixer used:
51 starts in 30 s of play (Bugs' steps 7/8/9, sounds 3/16/18, speech 53, emitters 69–72 and 95/96/103). All 25
positional starts match `sfxLevels` exactly (both pan sides, 0/0 beyond 4096: e.g. 26/14, 28/6, 3/24, 7/1), Bugs is
100/100, and every frequency equals `sfxRateLit(44100, note)` of the first table entry's program: notes 70/71/78/80/82
→ 24750/23361/15592/13891/12375 Hz (speech 53 uses the language bank's program). Asserted in `game.sfx`.

Not ported: LiT 0x4467cd push sound (global 0x4b2160), 0x43ea74/0x43eb31 releases of 0x4a in level 3, emitter
part offsets (flag 0x100), 2-player views (the view byte always matches).

## TB `70R4N.MP4`, CD audio

Out of scope here (MP4 = intro video, decode with ffmpeg).
