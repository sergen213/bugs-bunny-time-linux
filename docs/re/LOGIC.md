# Level logic VM and actor scripts — LiT 1.6 (`bugs.exe`), TB cross-map

Status: **Researched (static RE)**; nothing here is *Verified Against Original Behavior*. Tags: *observed* = read
from the decompile (`ghidra/diff/out/bugs.exe.c`) or exe bytes at the cited address; *inferred* = interpretation.
Companion of [GAMEPLAY.md](GAMEPLAY.md) (actor struct, main loop). BugsDecomp (no license) is cited for names only.

## 1. Storage the VM operates on (*observed*)

| name | address | size | persistence | notes |
|---|---|---|---|---|
| `S[]` scratch | `0x4b2260` (global, **not** per entity) | 256 B | level runtime, not saved | engine also uses it: `S[0x00]` (`0x4b2260`, checked by the tick), `S[0x03]` (`0x4b2263`: 1 → load `Savedata.dat` on the title, blocks damage when ≠0), `S[0x08]`, `S[0x0a]` (`0x4b226a`: save/load request 4 new game / 5 load / 6 save, see GAMEPLAY.md §8), `S[0x0c]` (`0x4b226c`: pause state 0 run / 1 pause menu / 2 unpausing / 3 other), `S[0x17]` (`0x4b2277`: demo/extended-skip), `S[0x1b]` (`0x4b227b`: save/load status), `S[0x2b..0x40]` (title slot summaries) |
| `G[]` globals | `GAME_DATA+0x40` = `*(0x52fd00)+0x10040` | 256 B | saved in `Savegame%d.dat` | `G[0]` 3 at new game (*inferred* lives), `G[1]` health (half-carrots) / `G[2]` max (6), `G[3]` carrots (HUD caps at 99), `G[5]` clocks, `G[7]` + `G[0xfc]<<8` golden-carrot total (every inc/dec/add/sub handler special-cases index 7), `G[0x0e]` next level id (A42), `G[0x0f]` input-disable (pad 0x4491c0 skips reading), `G[0x10]` language (= config+8), `G[0x25]` world 1..6, `G[0x36]` (`0x10076`) camera/cutscene lock, `G[0x9f]` save slot 1..6, `G[0xa7]` bit7 analog-pad flag |
| self | the actor running the item (behaviour fn arg) | 0x1f4 B | — | timer `+0x22` (u16, +1 per tick in 0x447d40), `+0x1e` u16 general register (tag 0x1e), zone `+0x26`, flags `+0x14`/`+0x18` |
| P | player actor `*(0x4b324c)` | | | state `+0x17c`, flags `+0x14` (`flags14`), `+0x18` (`flags18`) |

## 2. Item encoding

8-byte logic item `{u8 cond, u8 A, u8 B, u8 act, u16 X, u16 Y}` at node `+0x10` (*observed*: every caller passes
`node+0x10` to `TestLogicCond` 0x42a7d0 and `RunLogicAction` 0x42c6d0; handlers read A at `+1`, B at `+2`, X at `+4`,
Y at `+6`; some conditions read `AB` = u16 at `+1`). Code 0 for `cond` means "no condition", 0 for `act` means "no
action" (callers skip the call). Out-of-range codes: conditions return false, actions do nothing (dispatch bounds
99 / 129, *observed* in 0x42a7d0 / 0x42c6d0, tables `0x4ac550` / `0x4ac6e0`).

Section-1 sources (loader 0x42e7a0, entity chunks 0x07/0x08/0x0a; nodes are appended to per-actor lists in a
level arena `0x4b3a00`, limit `0x4b247c`; overflow = `jmp $` hang) — *observed*:

| tag | payload | node size | list | node layout (payload offset → node offset) |
|---|---|---|---|---|
| `0x31` | 32 B | 0x24 | `actor+0xfc` | `[0..1]→+4 u16 group`, `[2..3]→+6 u16 next-state index`, `[4..7]→+8 u32 gate mask on flags14`, `[8..11]→+0xc u32 gate mask on flags18`, `[12..19]→+0x10 logic item`, `[20..23]→+0x18 u32 item flags`, `[24..27]→+0x1c u32 P1`, `[28..31]→+0x20 u32 P2` |
| `0x35` | 40 B | 0x2c | `actor+0xfc` | `+4,+6` u16, `+8` s16, `+0xa` u16, `+0xc` u16, `+0xe/+0xf` u8, `[12..19]→+0x10 logic item`, `+0x18` u16, `+0x1c,+0x20,+0x24,+0x28` u32 |
| `0x30` | 32 B | 0x20 | `actor+0xf8` (script steps) | `+4 u16 step id`, `+6 u16 anim id`, `+8 s16 anim speed/blend`, `+0xa u16 event-group id`, `+0xc u32 step flags` (bit 2 = kill actor when the animation ends), `+0x10 u32`, `+0x14..+0x1c u16×5` (`+0x1e` used as byte offset into the `+0xfc` list) |
| `0x34` | 24 B | 0x1c | `actor+0xf8` | `+4,+6,+8` u16, `+0xa` u16, `+0xc` u32, `+0x10` s32, `+0x14,+0x16,+0x18` u16 |
| `0x32` | 12 B | 0x10 | `actor+0xf4` (script states) | `+4 u16 state id` (2 = preferred start, 1 = default start), `+6..+0xf` 5 × u16 step ids (`0xfff0` terminates) |

**Cross-references are IDs in the file, offsets in memory** (*observed*, 0x42e600, run after each entity chunk for
behaviour types 2, 0x0e and 0x22): 0x32 state-node `+6..+0xf` step IDs → byte offsets into the `+0xf8` list
(stride 0x20, i.e. 0x30 nodes); 0x30 step `+0xa` event-group ID → offset of the first `+0xfc` item with that group
(`+0x1e`, stride 0x24); 0x31 item `+6` next-state ID → offset into `+0xf4` (stride 0x10); with item flag 0x20 also
P1/P2 (`+0x1c/+0x20`) are state IDs → offsets. IDs live at node `+4` (state id / step id / item group). Script start
state = state id 2 if present, else 1 (0x4455e0). A native loader should keep IDs and resolve to indices.

## 3. Item evaluation dialects (who runs `+0xfc`)

*Observed* — the same list is interpreted differently by different behaviours:

1. **Script graph** (behaviour 2 `0x4455e0`, also 34 `0x446990`, 16 `0x4445e0`, 36 `0x444b50` variants): the actor
   is in script state `+0x180` (0x32 node) at step index `+0x17c`, current step `+0x184` (0x30 node). Each tick it
   plays the step's animation via `0x438970(actor, actor+0x144, animId, speed, work)` (returns 0x100 when done →
   next step; past the 5th step or `0xfff0` → restart the start state; step flag 4 → kill). If the step's event
   group `+0xa` ≠ 0 it scans `+0xfc` items whose `+4` group matches, and for each item applies the **item flags**
   (`+0x18`) as pre-gates and side effects:

   | item flag | meaning (*observed* in 0x4455e0) |
   |---|---|
   | 0x40 | gate: only when current anim frame `+0x146 == P1` (and `+0x14c == 0`) |
   | 0x04 | gate: only when `actor+0x15c & 0x2000` (anim finished) |
   | 0x02 | gate: random, pass when `(rand() & 0xffff) > P1` |
   | 0x20000 | gate: random, pass when `(rand() & 0xffff) < P2` |
   | (then) | condition (if code ≠ 0) → action (if code ≠ 0) |
   | 0x200 / 0x400 | play sound P1 (mode 1 / 2) via 0x41deb0 |
   | 0x4000000 | kill first actor whose entity id `+0x1c == P2` |
   | 0x80000 | **goto level P1** (`GD.currentLevel = P1`, `0x4b39bc = 1`) |
   | 0x80000000 | `0x4377a0(self, P2, work)` (*inferred* spawn/link actor P2) |
   | 0x10000 | kill self and return |
   | 0x8000 | keep scanning (no state change) |
   | 0x10 | stop scanning, no state change |
   | none of the above | switch to script state `item[+6]` (resolved offset into `+0xf4`), step 0 |

2. **Gated list** (moving platforms 0x43f520 types 12/13/18/19/22–29, and others): for each item, if
   `(node+8 == 0 || flags14 & node+8) && (node+0xc == 0 || flags18 & node+0xc)` and the condition holds → action;
   item flags 0x200/0x400 play sound `+0x1c`, 0x10000 kill self; the scan **stops after the first firing item**
   unless flag 0x8000.

The per-tick message queue (A17) and the `0x4b3fc4` overlay status (C55/C56/A49) are the only cross-actor
channels besides `S[]`/`G[]`.

## 4. Condition table (LiT, 99 entries, `0x4ac550`)

`A`, `B` = item bytes +1/+2; `S`/`G` as §1; `P` = player. Signed compares marked `(s8)`.

| # | hex | LiT handler | semantics (*observed*, decompile) | BugsDecomp name | TB slot (align sim / hash pair) |
|---|---|---|---|---|---|
| C0 | 0x00 | 0x42a800 | `true` — always 1 | CondTrue | cond 0 (1.00) |
| C1 | 0x01 | 0x42a810 | ciseq: `(s8)S[B] == (s8)A` | CondCiseq | cond 1 (0.76) |
| C2 | 0x02 | 0x42aa70 | cigeq: `G[B] == A` | CondCigeq | — |
| C3 | 0x03 | 0x42a8b0 | cisltu: `A < S[B]` (u8) | CondCisltu | cond 3 (0.82) |
| C4 | 0x04 | 0x42ab30 | cigltu: `A < G[B]` (u8) | CondCigltu | — |
| C5 | 0x05 | 0x42a950 | cisleu: `A <= S[B]` | CondCisleu | cond 5 (0.82) |
| C6 | 0x06 | 0x42abb0 | cigleu: `A <= G[B]` | CondCigleu | — |
| C7 | 0x07 | 0x42a970 | cisgtu: `S[B] < A` | CondCisgtu | cond 7 (0.82) |
| C8 | 0x08 | 0x42abd0 | ciggtu: `G[B] < A` | CondCiggtu | — |
| C9 | 0x09 | 0x42aa10 | cisgeu: `S[B] <= A` | CondCisgeu | cond 9 (0.82) |
| C10 | 0x0a | 0x42ac50 | ciggeu: `G[B] <= A` | CondCiggeu | — |
| C11 | 0x0b | 0x42acd0 | player `flags14 & 0x2000020 == 0` (player not in hurt/forced state) |  | — |
| C12 | 0x0c | 0x42ad40 | player state `P+0x17c == A` |  | — |
| C13 | 0x0d | 0x42add0 | `self+0x6c == 0` (nothing carried/attached) |  | cond 11 (1.00) |
| C14 | 0x0e | 0x42adf0 | player zone `P+0x26 == A` |  | cond 12 (1.00) |
| C15 | 0x0f | 0x42a830 | csseq `S[B] == S[A]` | CondCseq/CondCsseq | cond 13 (1.00); cond 13/cond 39 [hash] |
| C16 | 0x10 | 0x42aaa0 | cggeq `G[B] == G[A]` | CondCgeq/CondCggeq | cond 14 (1.00); cond 14/cond 40 [hash] |
| C17 | 0x11 | 0x42ae30 | player NOT facing self: `|angle(self→player pos) − P.yaw| ≥ 0x294` (58°); A=1/2 restrict to one side |  | — |
| C18 | 0x12 | 0x42aee0 | player facing self within ±0x294; A=1: only diff ≤ 0, A=2: only diff ≥ 0 |  | — |
| C19 | 0x13 | 0x42af90 | self facing player within ±0x294 (self yaw `+0xde` vs direction to player); A side filter as C18 |  | — |
| C20 | 0x14 | 0x42b040 | self NOT facing player (inverse of C19) |  | — |
| C21 | 0x15 | 0x42b0f0 | timer `self+0x22 < AB` (AB = u16 at item+0x11, ticks) |  | cond 15 (0.88) |
| C22 | 0x16 | 0x42b110 | timer `> AB` |  | cond 16 (0.88) |
| C23 | 0x17 | 0x42b130 | timer `== AB` |  | cond 17 (0.85) |
| C24 | 0x18 | 0x42b150 | timer/30 `< AB` (seconds) |  | cond 18 (0.73) |
| C25 | 0x19 | 0x42b190 | timer/30 `> AB` |  | cond 19 (0.73) |
| C26 | 0x1a | 0x42b1d0 | timer exactly at second AB (`t % 30 == 0 && t/30 == AB`) |  | cond 20 (0.77) |
| C27 | 0x1b | 0x42b220 | timer/1800 `< AB` (minutes) |  | cond 21 (0.73) |
| C28 | 0x1c | 0x42b260 | timer/1800 `> AB` |  | cond 22 (0.73) |
| C29 | 0x1d | 0x42b2a0 | timer/1800 `== AB` |  | cond 23 (0.68) |
| C30 | 0x1e | 0x42aa30 | tismnz `S[B] & A != 0` | CondTismnz | cond 24 (1.00); cond 24 [hash] |
| C31 | 0x1f | 0x42ac70 | tigmnz `G[B] & A != 0` | CondTigmnz | cond 25 (1.00); cond 25 [hash] |
| C32 | 0x20 | 0x42acf0 | player `flags14 & 0x2000020 != 0` |  | — |
| C33 | 0x21 | 0x42b600 | player `flags14 & 0x10` (same fn as C76) |  | — |
| C34 | 0x22 | 0x42aa50 | tismz `S[B] & A == 0` | CondTisaz/CondTismz | — |
| C35 | 0x23 | 0x42aca0 | tigmz `G[B] & A == 0` | CondTigaz/CondTigmz | — |
| C36 | 0x24 | 0x42ad10 | player `flags14 & 0x1000000` (player under script control) |  | — |
| C37 | 0x25 | 0x42a860 | cisne `S[B] != A` | CondCisne | cond 26 (0.76) |
| C38 | 0x26 | 0x42aad0 | cigne `G[B] != A` | CondCigne | — |
| C39 | 0x27 | 0x42aa50 | tisaz = C34 (same fn) | CondTisaz/CondTismz | — |
| C40 | 0x28 | 0x42aca0 | tigaz = C35 | CondTigaz/CondTigmz | — |
| C41 | 0x29 | 0x42ad90 | player `flags14 & 0x40` |  | — |
| C42 | 0x2a | 0x42adb0 | player `!(flags14 & 0x40)` |  | — |
| C43 | 0x2b | 0x42ade0 | `self+0x6c != 0` |  | cond 28 (1.00) |
| C44 | 0x2c | 0x42ae10 | player zone `!= A` |  | cond 29 (1.00) |
| C45 | 0x2d | 0x42b2e0 | camera hold timer `DAT_004b3dac != 0` (A41) |  | cond 30 (1.00) |
| C46 | 0x2e | 0x42b2f0 | tisxz `S[B] == A` | CondTisxz | cond 31 (0.86) |
| C47 | 0x2f | 0x42b310 | tigxz `G[B] == A` | CondTigxz | cond 32 (0.87) |
| C48 | 0x30 | 0x42ad20 | player `!(flags14 & 0x1000000)` |  | — |
| C49 | 0x31 | 0x42b340 | tbdact: `GD+0x0a & 0x08` (logical R1 'action' held) | CondTbdact | — |
| C50 | 0x32 | 0x42b380 | self yaw byte `((yaw & 0xfff) >> 4)` strictly between A and B |  | cond 33 (0.72) |
| C51 | 0x33 | 0x42b3d0 | player `flags18 & 2` | CondTpnmv | — |
| C52 | 0x34 | 0x42b3f0 | player `flags18 & 4` |  | — |
| C53 | 0x35 | 0x42b410 | player `!(flags18 & 2)` | CondTpmov | — |
| C54 | 0x36 | 0x42b430 | player `!(flags18 & 4)` |  | — |
| C55 | 0x37 | 0x42b450 | run overlay script X (`0x44e310`), store status in `0x4b3fc4`, test `A & status` |  | cond 34 (0.89) |
| C56 | 0x38 | 0x42b480 | `A & status(0x4b3fc4)` |  | cond 35 (0.94); cond 66 [hash] |
| C57 | 0x39 | 0x42b4a0 | raw-pad any-of A (mask byte: bits0-3 △○✕□, bits4-7 ↑→↓←; Start/Return also map to ✕; demo playback with any key → ✕) |  | — |
| C58 | 0x3a | 0x42b560 | `self+0x1e == A` |  | cond 36 (0.75) |
| C59 | 0x3b | 0x42b580 | `A < self+0x1e` (s16) |  | cond 37 (0.75) |
| C60 | 0x3c | 0x42b5a0 | `self+0x1e < A` |  | cond 38 (0.75) |
| C61 | 0x3d | 0x42a830 | = C15 (csseq) | CondCseq/CondCsseq | cond 39 (1.00); cond 13/cond 39 [hash] |
| C62 | 0x3e | 0x42aaa0 | = C16 | CondCgeq/CondCggeq | cond 40 (1.00); cond 14/cond 40 [hash] |
| C63 | 0x3f | 0x42a880 | csne `S[B] != S[A]` | CondCsne | cond 41 (1.00); cond 41 [hash] |
| C64 | 0x40 | 0x42ab00 | cgne `G[B] != G[A]` | CondCgne | cond 42 (1.00); cond 42 [hash] |
| C65 | 0x41 | 0x42a8f0 | csltu `S[A] < S[B]` | CondCsltu | cond 43 (1.00) |
| C66 | 0x42 | 0x42ab80 | cgltu `G[A] < G[B]` | CondCgltu | cond 44 (1.00) |
| C67 | 0x43 | 0x42a9b0 | csgtu `S[B] < S[A]` | CondCsgtu | cond 45 (1.00) |
| C68 | 0x44 | 0x42ac20 | cggtu `G[B] < G[A]` | CondCggtu | cond 46 (1.00) |
| C69 | 0x45 | 0x42b500 | raw-pad mask test, variant of C57 (0x42b500; tail truncated in decompile — inferred 'none-of') |  | — |
| C70 | 0x46 | 0x42a8d0 | cislt `(s8)A < (s8)S[B]` | CondCislt | cond 47 (0.76) |
| C71 | 0x47 | 0x42a990 | cisgt `(s8)S[B] < (s8)A` | CondCisgt | cond 48 (0.76) |
| C72 | 0x48 | 0x42a920 | cslt `S[A] < S[B]` signed | CondCslt | cond 49 (1.00); cond 49 [hash] |
| C73 | 0x49 | 0x42a9e0 | csgt `S[B] < S[A]` signed | CondCsgt | cond 50 (1.00); cond 50 [hash] |
| C74 | 0x4a | 0x42b5c0 | self zone `+0x26 == A` |  | cond 51 (1.00) |
| C75 | 0x4b | 0x42b5e0 | self zone `!= A` |  | — |
| C76 | 0x4c | 0x42b600 | player `flags14 & 0x10` |  | — |
| C77 | 0x4d | 0x42b620 | player `!(flags14 & 0x10)` |  | — |
| C78 | 0x4e | 0x42ad60 | player script step: `*(u16*)(P+0x180 + 6 + P+0x17e*2) == A` |  | — |
| C79 | 0x4f | 0x42b820 | raw-pad any-of A (no Start mapping) |  | — |
| C80 | 0x50 | 0x42b850 | raw-pad none-of A |  | — |
| C81 | 0x51 | 0x42b360 | tbuact: `!(GD+0x0a & 0x08)` | CondTbuact | — |
| C82 | 0x52 | 0x42b640 | player any-of: A bits → flags14 {1:0x1, 2:0x2, 4:0x10, 8:0x8, 0x10:0x200, 0x20:0x400, 0x40:0x4000, 0x80:0x1000000}; B bits → flags18 {1:0x2000000, …} |  | — |
| C83 | 0x53 | 0x42b740 | inverse of C82 (none-of) |  | — |
| C84 | 0x54 | 0x42b900 | `A <= (P.y >> 8) <= B` (signed bytes; height band) |  | cond 52 (0.93) |
| C85 | 0x55 | 0x42b930 | sound id A playing on any voice == (B == 1) |  | cond 53 (0.83) |
| C86 | 0x56 | 0x42ba00 | `P+0x6c != 0` (player carrying) |  | cond 54 (1.00) |
| C87 | 0x57 | 0x42b990 | sound id A playing for self == (B == 1) |  | cond 55 (0.87) |
| C88 | 0x58 | 0x42ab50 | ciglt `(s8)A < (s8)G[B]` | CondCiglt | — |
| C89 | 0x59 | 0x42abf0 | ciggt `(s8)G[B] < (s8)A` | CondCiggt | — |
| C90 | 0x5a | 0x42b880 | newly-pressed (GD+0x3e) any-of A (same byte mapping as C57) |  | cond 58 (1.00); cond 58 [hash] |
| C91 | 0x5b | 0x42b8c0 | newly-pressed none-of A |  | — |
| C92 | 0x5c | 0x42ba20 | menu item B value `== A` (menu table 0x4b3e80, 21×16 B; gated by item link) |  | cond 60 (0.57) |
| C93 | 0x5d | 0x42ba70 | menu item B value `!= A` |  | — |
| C94 | 0x5e | 0x42bad0 | key-binding conflict scan over config bytes 0x9ca8d0..0x9ca8f2 (front-end only) |  | — |
| C95 | 0x5f | 0x42c140 | capture pressed joystick button into joyMap slot (menu item B) — front-end only |  | — |
| C96 | 0x60 | 0x42c670 | tkdesc: Esc (`0x4b18d0 & 2`) | CondTkdesc | — |
| C97 | 0x61 | 0x42c680 | `DAT_004b1a2c != 0` (pause request pending) |  | — |
| C98 | 0x62 | 0x42c690 | joystick enabled (`config+0x38`) == A (bool) |  | — |

## 5. Action table (LiT, 129 entries, `0x4ac6e0`)

`X` = u16 item `+4`, `Y` = u16 item `+6`.

| # | hex | LiT handler | semantics (*observed*, decompile) | BugsDecomp name | TB slot (align sim / hash pair) |
|---|---|---|---|---|---|
| A0 | 0x00 | 0x417010 | nop | PadSetAct/ActionNop | — |
| A1 | 0x01 | 0x42cb20 | kill self (`flags14 |= 0x100`) |  | cond 66 (0.62) |
| A2 | 0x02 | 0x42cb30 | `S[Y]++`, then kill self (pickup idiom) |  | cond 71 (0.61) |
| A3 | 0x03 | 0x42c700 | incs `S[Y]++` | ActionIncs | cond 76 (0.80) |
| A4 | 0x04 | 0x42c720 | incg `G[Y]++` (Y==7: 16-bit golden-carrot total G[7]|G[0xfc]<<8) | ActionIncg | — |
| A5 | 0x05 | 0x42c7b0 | decs | ActionDecs | cond 79 (0.80) |
| A6 | 0x06 | 0x42c7d0 | decg (GC 16-bit when Y==7) | ActionDecg | — |
| A7 | 0x07 | 0x42c830 | clrs `S[Y]=0` | ActionClrs | cond 80 (0.71) |
| A8 | 0x08 | 0x42c850 | clrg | ActionClrg | — |
| A9 | 0x09 | 0x42c870 | negs | ActionNegs | act 1 (1.00) |
| A10 | 0x0a | 0x42c890 | negg | ActionNegg | act 2 (0.55) |
| A11 | 0x0b | 0x42c8c0 | orsi `S[Y] |= X` | ActionOrsi | act 3 (1.00) |
| A12 | 0x0c | 0x42c8e0 | orgi | ActionOrgi | act 4 (1.00); act 4 [hash] |
| A13 | 0x0d | 0x42c900 | andsi `S[Y] &= X` | ActionAndsi | act 5 (1.00) |
| A14 | 0x0e | 0x42c920 | andgi | ActionAndgi | act 6 (1.00); act 6 [hash] |
| A15 | 0x0f | 0x42cac0 | rnds `S[Y] = rand() % X` | ActionRnds | act 7 (1.00); act 7 [hash] |
| A16 | 0x10 | 0x42caf0 | rndg | ActionRndg | act 8 (1.00); act 8 [hash] |
| A17 | 0x11 | 0x42cba0 | post message X to per-tick queue `0x4b2480[8]` (count `0x4b39de`, cleared each tick in 0x447d40) |  | — |
| A18 | 0x12 | 0x42c770 | movsi `S[Y] = X` | ActionMovsi | act 10 (1.00) |
| A19 | 0x13 | 0x42c790 | movgi `G[Y] = X` | ActionMovgi | — |
| A20 | 0x14 | 0x42c940 | movss `S[Y] = S[X]` | ActionMovss | act 12 (1.00); act 12 [hash] |
| A21 | 0x15 | 0x42c960 | movgg `G[Y] = G[X]` | ActionMovgg | act 13 (1.00) |
| A22 | 0x16 | 0x42c9f0 | incss `S[Y]++, S[X]++` | ActionIncss | act 14 (1.00); act 14 [hash] |
| A23 | 0x17 | 0x42ca20 | incgg (GC-aware) | ActionIncgg | act 15 (0.57) |
| A24 | 0x18 | 0x42cb60 | `S[Y]++`, `S[X]…` (0x42cb60; tail truncated) |  | — |
| A25 | 0x19 | 0x42cbd0 | reset self timer `+0x22 = 0` |  | act 16 (1.00) |
| A26 | 0x1a | 0x42cc30 | force player state X: `P.yaw = self.yaw + 0x800`, `P+0x17e = 0`, `P+0x17c = X`, `P.flags14 |= 0x2000000`; detaches player from platform (`P+0x70`) |  | — |
| A27 | 0x1b | 0x42c990 | movgs `G[Y] = S[X]` | ActionMovgs | act 17 (1.00) |
| A28 | 0x1c | 0x42c9c0 | movsg `S[Y] = G[X]` | ActionMovsg | act 18 (1.00) |
| A29 | 0x1d | 0x42cbe0 | once per >1 s: `S[Y]--` and reset timer (countdown) |  | — |
| A30 | 0x1e | 0x42ccd0 | `flags14 = flags14 & ~0x400000 | 0x200000` (mover → state A) |  | — |
| A31 | 0x1f | 0x42ccf0 | `flags14 = flags14 & ~0x200000 | 0x400000` (mover → state B) |  | — |
| A32 | 0x20 | 0x42cd10 | activation `+0x3c &= ~0x40` |  | act 19 (1.00) |
| A33 | 0x21 | 0x42cd20 | drop/attach carried object `self+0x6c` (copies zone/rotation, flags14 |= 0x1000) |  | — |
| A34 | 0x22 | 0x42cdd0 | play sound X, mode 1 |  | act 20 (0.87) |
| A35 | 0x23 | 0x42ce00 | **kill player**: `G[1] = 0`, `0x437b90(deathState, …)` forces death state id `0x4b39d8` (A92) or default `0x4b2370`; after the death animation the player respawns at zone checkpoint `0x4b2460` with health 6 (PlayerRE, *observed* 0x437b90) |  | — |
| A36 | 0x24 | 0x42ce40 | addsi `S[Y] += X` | ActionAddsi | act 21 (1.00) |
| A37 | 0x25 | 0x42ce60 | addgi (GC-aware) | ActionAddgi | act 22 (0.62) |
| A38 | 0x26 | 0x42cfe0 | self yaw (`coord+0x12`) += X*16+Y; rebuild matrix 0x4376b0 |  | act 23 (1.00); act 23 [hash] |
| A39 | 0x27 | 0x42cec0 | subsi | ActionSubsi | act 24 (1.00) |
| A40 | 0x28 | 0x42cee0 | subgi (GC-aware) | ActionSubgi | act 25 (0.62) |
| A41 | 0x29 | 0x42d040 | camera hold `0x4b3dac = X` ticks (camera frozen, player flags14 |= 0x80000000) |  | — |
| A42 | 0x2a | 0x42d060 | **goto level** `GD.currentLevel = G[0x0e]`, `0x4b39bc = 1` |  | — |
| A43 | 0x2b | 0x42d080 | player yaw `= X << 4` |  | act 26 (1.00) |
| A44 | 0x2c | 0x42d0a0 | player `+0x28 = X` |  | act 27 (1.00) |
| A45 | 0x2d | 0x42d0c0 | `S[Y] = self+0x1e` |  | act 28 (1.00) |
| A46 | 0x2e | 0x42d0e0 | `self+0x1e = S[Y]` |  | act 29 (1.00) |
| A47 | 0x2f | 0x42d100 | player `flags18 |= 1` |  | — |
| A48 | 0x30 | 0x42d110 | gvcrt: health `G[1] < G[2]` ? `G[1]++` : `G[Y]++` | ActionGvcrt | — |
| A49 | 0x31 | 0x42d150 | run overlay script X → status 0x4b3fc4 |  | act 30 (0.82) |
| A50 | 0x32 | 0x42d170 | kill actor(s): X==0xff → all type-0x20 overlays, else first actor with entity id (`+0x1c`) == X |  | act 31 (0.95) |
| A51 | 0x33 | 0x42d1c0 | overlay script `S[Y]` |  | act 32 (0.96) |
| A52 | 0x34 | 0x42d1f0 | overlay script `G[Y]` |  | act 33 (0.96) |
| A53 | 0x35 | 0x42d010 | self yaw += (s8)S[Y] |  | act 35 (1.00); act 35 [hash] |
| A54 | 0x36 | 0x42d220 | self `+0x15c |= 0x10` |  | act 36 (1.00) |
| A55 | 0x37 | 0x42d230 | self `+0x15c &= ~0x10` |  | act 37 (1.00) |
| A56 | 0x38 | 0x42d240 | clear `0x4b3b8c/0x4b3c44`, self `+0x192 = 0`, `+0x196 = self.y` |  | — |
| A57 | 0x39 | 0x42d270 | rumble small motor `0x4b3241 = X` |  | act 38 (0.89) |
| A58 | 0x3a | 0x42d280 | rumble big motor power X duration Y (skipped if player in state 0x25 without `0x4b39d0&1`) |  | act 40 (0.73) |
| A59 | 0x3b | 0x42d2c0 | camera shake: freq `X*11` (0x4b3946), amplitude Y (0x4b3948) |  | act 41 (0.80) |
| A60 | 0x3c | 0x42d300 | for i<X: if bit i of G[Y] is 0 → golden-carrot total++ |  | — |
| A61 | 0x3d | 0x417010 | nop | PadSetAct/ActionNop | act 42 (1.00) |
| A62 | 0x3e | 0x417010 | nop | PadSetAct/ActionNop | act 43 (1.00) |
| A63 | 0x3f | 0x42d370 | destroy carried object `self+0x6c` |  | — |
| A64 | 0x40 | 0x42d3a0 | activation `+0x3c |= 0x40` |  | act 45 (1.00) |
| A65 | 0x41 | 0x417010 | nop | PadSetAct/ActionNop | — |
| A66 | 0x42 | 0x42d3b0 | rumble: `0x4b3242 = X`, `0x4b3246 = Y` |  | act 47 (0.74) |
| A67 | 0x43 | 0x42d3d0 | rumble attenuated by distance to camera target (< 0x800) |  | act 48 (0.75) |
| A68 | 0x44 | 0x42d460 | camera shake attenuated by distance (< 0x800) |  | act 49 (0.73) |
| A69 | 0x45 | 0x42d510 | Y=0: `0x4b2190 = X`; Y=1: `0x4b236b = X`; Y=2: `0x4b236a = X` |  | act 50 (1.00); act 50 [hash] |
| A70 | 0x46 | 0x42d550 | X=0 fade-in `0x44be90(1,0x100)`; X=1 fade-out `0x44bec0(2,0x100)` |  | act 51 (1.00); act 51 [hash] |
| A71 | 0x47 | 0x42d590 | `0x44be90(0x41,0x400)` (flash) |  | act 52 (1.00) |
| A72 | 0x48 | 0x42d5a0 | **damage player X** (if no invuln `0x4b3160`): `P+0x38 = X`, `G[1] -= X`, `P.flags14 |= 0x2000000`, invuln `0x4b3160 = 0x1e` (30 ticks); `G[1] < 1` → death flag |  | act 53 (0.68) |
| A73 | 0x49 | 0x42d670 | camera flags `&= ~0x2000` (look allowed) |  | — |
| A74 | 0x4a | 0x42d680 | camera flags `|= 0x2000` (look disabled) |  | — |
| A75 | 0x4b | 0x42cf40 | self yaw += ((X*16+Y) * stickX(GD+0x12) * dt(0x4b2384)) >> 7 (stick steering) |  | act 56 (1.00); act 56 [hash] |
| A76 | 0x4c | 0x42cf90 | same with S[Y] |  | act 57 (1.00); act 57 [hash] |
| A77 | 0x4d | 0x42d690 | camera defaults: target-height `0x4b215c = −200`, eye height `0x4b2150 = 600`, distance `0x4b2154 = 1100` |  | act 58 (1.00); act 58 [const] |
| A78 | 0x4e | 0x42d6b0 | `0x4b215c = X*100 + Y` |  | act 59 (0.67) |
| A79 | 0x4f | 0x42d6d0 | `0x4b2150 = X*100 + Y` |  | act 60 (0.67) |
| A80 | 0x50 | 0x42d6f0 | `0x4b2154 = X*100 + Y` |  | act 61 (0.67) |
| A81 | 0x51 | 0x42d750 | demo control: X>1 start demo playback (`S[0x17]=8`, id X); X=1 start; X=0 stop |  | act 62 (0.95); act 62 [fuzzy] |
| A82 | 0x52 | 0x42d7f0 | play sound X, mode 2 |  | act 63 (0.87) |
| A83 | 0x53 | 0x42d830 | camera flags `|= 0x8000` (L2/R2 rotation disabled) |  | — |
| A84 | 0x54 | 0x42d820 | camera flags `&= ~0x8000` |  | — |
| A85 | 0x55 | 0x42d840 | camera target actor `0x4b39b8 = self` |  | act 83 (1.00) |
| A86 | 0x56 | 0x42d050 | player `flags14 |= 0x80000000` |  | act 100 (0.75) |
| A87 | 0x57 | 0x42d850 | self `flags18 |= 0x400` |  | act 109 (0.89) |
| A88 | 0x58 | 0x42d710 | `0x4b2378 = (s8)X*100 + (s8)Y` |  | — |
| A89 | 0x59 | 0x42d860 | `S[Y] = (signed angle from self yaw to player) >> 4` |  | — |
| A90 | 0x5a | 0x42d8f0 | camera flags `&= ~0x300` (end scripted camera) |  | act 115 (0.67) |
| A91 | 0x5b | 0x42d730 | `0x4b2378 = X<<8 | Y` |  | act 117 (0.62) |
| A92 | 0x5c | 0x42d900 | death-state id override `0x4b39d8 = X` (used by A35 / death) |  | act 126 (0.89) |
| A93 | 0x5d | 0x42d910 | title save-slot icons via overlay scripts 0x2e..0x49 (slot mask `0x4b28e4`, G[X]) |  | — |
| A94 | 0x5e | 0x42da70 | camera flags `|= 1` |  | act 140 (0.67) |
| A95 | 0x5f | 0x42da80 | camera flags `&= ~1` |  | act 141 (1.00) |
| A96 | 0x60 | 0x42da90 | camera flags `= 0x10` |  | t3 12 (1.00) |
| A97 | 0x61 | 0x42daa0 | player `flags14 &= ~9` |  | — |
| A98 | 0x62 | 0x42dab0 | camera flags `|= 0x20` |  | — |
| A99 | 0x63 | 0x42dac0 | camera flags `&= ~0x20` |  | — |
| A100 | 0x64 | 0x42dae0 | save-slot menu cursor G[X] (skips empty slots, raw ↑/↓) |  | — |
| A101 | 0x65 | 0x42dad0 | camera flags `|= 4` |  | — |
| A102 | 0x66 | 0x42dbe0 | menu item Y ++ (wrap) |  | — |
| A103 | 0x67 | 0x42dc20 | menu item Y −− (wrap) |  | t4 8 (0.60) |
| A104 | 0x68 | 0x42dc50 | menu item Y = X (bounded) |  | — |
| A105 | 0x69 | 0x42dc90 | menu item Y max = X (<0x14) |  | — |
| A106 | 0x6a | 0x42dcd0 | resolution list step down |  | — |
| A107 | 0x6b | 0x42dd50 | resolution list step up |  | — |
| A108 | 0x6c | 0x42de30 | menu item Y = index of current resolution (config W/H 0x9ca830/34) |  | — |
| A109 | 0x6d | 0x42de70 | menu item Y link = X |  | t4 14 (0.86) |
| A110 | 0x6e | 0x42de90 | menu item Y link value = X |  | t4 15 (0.86) |
| A111 | 0x6f | 0x42deb0 | menu item Y = fullscreen (config 0x9ca838) |  | — |
| A112 | 0x70 | 0x42def0 | menu item Y = fogDist (0x9ca83c) |  | t4 16 (0.92) |
| A113 | 0x71 | 0x42df10 | menu item Y = renderer (0x4ac094 2→0, 0→1, 1→2) |  | — |
| A114 | 0x72 | 0x42df70 | apply resolution from item Y → 0x4b3e60/64 |  | — |
| A115 | 0x73 | 0x42dfc0 | `0x4b3e68 = item Y` |  | t4 19 (0.83) |
| A116 | 0x74 | 0x42dfe0 | `0x4b3e6c = item Y` (forced 1 when renderer != 2) |  | — |
| A117 | 0x75 | 0x42e030 | `0x4b3e70 = item Y` |  | t4 21 (0.83) |
| A118 | 0x76 | 0x42e050 | apply video mode (resolution search) |  | — |
| A119 | 0x77 | 0x42e1c0 | clamp item X to ≤5 if item Y set |  | — |
| A120 | 0x78 | 0x42e210 | `0x4b3fc0 = 1` |  | t4 23 (0.57) |
| A121 | 0x79 | 0x42e220 | `0x4b3fc0 = 0` |  | t4 26 (0.67) |
| A122 | 0x7a | 0x42e230 | volume `0x9ca854` (config+0x34) +5 (wrap at 95→0); WriteConfig |  | — |
| A123 | 0x7b | 0x42e260 | same −5 (below 51 → 100) |  | — |
| A124 | 0x7c | 0x42e290 | volume `0x9ca850` (config+0x30) +10 (wrap) |  | — |
| A125 | 0x7d | 0x42e2c0 | same −10 |  | — |
| A126 | 0x7e | 0x42e2f0 | item Y = vol34/5 − 10 |  | — |
| A127 | 0x7f | 0x42e320 | item Y = vol30/10 |  | — |
| A128 | 0x80 | 0x42e350 | quit dialog (`0x45f2b0 = 0x14`) |  | t4 27 (1.00) |

## 6. TB

*Observed* (ENGINE_COMPARISON.md): TB dispatches inline in `0x45d7a0` with tables cond `0x4cd560` (83), act `0x4cd6ac`
(142), extra `0x4cd8e4` (14) and `0x4cd91c` (31). The "TB slot" column above lists (a) the positional alignment from
`tools/ghidra/align_logic_tables.py` (`ghidra/diff/out/logic_align.tsv`, similarity in parentheses) and (b) TB table
slots holding the function paired with the LiT handler by `match_functions.py` (`pairs.tsv`, method in brackets).
Opcode numbers are game-specific; every TB opcode still needs semantic verification. TB-specific opcodes
(character/co-op): see GAMEPLAY.md §9.

## 7. Reimplementation notes

* Keep `S[256]` global per level (cleared on level load — *inferred*, the loader re-initialises the level arena)
  and `G[256]` in the save. Use wrapping 8-bit arithmetic; route every G write through one helper that maps index 7
  to the 16-bit golden-carrot counter `G[7] | G[0xfc] << 8` (inc/dec/add/sub/incgg; `movgi`/`clrg` do *not* special-case it).
* Timers are in 30 Hz ticks (`/30` seconds, `/1800` minutes). `rand()` = CRT `rand` 0x450810 (*observed*: seed `0x4ae610`,
  `x = x*0x343fd + 0x269ec3; return (x >> 16) & 0x7fff`) — needed for bit-exact random gates.
* Front-end/options menus (C92–C98, A93, A100–A128) are implemented in level logic of the title/option levels and
  poke `config.pc` fields directly; a native front end may replace them but must keep their side effects.

## 8. Residual unknowns

* C69 tail (decompile truncated), A24 tail, exact bit map of C82/C83's B byte beyond bit 0.
* 0x44e310 overlay-script interpreter `0x44d100` (scripts at `0x4b3a04`; source chunk not identified).
* Semantics of 0x35 node fields `+8..+0xf`, 0x34 step fields; `0x4377a0` exact role.
* Player flag names (flags14/18 bits) — see GAMEPLAY.md §3.
