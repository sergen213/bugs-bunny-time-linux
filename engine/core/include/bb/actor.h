// LiT actor animation: Sony TOD decode, the animation player 0x4393e0 / frame applier 0x44e400 and rigid-part
// posing with slot-buffer skinning (0x40a050, 0x411350). Spec: docs/re/GAMEPLAY.md §6. Data types: bb/scene.h;
// buildLitScene fills Scene::actorModels / Scene::actors.
#pragma once
#include "bb/scene.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace bb {

// Parses a TOD (id 0x50, version 0) at d[off..]. Throws FormatError on malformed data.
Tod parseTod(std::span<const uint8_t> d, size_t off);

// One GsCOORDINATE2 part (pool entry 0x4b2240) as the frame applier leaves it.
struct TodPart {
    uint32_t id = 0;             // creating packet's id (parents and kills search by it)
    uint16_t flags = 0x8008;     // 0x8000 unposed/hidden, 8 no object, 4 has object, 0x40.. light nodes
    int32_t object = -1;         // 0-based TMD object, -1 = none
    int32_t parent = -2;         // part index into the part list; -1 = actor root, -2 = none (unattached)
    bool dead = false;           // killed (type 8, odd flag other than 0xb); kept so indices stay stable
    // Param: rot 4.12, scale Q12, trans units. The original does not clear reused pool entries; a fresh part
    // starts at scale 1 here (Inferred, only matters if an animation never sends scale for a part).
    std::array<int32_t, 3> rot{}, scale{4096, 4096, 4096}, trans{};
    std::array<float, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1}; // coord matrix, row major, 1.0 = 4096
};

// Mode bits of 0x4393e0 (pass a mode only when (re)starting/controlling; 0 on ordinary ticks).
inline constexpr uint32_t AnimLoop = 1, AnimOnce = 2, AnimResume = 4, AnimPause = 0x10, AnimStop = 0x20, AnimHold = 0x800;
// animTick return values.
inline constexpr uint32_t AnimPlaying = 0x200, AnimFinished = 0x100, AnimMissing = 0x400, AnimStopped = 0x20;

// Animation state actor+0x144 plus the actor's part list actor+0x78 and TOD bbox.
struct AnimState {
    std::vector<TodPart> parts;  // empty until the first animTick builds the skeleton
    const Tod* tod = nullptr;    // playing TOD (points into the ActorModel: keep the model alive and unmoved)
    uint16_t anim = 0;           // playing animation id (+0x158)
    uint16_t last = 0;           // nframes - 1
    uint16_t frame = 0;          // frame counter (+0x146)
    uint8_t ticksPerFrame = 0;   // resolution >> 1 (u8: 0 wraps to a 256-tick hold, as the original)
    uint8_t countdown = 0;
    uint32_t flags = 0;          // 0x100 finished, 0x200 playing, 0x1000 (re)started, 0x2000 last shown frame, 0x10 paused
    size_t next = 0;             // index of the next TOD frame the applier reads
    std::array<int16_t, 6> box{}; // TOD packet 9: actor box min xyz, max xyz (+0x48..+0x54), PS1 axes
    uint16_t radius = 0;         // TOD packet 9: +0x2e
};

// One 30 Hz tick of 0x4393e0 (TOD path; frame skip, sound and logic events not ported). Any of mode bits
// 0xb (re)starts `animId` (a role in the actor's tag-0x0f list) at frame 0. Each TOD frame is shown
// resolution >> 1 ticks, no interpolation; the last TOD frame is never applied. Loop wraps to frame 0;
// once returns AnimFinished on the tick its last shown frame expires (AnimPlaying instead with AnimHold) and
// then keeps returning it without changing the pose.
uint32_t animTick(const ActorModel& model, AnimState& st, uint16_t animId, uint32_t mode);

// Applies TOD frame `frame` to st.parts / st.box (0x44e400). Exposed for tests/tools.
void applyTodFrame(AnimState& st, const Tod& tod, size_t frame);

// Appends the posed model as engine-space meshes (Mesh::texture indexes Scene::textures). Root coordinate =
// Trans(pos) * RotMatrix(rot) * Scale(scale) in PS1 space (actor +0xe4 / +0xdc / +0xcc). Parts are drawn when
// they have an object, are not hidden (0x8000) and their world matrix is non-zero; blend slots are averaged in
// world space (the original averages after projection: approximation). Triangles touching unwritten slots are
// dropped.
void poseActor(const ActorModel& model, const AnimState& st, std::array<int32_t, 3> pos, std::array<int16_t, 3> rot,
               std::array<int32_t, 3> scale, std::vector<Mesh>& out);

// Slot positions of the current pose in PS1 axes with an identity root; unwritten slots are NaN. For tests.
std::vector<std::array<float, 3>> poseSlots(const ActorModel& model, const AnimState& st);

// Index of animation `animId` in model.anims, -1 if absent.
int findAnim(const ActorModel& model, uint16_t animId);

} // namespace bb
