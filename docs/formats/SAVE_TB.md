# TB save files — `Savegame%d.dat`, `Savedata.dat` (bblit2.exe)

Evidence keys
- `[G]` Ghidra `bblit2.exe`: request 0x401b60 (S[0x29] = 6), slot pick 0x428f70, writer 0x428a50, record 0x428b00,
  Savedata writer 0x428d70 / reader 0x428e50, load 0x428bf0, delete 0x428fb0, checksum 0x433600, percent 0x464050.
- `[W]` Wine (RUNNER=wine) saves of one GRANNY session, `local/saves/tb/`: `save1_granny` (first save, new file 0),
  `save2_granny` (second save, prompt "Do you want to use the same savegame file?" → YES, file 0 again, with the
  GAME_DATA dump `gd_before3.txt`), `save3_newslot` (third save → NO, new file 1, `gd_before4.txt`, and
  `gd_afterload.txt` after the title menu's Load of it). Screens: `local/wine/tbsave/p3.png` (pause menu),
  `p8.png` (same-file prompt), `l1.png` (Load page listing both), `l2.png` (loaded GRANNY).
- `[T]` `ctest -R game.tb_save`: the native writer reproduces the `[W]` Savegame and Savedata bytes.

Location: `Datas/BIN/` (`..\bin\`). bbtime: `$XDG_DATA_HOME/bbtime/saves/tb/`, every overwrite first copies the old
file to `<name>.bak`.

## `Savegame<N>.dat` — 688 bytes (0x2b0), raw `GAME_DATA[0..0x2af]` — Verified [G][W][T]

| offset | size | content |
|---|---|---|
| +0x00 | u32 | level-table index (0x4cd998; GRANNY = 4) — the load continues there |
| +0x04 | s32 | checksum: Σ (signed char) of all 0x2b0 bytes **including this field's previous value** (as LiT). [W]: −50 → 153 → 103 |
| +0x08..+0x57 | | pad records 0 / 1 (pressed / held / raw / prev words, +0x10..+0x27 remap words 8000 2000 1000 4000 80 20 10 40 4 1 8 2) |
| +0x58.. | | logic GD (`S`/`R` ops index GD+0x58+n), +0x94 pad-present bits, +0x9f letterbox, +0xa0 lives, +0xa5/+0xa6 health, +0x1f4.. item bits |
| +0x14b | u8 | completion percent written by the save (0x464050: GD+0x5a u16 · 100 / 6010, ≤ 100) |

Load (0x428bf0, title Load page): the file over GAME_DATA, then GD+0x93 = language and +0x14a, 0xfe–0x108, 0x13d
cleared; [W] `gd_afterload.txt` = the file except the pad words.

## `Savedata.dat` — u32 n, `"1.00\0"`, n × 0x54 — Verified [G][W][T]

Record: u8 slot (`N`), text "MM/DD/YY, HH:MM:SS, n%" (`_strdate`, `_strtime`, percent) + NUL at +1 (≤ 0x4b B),
u32 level at +0x4c, +0x50 the in-memory next pointer. The record is a `malloc` block: a new one is 0xcd-filled past
the NUL; an update `sprintf`s over the old text (its tail stays). The exe writes its heap pointer in a non-last
record's +0x50 ([W] save 3: 0x01602900); bbtime writes 0 (never read; the test masks it).

## Behaviour

* **Trigger**: the pause menu (Start/Esc → 0x4014c0: S[9] = 1 + template 0x1d, only flags1 0x40000000 entities
  tick) item "save game" → its logic sets S[0x29] = 6 with S[0x27] = 1; on a game that already has a file it first
  asks "use the same savegame file? YES NO CANCEL" (S[0x11] 1/2/3, NO default) and copies S[0x11] to S[0x27].
* **0x401b60** (next frame, no reload pending): 0x428f70 — S[0x27] == 2 (NO) or no file yet (0x48840c = −1, new
  game) → slot = record count; else the current slot. 0x428a50: GD+0x14b = percent, GD+4 = checksum, write
  `Savegame<slot>.dat` (0x2b0, "wb"); on success 0x428b00 (find / append the record: level GD+0, text) and 0x428d70
  (rewrite Savedata). S[0x27] = 0x0e written / 0x0f failed; S[0x29] = 0. The menu closes after success.
* Delete and the title list: TIME_BUSTERS.md §8.1.
