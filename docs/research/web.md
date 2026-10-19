# Web research: Bugs Bunny: Lost in Time (LiT) and Bugs Bunny & Taz: Time Busters (TB)

Collected 2026-10-18. Every claim has a URL. Confidence: **High** = primary/source page read directly; **Med** = secondary or single source; **Low** = inference or snippet only.
Items marked **[LOCAL]** were checked against local files in `/mnt/1st4TB/BugsBunny/`, not the web. Items marked **[INFERENCE]** are my own reading, not something a source states.

Fetch notes:
- PCGamingWiki article HTML rendered empty, and `action=raw` returned HTTP 403. The wikitext came through the MediaWiki API instead (`/w/api.php?action=parse&prop=wikitext`).
- MobyGames returned HTTP 403. Credits came from Wayback Machine snapshots.
- IGN returned HTTP 403. Only the search snippet was used.

---

## 1. PCGamingWiki

### LiT: https://www.pcgamingwiki.com/wiki/Bugs_Bunny:_Lost_in_Time
(wikitext: https://www.pcgamingwiki.com/w/api.php?action=parse&page=Bugs_Bunny:_Lost_in_Time&prop=wikitext&format=json)
- Developer: Behaviour Interactive. Publisher: Infogrames. Engine field: empty. Windows release: Oct 19, 1999, citing the Infogrames press release https://web.archive.org/web/20010804151033/http://www.infogrames.net/corporate/press_archive/101999_bblitpcships.asp. **High**
- Retail disc only, with a "simple disc check". **High**
- Config file: `<game>\bin\config.pc`. Saves: `<game>\bin`. **High**
- Video:
  - Widescreen, ultrawide, 4K, FOV and 60/120 FPS are listed as "hackable via BBLIT Remaster".
  - "Locked at 30 FPS by default".
  - Windowed: true. Borderless: false. AA: hackable (Remaster). Vsync: false. **High**
- Input:
  - Key remapping supported.
  - Mouse not used for gameplay.
  - Controller: "DirectInput only", and you cannot pause with the controller. **High**
- Audio:
  - Separate volume for music and SFX.
  - No surround and no subtitles.
  - Mutes when focus is lost: "always on". **High**
- API: OpenGL = true and software mode = true (refcheck Eddman 2025-01-26). DirectDraw and Direct3D are not listed. 32-bit exe. **High**
- Languages: EN/NL/FR/DE/IT/ES. Brazilian Portuguese (interface only). Portuguese fan translation by rubinho146. Russian bootleg by CITY. **High**
- System requirements:
  - Minimum: Win95/98/NT SP3, P166, 16 MB RAM, 4 MB VRAM, DirectX 5.0.
  - Recommended: P-MMX 200, 32 MB RAM. **High**
- Essential improvement: BBLIT Remaster (see §4). **High**

### TB: https://www.pcgamingwiki.com/wiki/Bugs_Bunny_%26_Taz:_Time_Busters
(wikitext: https://www.pcgamingwiki.com/w/api.php?action=parse&page=Bugs_Bunny_%26_Taz:_Time_Busters&prop=wikitext&format=json)
- Developer: Artificial Mind & Movement. Publisher: Infogrames. Windows release: Dec 8, 2000 (no ref). WineHQ app id 17087. **High**
- Config and save locations: blank (unknown) on PCGW. **High**
- Video:
  - Widescreen and 4K are "hackable" via community file 1705 (§3). Multimonitor and ultrawide: false.
  - Windowed: false.
  - "Locked at 30 FPS." **High**
- Input: key remap supported. "DirectInput only". Cannot pause with the controller. **High**
- Local multiplayer for 2 players. No LAN or online. **High**
- Issue fixed, "Crash on launch":
  - Run the game as admin, or use DDrawCompat (https://github.com/narzoul/DDrawCompat): `DDraw.dll` next to `bblit2.exe` plus `DDrawCompat.ini` containing `DesktopResolution = 1024x768` and `DisplayResolution = 1024x768`.
  - Editor note: "what the game requires admin mode for is to set a safe screen resolution … 1024x768". **High** (editor claim, Med)
- API: OpenGL = true and software mode = true (refcheck Eddman 2025-01-26). **High**
- System requirements:
  - Minimum: P233, 32 MB RAM, "OpenGL compatible" GPU with 4 MB VRAM, DirectX 7.
  - Recommended: Win98, P266, 8 MB VRAM.
  - Explicitly unsupported GPUs: G100MGA100A, Jaton Riva 128ZX, STB Velocity 128, SIS 6326, PowerVR 2, Matrox G200, ATI Rage Pro Turbo. **High**
- Languages: EN/NL/FR/DE/IT/ES. Brazilian Portuguese was a "Promotional only from C&A in 2000" release. Portuguese fan translation by rubinho146. Russian bootleg by Fargus. **High**

## 2. WSGF

### LiT: https://www.wsgf.org/dr/bugs-bunny-lost-time/en
Grades: Bronze widescreen, ultrawide and 4K. Release 30 Sep 1999. "Tested Version: Retail, Demo". **High**

The page says:
- Stock resolutions are 320x240, 512x384, 640x480, 800x600 and 1024x768.
- Custom resolutions are set with `bugs.exe` (bin folder) arguments `/x` (width), `/y` (height), `/win`, `/skip_intro` and `/opengl` ("3D acceleration mode").

Hex edit:
- "Open 'BugsBunny.exe' in HxD … 1024x768. In hex it is `00 04 00 03` … change to … 720p `00 05 D0 02`, 1080p `80 07 38 04`".
- The resolution is stored as LE u16 width then LE u16 height, in the launcher `BugsBunny.exe` (not `bugs.exe`).
- On its own this "will only stretch the game without the cheat table". **High**

Cheat Engine:
- Horizontal FOV, default 4:3 = **144**. Use 50 for 21:9, 90 for 16:9 and 100 for 16:10.
- "Horizontal and Vertical stretch", default H=50 and V=37. Use H=40/V=37 for 16:9 and H=42/V=37 for 16:10.
- "There might be pop-in due to culling. Particles are still stretched." **High**

### TB: https://www.wsgf.org/dr/bugs-bunny-taz-time-busters/en
Submitted by **Pixelise** on 11 Feb 2019 and updated by skipclarke on 24 Mar 2022, per the search snippet for the same URL. Grades: Bronze widescreen, Hor+. HUD is stretched. Developer is listed as "Infogrames", which is wrong (see §6). **High**

- The same stock resolutions and the same arguments apply to `bblit2.exe` (in the bin folder): `/x /y /win /skip_intro /opengl`. **High**
- Cheat Engine table from the PCGW community files:
  - Horizontal FOV, default 4:3 = **50**. Use 25 for 21:9, 35 for 16:9 and 37 for 16:10.
  - "There might be pop-in due to culling. Particles are still stretched." **High**
- The page has no hex-edit addresses for TB. **High**

### Earlier LiT attempt (same author)
https://assemblergames.org/viewtopic.php?t=5775: **PixeliseGamePL**, Jan 2018.
- "I found values in RAM that change FOV and Zoom … (I've tried searching for values in game file but to no success)". Menu FOV is normally 91, and was edited to 50.
- On Jan 17, 2018 they released a Cheat Engine table on mega.nz: "I didn't figure out how to make widescreen permament". **High**

## 3. PCGW community files

### LiT Widescreen fix 1.0.0, file 2137
https://community.pcgamingwiki.com/files/file/2137-bugs-bunny-lost-in-time-widescreen-fix/ **High**
- Author: **killer-m**. Updated 2021-03-10. Tags: 16:9, 21:9, 48:9, ultrawide.
- Features: "HOR+ FOV".
- Install: copy all files to `\Bugs Bunny Lost In Time\bin\`, then edit `Settings.ini`:
  - `[RESOLUTION] Width=1920 Height=1080` forces the resolution.
  - `Fullscreen=1`.
  - `Size1=2.0 Size2=1.0` forces the font size.
- This is a DLL/ini drop-in, not a Cheat Engine table. The archive contents were not inspected.
- No comments are visible on the page.

### TB widescreen fix, file 1705
https://community.pcgamingwiki.com/files/file/1705-bugs-bunny-taz-time-busters-widescreen-fix/ **High**
- Uploader: **WSGF** (the PCGW account mirroring WSGF files). The og:updated_time is 2019-12-01.
- The full description is: `password = "pcgw"` and "for usage details, visit the WSGF - http://www.wsgf.org/dr/bugs-bunny-taz-time-busters/en".

## TB widescreen fix (details)

**Password:** `pcgw` is published on the file page (the URL above, in both the meta description and the body). **High**

[LOCAL] Verified locally:
- `Time Busters Widescreen V1.7z` is really a RAR v4 archive with the `.7z` extension. It is encrypted (`Encrypted = +`, method m5:17).
- `7z t -ppcgw` reports "Everything is Ok". A wrong password fails with "Data Error in encrypted file".
- It contains one file, `Time Busters Widescreen V1.CT`, 714 bytes, dated 2019-02-11 10:10:19. That matches the WSGF submission date of 11 Feb 2019, so the author is very likely Pixelise. **Med**

[LOCAL] What the fix patches: it is a **Cheat Engine table** (CheatEngineTableVersion 18). It does not modify the exe on disk. It has two entries, both `Byte`, with static absolute addresses:

| Description | Address |
|---|---|
| "Horizontal" | `0x00639A8D` |
| "Vertical" | `0x00639A8F` |

[LOCAL] Both addresses fall in `bblit2.exe`'s `.data` section, in its uninitialized tail:
- Image base is 0x400000. `.data` has VA 0x484000, VirtualSize 0x1B949C and RawSize 0x4D000.
- So these are runtime globals, not code bytes.
- They are odd-aligned single bytes, so they are probably fields of a larger struct. [INFERENCE] **Med**

Instructions (from WSGF): attach Cheat Engine to the running game and set Horizontal to 35 for 16:9 (4:3 default is 50). Launch with `/x /y` for a custom resolution. **High**

**User reports of broken object spawns or challenges: none found published.**
- The file-1705 page shows empty "User Feedback / Recommended Comments" and no comments.
- WSGF only warns of "pop-in due to culling" and stretched particles.
- I also searched the web for "Time Busters" widescreen together with spawn, disappear and pop-in, and found nothing.
- So the claim cannot be backed with a public quote.

[INFERENCE] There is a plausible mechanism, from the LiT engine research (**Med** for LiT, **Low** for transfer to TB):
- AleMastroianni/BBLIT-Findings #294 and #297 (https://github.com/AleMastroianni/BBLIT-Findings/blob/main/FINDINGS.md) read in the LiT code that object culling flag `0x20` is a screen test against a fixed PlayStation **512×240** screen (sx in ±256, sy in ±120), whatever the PC resolution.
- A hidden object without the keep-running bit `0x40` (or `0x80` for the screen test) "runs no rules, does not move, and its clock stops". By the same reading it "is not solid either".
- So an FOV hack that changes the projection used by this test would change which objects simulate. That could break spawns and timed challenges.
- BBLIT-Findings marks this as read in the code and **not tested in the game**.

## 4. Mods and other sites

- **ModDB**: the search found no ModDB page for either game. "Not found" means a web_search query returned nothing; ModDB was not crawled directly. **Med**
- **BBLIT Remaster** (LiT only), https://github.com/InLineR495/BBLIT-Remaster, MIT, v1.0-beta. **High**
  - A `ddraw.dll` proxy plus `remaster.ini`. It only works in the engine's OpenGL mode.
  - Run as `bugs.exe /opengl /win /b0 /x800 /y600`.
  - Supports only unpatched `bugs.exe` v1.6: 772,096 bytes, SHA-256 `7342f724…512139`, `.text` CRC32 `ecffb7c9`.
  - Proton needs `PROTON_NO_NTSYNC/FSYNC/ESYNC=1`: "with Wine's newer synchronisation primitives the game freezes, and so does the unmodded original".
  - Notes on the original game:
    - It is hard-locked to 30 fps, and "position, camera and animation advance once per frame, so raising the cap just speeds the game up".
    - The remaster keeps logic at 30 ticks/s and interpolates rendering.
  - Notes on the PC port:
    - It "divides vertex colours by 255, while the PS1 treats 0x80 as neutral", so the PC world is about half as bright.
    - PS1 transparency was replaced by "alpha fakes".
    - Pushable-stone collision offset is "a bug [that] dates back to the PS1 version".
  - Startup without arguments "can enter startup paths requiring resources absent from some distributions (such as `LANGUAGE.BZE`)".

## 5. Other projects found (RE / tools / reimplementations)

| Project | URL | One-line summary | License |
|---|---|---|---|
| BugsDecomp | https://github.com/quantumdude836/BugsDecomp | LiT PC decompilation. DLL injection patches functions over the original. Supports `Bugs.exe` 1.0 (MD5 59731F03…) and 1.6 (MD5 121DB63D…). Has `gte.c` ("High-level emulation of PS1's GTE API", Q12 fixed point), `rogl.c`, `cfg.c`, `track.c`. Docs `doc/bze.md` (BZE header, section IDs 1=load instructions, 3=textures/SFX/text, 4=models, LZ-like compression) and `doc/internals.md` (full CLI flags, 128-byte `config.pc` layout). Last push 2024-04. | **none stated** (all rights reserved by default) |
| BBLIT Remaster | https://github.com/InLineR495/BBLIT-Remaster | LiT `ddraw.dll` renderer replacement with interpolation, Hor+, XInput and tools (`dump_objects.py`, `verify_exe.py`). | MIT |
| bblit-remake | https://github.com/Fr3d-/bblit-remake | LiT reimplementation in Rust + Bevy. IDA analysis of v1.6 is done, "every file on the disc parses", and the game is playable through the hub and levels. | none stated |
| Bugs-bunny-lost-in-time-reverse-engineered | https://github.com/Ombelll/Bugs-bunny-lost-in-time-reverse-engineered | Prose RE docs for LiT, 255 proven findings: container, LZ77 variant, load script, object/behaviour machine, models, 30-tick object clock (finding 233). States BZE section 2 = "PSX MIPS code (unused on PC)". | none stated |
| bbtrm (Ombelll) | https://github.com/Ombelll/bbtrm | Linked from Ombelll's README as "*Time Busters* … tooling, format research, and a Godot rebuild in progress". **The GitHub API returns 404 (private or deleted) as of 2026-10-18.** | n/a |
| BBLIT-Findings | https://github.com/AleMastroianni/BBLIT-Findings | LiT findings 256–374, with PC (`bugs.exe` 1.0) and PS1 (SLUS_008.38) addresses: culling, portals, collision, saves, cheats. | none stated |
| BBLIT-Viewer | https://github.com/AleMastroianni/BBLIT-Viewer | LiT level viewer for the PC `.bze` files. Includes `bze.py` (unpack and decompress), `tim.py` (TIM to PNG) and an OBJ exporter. | GPL-3.0 |
| bblit (talaria0101) | https://github.com/talaria0101/bblit | Ghidra dump of `BUGS.EXE` (576 functions) and `HV3DFX.DLL`, plus `RE_NOTES.md`: renderers, build paths, disc check, BZE/XA notes, `bze_parse.py`. | none stated |
| BuggyBunny | https://github.com/hadashisora/BuggyBunny | C# unpacker/repacker for **PS1** `.bzz` archives (uses Denim's toolBZZ). Assets are TIM images and VAG audio. 2019. | none stated |
| BugsBunnyLostinTimeRecomp | https://github.com/PeriBluGaming/BugsBunnyLostinTimeRecomp | Static recompilation of **PS1** LiT (USA, SLUS-00838) to a native PC exe using PSXRecomp. | none stated |
| BugsBunnylitAPWorld / Poptracker | https://github.com/SynthKat67/BugsBunnylitAPWorld , https://github.com/SynthKat67/Bugs-Bunny-Lost-in-Time-Poptracker | Archipelago randomizer for LiT via BizHawk, which implies the PS1 version. | none / GPL-3.0 |
| DDrawCompat | https://github.com/narzoul/DDrawCompat | Generic DirectDraw shim recommended by PCGW for the TB launch crash. | 0BSD (GitHub API) |

Not found:
- No public RE, decompilation or tooling project for **Time Busters** / `bblit2.exe`. Checked: GitHub repo search for "time busters", "bblit2", "bbtrm", "timebusters", "bugs taz"; GitHub code search for `bblit2` (which only hit a Chinese game-intro page https://github.com/chiuinan/chiuinan.github.io/blob/master/game/game/intro/eng/e52/bblit2.htm); and web searches.
- No Xentax, ZenHAX or ResHax threads on `.bze` or `.xa` turned up in web search. Xentax is offline and was not crawled.

## 6. Developer attribution and engine lineage

- LiT developer is **Behaviour Interactive**:
  - https://en.wikipedia.org/wiki/Bugs_Bunny:_Lost_in_Time
  - MobyGames https://www.mobygames.com/game/2181/bugs-bunny-lost-in-time/ (via https://web.archive.org/web/2024/https://www.mobygames.com/game/2181/bugs-bunny-lost-in-time/)
  - **High**
- TB developer is **Artificial Mind & Movement (A2M)**:
  - https://en.wikipedia.org/wiki/Bugs_Bunny_%26_Taz:_Time_Busters
  - MobyGames https://web.archive.org/web/2024/https://www.mobygames.com/game/3477/bugs-bunny-taz-time-busters/
  - IGDB https://www.igdb.com/games/bugs-bunny-taz-time-busters
  - **High**
- Publisher for both is Infogrames (Europe SA / North America / Inc.), per the same MobyGames pages. **High**
- **They are the same studio under different names.** Behaviour Interactive's Wikipedia page (https://en.wikipedia.org/wiki/Behaviour_Interactive) says:
  - Former names: Megatoon (1992–1997) and Artificial Mind and Movement (2000–2010).
  - "In 1999, Racine and some investors bought the studio back, but they had to rename it, changing the name in 2000 to Artificial Mind and Movement Inc. (A2M)".
  - It renamed back to Behaviour Interactive on Nov 8, 2010.
  - **High**
- Shared staff (Wikipedia infoboxes plus MobyGames):
  - Lead programmer **Martin Ross** on both. MobyGames lists "Lead Programming: Martin Ross" for both.
  - Producer **Denis Lacasse** on both.
  - Designer **Claude Pelletier**: LiT director and lead design, TB art director and lead designer.
  - Artist Martin Dubeau and composer Gilles Léveillé on both.
  - LiT also credits "3D / Graphics Programming: Stéphane Joncas".
  - The MobyGames TB credits page says people credited on TB were also credited on "Bugs Bunny: Lost in Time, a group of 6 people" (of 7 TB credits). https://web.archive.org/web/2024/https://www.mobygames.com/game/3477/bugs-bunny-taz-time-busters/credits/windows/
  - **High**
- Wikipedia says TB's gameplay "plays much the same as *Bugs Bunny: Lost in Time*". https://en.wikipedia.org/wiki/Bugs_Bunny_%26_Taz:_Time_Busters **Med**
- Ombelll calls TB "A related PlayStation title on the same engine family". https://github.com/Ombelll/Bugs-bunny-lost-in-time-reverse-engineered **Med** (no proof shown)
- [LOCAL] `bblit2.exe` contains the window class `BBLIT Game`, `..\bin\config.pc`, `..\bin\Savegame%d.dat` and PSX-style ISO paths `..\BZE\<NAME>.BZE;1` (e.g. `GRANNY.BZE;1`, `ARCASG01.BZE;1`).
  - talaria0101's notes list the same strings for LiT `BUGS.EXE`: "BBLIT Game", `..\bin\config.pc`, `..\bin\Savegame%d.dat`, `..\BZE\...BZE;1`. https://github.com/talaria0101/bblit/blob/main/RE_NOTES.md
  - So the name "bblit2" (= BBLIT 2) is backed by a shared window class, config path, save naming and BZE container. **High** for shared lineage. **Med** for "same engine with modifications" (code diff not done here).
- No interview or engine name was found.

### PS1 heritage of the PC version
- LiT shipped on PS1 first (EU June 1999, NA July 1, 1999), then PC (NA Oct 19, 1999). https://en.wikipedia.org/wiki/Bugs_Bunny:_Lost_in_Time **High**
- TB also shipped on both PS1 and PC in 2000. Wikipedia gives PS1 NA/EU Nov 24, 2000 and PC NA "September 2000", EU Dec 8, 2000. **High**
- Evidence that the PC code is a PS1 port:
  1. BugsDecomp `gte.h` says "High-level emulation of PS1's GTE (Geometry Transform Engine) API". It uses Q12 fixed point, with `RotMatrix`, `rsin`/`rcos`, `ratan2`, `SquareRoot0` at bugs.exe 0x407a00 and nearby. https://github.com/quantumdude836/BugsDecomp/blob/main/BugsDecomp/gte.h **High**
  2. BZE section 2 = "PSX MIPS code (unused on PC)". https://github.com/Ombelll/Bugs-bunny-lost-in-time-reverse-engineered **Med**
  3. Textures are PS1 TIM. https://github.com/AleMastroianni/BBLIT-Viewer **High**
  4. The screen-space culling uses the PS1 512×240 screen. BBLIT-Findings #297 (code reading). **Med**
  5. Vertex colour semantics: 0x80 is neutral on PS1, while the PC port divides by 255. https://github.com/InLineR495/BBLIT-Remaster **Med**
  6. The exe references `..\BZE\*.BZE;1` ISO version suffixes, `MUSIC.XA` and `SPEECHES.XA`. "Windows port of the Behaviour Interactive PS1 platformer". https://github.com/talaria0101/bblit/blob/main/RE_NOTES.md **Med**

## Facts relevant to engine reimplementation
1. **Fixed 30 Hz simulation.** Logic, camera and animation advance once per frame, and uncapping speeds the game up. Interpolate rendering, not logic. (BBLIT Remaster README; Ombelll finding 233; PCGW "Locked at 30 FPS" for both games.) **High**
2. **Culling gates simulation (LiT).** Objects hidden by the distance, area or screen tests without the keep-running bit freeze, and per the code reading they are not solid. The screen test uses a fixed 512×240 PS1 viewport. A widescreen reimplementation must keep this *logical* visibility test independent of the render FOV, or behaviour changes. This likely relates to the TB widescreen side-effects; that link is unproven. (BBLIT-Findings #294/#297.) **Med**
3. **Math is PS1 GTE style.** Q12 fixed point, angle 4096 = 360°, rotation order Rx·Ry·Rz, scale 4096 = 1.0. (BugsDecomp gte.h; Ombelll findings 32, 58, 74.) **High**
4. **Renderers.** Software 8-bit and 24-bit blitter (`pcrsoft8.c`), OpenGL (`Pcrogl.c`, dynamic `opengl32.dll` / `/dll:`), and a DirectDraw path for display. No Direct3D import. 3Dfx is handled by the bundled `HV3DFX.DLL` OpenGL mini-ICD. Audio uses DirectSound. (talaria0101 RE_NOTES; PCGW API: OpenGL + software.) **Med**
5. **Command-line flags** (shared by `bugs.exe` and `bblit2.exe` per WSGF): `/x /y /win /full /opengl|/ogl /soft* /skip_intro /no_sync /no_skip /b<levelID> /dll:`. (BugsDecomp internals.md; WSGF.) **High**
6. **`config.pc`** is a 128-byte struct loaded as-is (version 0x10, renderer, language, width, height, fullscreen, fog, volumes, joystick and key maps). Saves are `..\bin\Savegame%d.dat` and `Savedata.dat`. Paths are relative to `bin`. (BugsDecomp internals.md; talaria RE_NOTES; PCGW.) **High**
7. **The BZE container is 2048-aligned with an LZ-compressed section per ID.** The sources disagree on the header field order:
   - BugsDecomp: `{id, actual size, file size}` plus a checksum at 0x7FC.
   - talaria: `{raw_size, aligned_size, chunk_id}`.
   - Verify against data; the CoreFormats/AssetFormatsRE peers own this. **Med**
8. **Audio.** TRACK*.XA and SPEECH*.XA are XA-ADPCM streams without CD sector framing, played by the in-game streamer and never through MCI. Per-level sound banks are AIFFs. (talaria; Ombelll.) **Med** (sources partly conflict: Ombelll rejects "track/*.xa is PSX XA-ADPCM")
9. **Startup requirements (LiT).**
   - Registry `HKCU\Software\Infogrames\Bugs Bunny Lost In Time` with the install path.
   - A CD drive labelled `BBLIT`, with a fallback file lookup on the CD.
   - Single instance via `FindWindowA("BBLIT_Game")`.
   - Not needed for a native reimplementation; only the asset lookup order matters. (talaria RE_NOTES.) **Med**
10. **The TB widescreen state is two bytes in `.data` BSS:** `0x639A8D` (H) and `0x639A8F` (V). Default H = 50 at 4:3, and 35 gives 16:9. Useful as Ghidra xref anchors for the TB projection setup. [LOCAL] + WSGF. **Med**
11. **The LiT launcher `BugsBunny.exe` stores its default resolution as LE u16 pairs** (`00 04 00 03` = 1024x768). LiT CE stretch defaults are H=50, V=37 and FOV 144 (WSGF). **High**
12. **Known PC-port defects to fix or not replicate:** half-brightness vertex colours, alpha-faked PS1 transparency, cliff cracks, and the pushable-stone collision offset (also on PS1). (BBLIT Remaster.) **Med**
13. **TB crashes on modern Windows without admin rights or DDrawCompat**, because of display-mode setup at 1024x768. (PCGW TB.) **Med**
