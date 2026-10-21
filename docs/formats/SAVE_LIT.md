# LiT save files — `Savegame%d.dat`, `Savedata.dat` (bugs.exe v1.6)

Evidence keys
- `[G]` Ghidra `bugs.exe`: request handler `0x407190` (S[0x0a] 4 new game / 5 load / 6 save), writer `0x4074f0`,
  Savedata reader `0x4076f0`, checksum `0x42e5e0`, fopen wrapper `0x405760` (falls back to the CD path),
  strings `..\bin\Savegame%d.dat` `0x45f3b8`, `..\bin\Savedata.dat` `0x45f3d0`.
- `[W]` Wine-produced files `local/saves/lit/save{1,2,3}_*/` (RUNTIME_REFERENCE.md §9): three saves of slot 2 in
  one session, the pre-save GAME_DATA dumps `gd_before*.txt`, and a load back into the original.
- `[T]` `ctest -R game.lit_save`: the native writer (`bb/game/lit_save.h`) reproduces all three `[W]` file pairs.

Location: `Datas/bin/` next to `bugs.exe` (paths are relative, `..\bin\`). bbtime keeps the same files in
`$XDG_DATA_HOME/bbtime/saves/lit/` (`bbtime --import-saves <Datas/bin or install root>` copies valid originals;
every overwrite first copies the old file to `<name>.bak`; the launcher's Saves tab backs up the whole directory).

## `Savegame<N>.dat` — 320 bytes, raw `GAME_DATA[0..0x13f]` — Verified [G][W][T]

`GAME_DATA` = `*(u32*)0x52fd00 + 0x10000`. `N` = `G[0x9f]` (GD+0xdf), the slot picked in the title menu (1..6).
A level booted directly with `/bNN` has `G[0x9f]` = 0 and would write `Savegame0.dat` (mask bit 31, no summary;
*inferred* from [G], not run). The shipped `Savegame0..5.dat` are all zero (blank; `Savegame6.dat` is created on
first use).

| offset | size | content |
|---|---|---|
| +0x00 | u32 | current level id (exe level table 0x4ad380; MERLIN = 51) — load continues there, 0 → 0x38 CCINTRO |
| +0x04 | s32 | checksum: Σ (signed char) over **all 320 bytes including this field's previous value**, stored before writing. Never verified on load. [W]: save 1 (old 0) = 378 = 0x17a; save 2 over it = 514 + 0x7a + 0x01 = 637 |
| +0x08..+0x3f | | pad words of the frame (GAMEPLAY.md §2.2: held/pressed/raw buttons, sticks, analog calibration +0x16..+0x20, remap words +0x2e..+0x3c) — transient, overwritten by the live pad after a load |
| +0x40..+0x13f | 256 | `G[0..255]` logic globals (LOGIC.md §1): G[0] 3, G[1]/G[2] health/max, G[3] carrots, G[5] clocks, G[7] + G[0xfc]<<8 golden carrots, G[0x0e] next level, G[0x10] language, G[0x24] cheat bits, G[0x25] world, G[0x9f] slot, collected-item flag bits (GAMEPLAY.md §7.4) |

Player position is not saved: a load restarts the saved level at its spawn ([W]: (5940,−560,10050) in MERLIN).

## `Savedata.dat` — 360 bytes (0x168), image of `0x4b28e0` — Verified [G][W][T]

| offset | size | content |
|---|---|---|
| +0x00 | u32 | 0 in every observed file (not written by 0x4074f0) |
| +0x04 | u32 | slot mask, bit `1 << ((slot − 1) & 31)` set by each save |
| +0x08 + 0x2c·(slot−1) | u16 | golden carrots `G[7] | G[0xfc] << 8` at save time (slots 1..6) |
| +0x0a + 0x2c·(slot−1) | u16 | clocks `G[5]` |
| rest | | zero in all observed files; preserved unchanged |

[W] save 3: `+0x04` = 2, `+0x34` = `4d 01 7c 00` (333 golden carrots, 124 clocks); the title menu shows
"2. 333 124" from these (S[0x2b..0x40], 0x4076f0), not from the slot file.

## Behaviour

* **Save** (S[0x0a] = 6, set by the "Do you want to save your game?" Yes of the type-16 menu entity present in
  every playable level; the prompt (overlay scripts 68–72, GAMEPLAY.md §7.6) opens when S[17] = 1, written by the
  Merlin/LS01 save actors after their speech): checksum, write
  `Savegame<N>.dat` ("wb", 320 B); on success mask |= bit, slot summary, write `Savedata.dat` (360 B); then the
  in-memory mask is XORed with the bit again (`0x4076cc`, file keeps it; the title re-reads the file).
  Result `S[8]` = 0x0a ok / 0x14 failed (+ `S[0x1b]` = 7), which entity 0x51c5 copies to S[17] (10 / 20).
  bbtime: the drawn prompt saves in play (GAMEPLAY_STATUS.md Saves row: MERLIN smoke, slot 3).
* **Load** (S[0x0a] = 5, title "Load game", slot must be in the mask): `fread` 320 B over GAME_DATA (short read →
  `S[0x1b]` = 4 and new-game values), keep `G[0x10]` = config language, `G[0x36]` = 0, next level = byte +0 (or 0x38).
* **New game** (S[0x0a] = 4): G[] = 0, G[0] 3, G[1] = G[2] = 6, G[0x10] language, G[0x11] 0, calibration
  min/max/centre 0/0xff/0x7f, next level 0x38; the slot byte survives.
* **Title**: `Savedata.dat` is read (created zero-filled if missing) when S[3] = 1.

## bbtime validation (`parseLitSaveGame`)

Rejects: wrong size, slot byte ≠ file number, level id outside the table. The checksum cannot be validated (it
depends on the previous checksum); bbtime recomputes it with the original rule on every save.
