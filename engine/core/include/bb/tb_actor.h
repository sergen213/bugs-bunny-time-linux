// Time Busters actors: chunk 0x22 tag 0x25 "packed TOD" animation (GAMEPLAY_TB.md §5, probe tools/re/tb_anim.py)
// and multi-object actor models (MODELS.md §10.2, slot-indexed primitives §5.3), posed with the shared LiT
// TOD player (bb/actor.h: animTick / poseActor; Tod::packed routes frames to applyTbFrame).
#pragma once
#include "bb/actor.h"
#include "bb/bze.h"
#include "bb/scene.h"

#include <span>

namespace bb {

// Packed TOD at d (header 0x50 0 res 0, u32 nframes). Packets keep the 16-bit header split as id = w & 0xff,
// type = w >> 8 & 0xf, flag = w >> 12, and their raw payload words in v[0..]. Throws FormatError.
Tod parseTbTod(std::span<const uint8_t> d);

// Frame applier 0x468280 (tb_anim.py apply_frame): packets address parts by 1-based list position; deleted parts
// are erased (later positions shift). Packet 0x900 writes st.box (min xyz, max xyz) / st.radius; 0xa00 (root
// motion probe) is not applied.
void applyTbFrame(AnimState& st, const Tod& tod, size_t frame);

// Role-13 actor model `model` -> ActorModel objects/meshes (textures resolved against s.textures by the
// "{level}/tex/{id}" names buildTbScene gives them; texture links show their start frame).
ActorModel buildTbActorModel(const Scene& s, std::span<const uint8_t> model);

// Adds every placed TB entity (chunks 0x07/0x0a/0x63) that links a role-1 rig and a role-13 model which
// buildTbScene did not already draw as a rigid prop to s.actorModels / s.actors (first link with a role wins,
// §5.1); anims = the entity's other packed TODs keyed by role. Initial anim = the anim of the first state of the
// event-2 (else event-1) transition (own + template records), else the first linked anim. Bugs (id 2) and Taz
// (id 3) get ActorInstance::player. Throws FormatError.
void addTbActors(Scene& s, const BzeFile& bze);

} // namespace bb
