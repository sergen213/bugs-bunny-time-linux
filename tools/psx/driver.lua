-- PCSX-Redux driver for tools/psx/run.sh (all options arrive as PSX_* environment variables).
-- Counts GPU vsyncs (60/s NTSC); the pad plan and the dump frame are in vsyncs.
local env = function(k, d) local v = os.getenv(k); if v == nil or v == '' then return d end; return v end
local frames = tonumber(env('PSX_FRAMES', '600'))
local out = env('PSX_OUT', '.')
local shotEvery = tonumber(env('PSX_SHOT_EVERY', '0'))  -- also write shot_<vsync>.ppm every N vsyncs
local keys = env('PSX_KEYS', '')
local pad = PCSX.SIO0.slots[1].pads[1]
local B = PCSX.CONSTS.PAD.BUTTON
local names = { U = 'UP', D = 'DOWN', L = 'LEFT', R = 'RIGHT', X = 'CROSS', O = 'CIRCLE', S = 'SQUARE', T = 'TRIANGLE',
                ST = 'START', SE = 'SELECT', L1 = 'L1', L2 = 'L2', R1 = 'R1', R2 = 'R2' }

-- PSX_KEYS: "ST*1,_*90,X+U*30" = segments of BUTTONS*VSYNCS; buttons joined by '+', '_' = nothing held.
local plan = {}
for seg in keys:gmatch('[^,]+') do
  local b, n = seg:match('^(.-)%*(%d+)$')
  assert(b, 'PSX_KEYS segment needs BUTTONS*N: ' .. seg)
  local set = {}
  for k in b:gmatch('[^+]+') do
    if k ~= '_' then set[#set + 1] = B[assert(names[k], 'unknown button ' .. k)] end
  end
  plan[#plan + 1] = { set = set, n = tonumber(n) }
end

local held = {}
local function hold(set)
  for _, b in ipairs(held) do pad.clearOverride(b) end
  for _, b in ipairs(set) do pad.setOverride(b) end
  held = set
end

local function writeFile(path, data) local f = assert(io.open(path, 'wb')); f:write(data); f:close() end

-- PSX_TRACE="bx=0x8006ad30:s32,pos=0x8006ad30:s32*3" -> trace.csv, one row per vsync after the state/boot
-- (vsync, then each value; *N = N consecutive values). Types u8 s8 u16 s16 u32 s32. Names from tools/psx/syms_*.txt.
local CT = { u8 = 'uint8_t', s8 = 'int8_t', u16 = 'uint16_t', s16 = 'int16_t', u32 = 'uint32_t', s32 = 'int32_t' }
local trace, header = {}, { 'vsync' }
for spec in env('PSX_TRACE', ''):gmatch('[^,]+') do
  local name, addr, ty, n = spec:match('^([%w_]+)=0x(%x+):(%a+%d+)%*?(%d*)$')
  assert(name and CT[ty], 'PSX_TRACE entry needs NAME=0xADDR:TYPE[*N]: ' .. spec)
  n = tonumber(n) or 1
  local off = bit.band(tonumber(addr, 16), 0x1fffff)
  local p = ffi.cast(CT[ty] .. '*', ffi.cast('uint8_t*', PCSX.getMemPtr()) + off)
  trace[#trace + 1] = { p = p, n = n }
  for i = 1, n do header[#header + 1] = n > 1 and (name .. (i - 1)) or name end
end
local traceFile
if #trace > 0 then
  traceFile = assert(io.open(out .. '/trace.csv', 'w'))
  traceFile:write(table.concat(header, ','), '\n')
end
local function traceRow(n)
  local row = { n }
  for _, t in ipairs(trace) do for i = 0, t.n - 1 do row[#row + 1] = tostring(tonumber(t.p[i])) end end
  traceFile:write(table.concat(row, ','), '\n')
end

-- Displayed frame as PPM (15-bit BGR -> 24-bit RGB; 24-bit modes (bpp ~= 0) are written as-is).
local function shot(path)
  local s = PCSX.GPU.takeScreenShot()
  local w, h, data = s.width, s.height, tostring(s.data)
  if tonumber(s.bpp) == 0 then
    local px = {}
    for i = 0, w * h - 1 do
      local lo, hi = data:byte(2 * i + 1, 2 * i + 2)
      local v = lo + hi * 256
      px[i + 1] = string.char(bit.lshift(bit.band(v, 31), 3), bit.lshift(bit.band(bit.rshift(v, 5), 31), 3),
                              bit.lshift(bit.band(bit.rshift(v, 10), 31), 3))
    end
    data = table.concat(px)
  end
  writeFile(path, string.format('P6\n%d %d\n255\n', w, h) .. data)
end

local function dump()
  writeFile(out .. '/ram.bin', ffi.string(PCSX.getMemPtr(), 2 * 1024 * 1024))
  writeFile(out .. '/scratch.bin', ffi.string(PCSX.getScratchPtr(), 1024))
  local r = PCSX.getRegisters()
  print(string.format('psx: frame %d pc %08x', frames, r.pc))
  shot(out .. '/shot.ppm')
  local save = env('PSX_SAVE', nil)
  if save then
    local f = Support.File.open(save, 'TRUNCATE')
    f:writeMoveSlice(PCSX.createSaveState())
    f:close()
    print('psx: saved state ' .. save)
  end
end

-- PSX_LEVEL="LOADER:LEVELVAR:FROM:TO" (hex addresses, decimal indices): when the level loader at LOADER is entered
-- with a0 == FROM (once), load TO instead (a0 and the s32 current-level variable LEVELVAR both set; values in
-- tools/psx/syms_<game>.txt). run.sh starts the interpreter for this (breakpoints do not fire under the dynarec).
-- Armed after a PSX_LOAD state is in place (loading a state drops earlier breakpoints).
local function armRedirect()
  local redirect = env('PSX_LEVEL', nil)
  if not redirect then return end
  local addr, var, from, to = redirect:match('^0x(%x+):0x(%x+):(%d+):(%d+)$')
  assert(addr, 'PSX_LEVEL needs 0xLOADER:0xLEVELVAR:FROM:TO')
  from, to, var = tonumber(from), tonumber(to), bit.band(tonumber(var, 16), 0x1fffff)
  PCSX.settings.emulator.Debug.Debug = true
  local done = false
  LevelBreak = PCSX.addBreakpoint(tonumber(addr, 16), 'Exec', 4, 'psx level', function()
    local r = PCSX.getRegisters().GPR.n
    if not done and r.a0 == from then
      r.a0, done = to, true
      ffi.cast('int32_t*', ffi.cast('uint8_t*', PCSX.getMemPtr()) + var)[0] = to
      print(string.format('psx: level %d -> %d', from, to))
    end
  end)
end

-- A state is loaded once the booted emulator reaches its first vsync (loading before -run never resumes).
local load = env('PSX_LOAD', nil)
if env('PSX_LEVEL', nil) then PCSX.settings.emulator.Debug.Debug = true end  -- before the CPU starts
if not load then armRedirect() end
local n, step, left = 0, 1, plan[1] and plan[1].n or 0
if plan[1] then hold(plan[1].set) end
Listener = PCSX.Events.createEventListener('GPU::Vsync', function()
  if load then
    local path = load
    load = nil
    PCSX.nextTick(function()
      local f = Support.File.open(path, 'READ')
      PCSX.loadSaveState(f)
      f:close()
      print('psx: loaded state ' .. path)
      armRedirect()
    end)
    return
  end
  n = n + 1
  if shotEvery > 0 and n % shotEvery == 0 then shot(string.format('%s/shot_%05d.ppm', out, n)) end
  if traceFile then traceRow(n) end
  if plan[step] then
    left = left - 1
    if left <= 0 then
      step = step + 1
      hold(plan[step] and plan[step].set or {})
      left = plan[step] and plan[step].n or 0
    end
  end
  if n == frames then
    if traceFile then traceFile:close() end
    local ok, err = pcall(dump)
    if not ok then print('psx: dump failed: ' .. tostring(err)) end
    PCSX.quit(ok and 0 or 1)
  end
end)
