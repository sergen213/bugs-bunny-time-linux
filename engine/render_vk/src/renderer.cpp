#include "bb/render_vk/renderer.h"

#include "fsr.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <format>
#include <glm/gtc/packing.hpp>
#include <numbers>
#include <limits>
#include <fstream>
#include <print>
#include <ranges>
#include <tuple>
#include <chrono>
#include <span>
#include <stdexcept>

namespace bb::render_vk {
namespace fs = std::filesystem;
namespace {

// SPIR-V compiled at build time by glslc (-mfmt=num), see CMakeLists.txt.
constexpr uint32_t kSceneVert[] = {
#include "scene.vert.inc"
};
constexpr uint32_t kSceneFrag[] = {
#include "scene.frag.inc"
};
constexpr uint32_t kSceneLitVert[] = {
#include "scene_lit.vert.inc"
};
constexpr uint32_t kSceneMapFrag[] = {
#include "scene_map.frag.inc"
};
constexpr uint32_t kSceneRtFrag[] = {
#include "scene_rt.frag.inc"
};
constexpr uint32_t kSceneMvVert[] = {
#include "scene_mv.vert.inc"
};
constexpr uint32_t kSceneMvFrag[] = {
#include "scene_mv.frag.inc"
};
constexpr uint32_t kSceneMapMvFrag[] = {
#include "scene_map_mv.frag.inc"
};
constexpr uint32_t kSceneRtMvFrag[] = {
#include "scene_rt_mv.frag.inc"
};
constexpr uint32_t kShadowVert[] = {
#include "shadow.vert.inc"
};
constexpr uint32_t kShadowFrag[] = {
#include "shadow.frag.inc"
};
constexpr uint32_t kUiVert[] = {
#include "ui.vert.inc"
};
constexpr uint32_t kUiFrag[] = {
#include "ui.frag.inc"
};

constexpr uint32_t kFrames = 2;
constexpr uint32_t kNoSlot = UINT32_MAX;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT; // ponytail: no D24 fallback; every desktop driver has D32
constexpr VkFormat kTexFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkBufferUsageFlags kStreamUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
// Extra usage for buffers that feed acceleration-structure builds and scene.frag's buffer references.
constexpr VkBufferUsageFlags kRtInput = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
enum : uint32_t { kSampNearest, kSampTrilinear, kSampUiNearest, kSampUiLinear, kSampLinear };
enum : uint32_t { kSlotWhite, kSlotFont }; // first two images created in init()

// Matches the push block in scene.vert/scene.frag.
struct Push3D {
    glm::mat4 viewProj;
    glm::vec4 viewZ, fogColorScale;
    glm::vec2 fog;
    float alphaRef;
    uint32_t flags, tex, samp;
    float texGamma;     // Settings::texGamma (1 = off)
    uint32_t material;  // lit variants only: GpuMesh::material
};
static_assert(sizeof(Push3D) == 128 && offsetof(Push3D, alphaRef) == 104);

// Enhanced sun shadows (docs/ROADMAP.md "Ray tracing"). Neither exe has a dynamic light (LiT draws blob shadows, TB
// none), so the sun is a choice made per level: 55° high, behind the level's opening camera (its yaw at the first
// drawScene after uploadScene) and 35° to its right, so the opening view is front-lit and shadows fall away from the
// camera, short and readable under the characters. ponytail: a per-level override table if some level's art
// contradicts it.
constexpr float kSunElevation = 0.96f, kSunSide = 0.6f; // radians
constexpr float kShadowBrightness = 0.55f; // shadowed surfaces keep 55% of their (baked, vertex-lit) colour
// Bugs-scale objects are a few hundred units tall (MODELS.md §3, both games); the effect sizes follow that unit.
constexpr float kCharUnit = 300.0f;
constexpr float kCascadeEnd[4] = {7 * kCharUnit, 25 * kCharUnit, 80 * kCharUnit, 250 * kCharUnit}; // view distance
constexpr uint32_t kCascades = 4, kShadowSize = 2048;
constexpr float kAoRadius = 0.7f * kCharUnit, kAoStrength = 0.65f;
constexpr float kRayOffset = 1e-3f; // shadow / AO ray origin pushed off the surface by this × view distance (+0.5)
// Rays ignore hits closer than this: the PS1 levels stack decal polygons (tracks, patches) a few units over the
// ground, which would otherwise shadow the ground under them. Costs contact shadows of objects thinner than ~10 units.
constexpr float kRayMin = 0.03f * kCharUnit;

// set 1 binding 0, std140 (scene.frag Light).
struct LightUbo {
    glm::mat4 cascade[4];
    glm::vec4 splits, texel, sun, eye, rt;
    uint32_t mode;
    float rayMin;
    uint32_t reflect, temporal;
    glm::mat4 curViewProj, prevViewProj; // motion vectors (FSR): unjittered
};
static_assert(sizeof(LightUbo) == 480);

// Smooth normals: faces meeting at a welded position are averaged (weighted by their corner angle) when they are
// within this angle of each other, so rounded models shade smoothly while box edges and creases stay hard.
const float kCreaseCos = std::cos(55.0f * std::numbers::pi_v<float> / 180.0f);
constexpr float kWeldGrid = 0.25f; // positions closer than this snap together (decoders keep game-unit coordinates)

// Per-vertex smooth normals of verts (triangles idx[first, first + count) offset by base, per range), packed as
// R8G8B8A8_SNORM. Vertices of no triangle get 0 (the shaders then use the face normal), as every vertex does with
// BBTIME_VK_FLAT_NORMALS=1 (before/after comparisons).
struct TriRange { uint32_t first, count; int32_t base; };
std::vector<uint32_t> smoothNormals(std::span<const Vertex> verts, std::span<const uint32_t> idx, std::span<const TriRange> ranges) {
    static const bool flat = [] { const char* e = std::getenv("BBTIME_VK_FLAT_NORMALS"); return e && *e && *e != '0'; }();
    if (flat) return std::vector<uint32_t>(verts.size(), 0); // comparison / debugging: face normals everywhere
    struct Corner { uint64_t key; uint32_t tri, vert; };
    std::vector<glm::vec3> faceN;
    std::vector<Corner> corners;
    std::vector<float> angle;
    auto pos = [&](uint32_t v) { return glm::vec3(verts[v].pos[0], verts[v].pos[1], verts[v].pos[2]); };
    auto key = [](glm::vec3 p) {
        const auto q = [](float f) { return uint64_t(uint32_t(int32_t(std::lround(f / kWeldGrid))) & 0x1fffff); };
        return q(p.x) | q(p.y) << 21 | q(p.z) << 42;
    };
    for (const TriRange& r : ranges)
        for (uint32_t i = r.first; i + 2 < r.first + r.count; i += 3) {
            const uint32_t v[3] = {uint32_t(int32_t(idx[i]) + r.base), uint32_t(int32_t(idx[i + 1]) + r.base), uint32_t(int32_t(idx[i + 2]) + r.base)};
            const glm::vec3 p[3] = {pos(v[0]), pos(v[1]), pos(v[2])};
            const glm::vec3 n = glm::cross(p[1] - p[0], p[2] - p[0]);
            const float len = glm::length(n);
            if (!(len > 1e-6f)) continue;
            const uint32_t tri = uint32_t(faceN.size());
            faceN.push_back(n / len);
            for (int k = 0; k < 3; ++k) {
                const glm::vec3 a = glm::normalize(p[(k + 1) % 3] - p[k]), b = glm::normalize(p[(k + 2) % 3] - p[k]);
                angle.push_back(std::acos(std::clamp(glm::dot(a, b), -1.0f, 1.0f)));
                corners.push_back({key(p[k]), tri, v[k]});
            }
        }
    std::vector<uint32_t> order(corners.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::ranges::sort(order, {}, [&](uint32_t c) { return corners[c].key; });
    std::vector<glm::vec3> acc(verts.size(), glm::vec3(0.0f));
    for (size_t g = 0; g < order.size();) {
        size_t e = g + 1;
        while (e < order.size() && corners[order[e]].key == corners[order[g]].key) ++e;
        for (size_t i = g; i < e; ++i) {
            const glm::vec3 nc = faceN[corners[order[i]].tri];
            glm::vec3 sum(0.0f);
            for (size_t j = g; j < e; ++j) {
                const glm::vec3 nd = faceN[corners[order[j]].tri];
                if (glm::dot(nd, nc) >= kCreaseCos) sum += angle[order[j]] * nd;
            }
            acc[corners[order[i]].vert] += glm::normalize(sum);
        }
        g = e;
    }
    std::vector<uint32_t> out(verts.size(), 0);
    for (size_t v = 0; v < acc.size(); ++v) {
        const float len = glm::length(acc[v]);
        if (len > 1e-6f) out[v] = glm::packSnorm4x8(glm::vec4(acc[v] / len, 0.0f));
    }
    return out;
}


void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::format("{} failed (VkResult {})", what, int(r)));
}

void imageBarrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, uint32_t mip, uint32_t mips,
                  VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, uint32_t layers = 1) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, mip, mips, 0, layers};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}

void memoryBarrier(VkCommandBuffer cb, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}

VKAPI_ATTR VkBool32 VKAPI_CALL onDebug(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                       const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) {
        ++*static_cast<uint32_t*>(user);
        std::println(stderr, "VK-VALIDATION [{}] {}", sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning",
                     data->pMessage);
    } else {
        std::println(stderr, "vk: {}", data->pMessage); // loader / third-party layer chatter, not counted
    }
    return VK_FALSE;
}

fs::path cacheDir() {
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return fs::path(x) / "bbtime";
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".cache/bbtime";
}

const char* vendorName(uint32_t id) {
    switch (id) {
    case 0x1002: return "AMD";
    case 0x10DE: return "NVIDIA";
    case 0x8086: return "Intel";
    case 0x10005: return "Mesa";
    default: return "unknown vendor";
    }
}

} // namespace

// ---------------------------------------------------------------- resources

uint32_t Renderer::memoryType(uint32_t bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & want) == want) return i;
    throw std::runtime_error("no suitable Vulkan memory type");
}

Renderer::Buffer Renderer::makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    check(vkCreateBuffer(device_, &ci, nullptr, &b.buf), "vkCreateBuffer");
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(device_, b.buf, &r);
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ai.pNext = &fi;
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = memoryType(r.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                           : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device_, &ai, nullptr, &b.mem), "vkAllocateMemory(buffer)");
    check(vkBindBufferMemory(device_, b.buf, b.mem, 0), "vkBindBufferMemory");
    if (host) check(vkMapMemory(device_, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map), "vkMapMemory");
    return b;
}

void Renderer::destroy(Buffer& b) {
    if (b.buf) vkDestroyBuffer(device_, b.buf, nullptr);
    if (b.mem) vkFreeMemory(device_, b.mem, nullptr);
    b = {};
}

// Replaces b with a buffer of at least `size` (doubling) when it is too small. Only for buffers no pending frame uses.
void Renderer::growBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host, std::vector<Buffer>* garbage) {
    if (b.size >= size) return;
    const VkDeviceSize n = std::max<VkDeviceSize>(size, 2 * b.size + 4096);
    if (garbage) garbage->push_back(b); // still referenced by commands recorded this frame
    else destroy(b);
    b = makeBuffer(n, usage, host);
}

VkDeviceAddress Renderer::address(VkBuffer b) const {
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai.buffer = b;
    return vkGetBufferDeviceAddress(device_, &ai);
}

void Renderer::destroy(Frame::Accel& a) {
    if (a.as) vkDestroyAS_(device_, a.as, nullptr);
    destroy(a.buf);
    a = {};
}

VkImage Renderer::newImage(uint32_t w, uint32_t h, VkFormat fmt, VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                           uint32_t mips) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img;
    check(vkCreateImage(device_, &ci, nullptr, &img), "vkCreateImage");
    return img;
}

VkImageView Renderer::newView(VkImage img, VkFormat fmt, VkImageAspectFlags aspect, uint32_t mips) {
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = img;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = fmt;
    ci.subresourceRange = {aspect, 0, mips, 0, 1};
    VkImageView v;
    check(vkCreateImageView(device_, &ci, nullptr, &v), "vkCreateImageView");
    return v;
}

// Render target with its own allocation. ponytail: no LAZILY_ALLOCATED memory (tilers only; desktop GPUs lack it).
Renderer::Image Renderer::makeTarget(VkFormat fmt, VkSampleCountFlagBits samples, VkImageUsageFlags usage) {
    Image im;
    im.img = newImage(extent_.width, extent_.height, fmt, samples, usage, 1);
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(device_, im.img, &r);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = memoryType(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device_, &ai, nullptr, &im.mem), "vkAllocateMemory(target)");
    check(vkBindImageMemory(device_, im.img, im.mem, 0), "vkBindImageMemory");
    const bool depth = fmt == kDepthFormat;
    im.view = newView(im.img, fmt, depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 1);
    return im;
}

void Renderer::destroy(Image& i) {
    if (i.view) vkDestroyImageView(device_, i.view, nullptr);
    if (i.img) vkDestroyImage(device_, i.img, nullptr);
    if (i.mem) vkFreeMemory(device_, i.mem, nullptr);
    i = {};
}

VkCommandBuffer Renderer::beginOneShot() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    check(vkAllocateCommandBuffers(device_, &ai, &cb), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");
    return cb;
}

void Renderer::endOneShot(VkCommandBuffer cb) {
    check(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
    VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    ci.commandBuffer = cb;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &ci;
    std::lock_guard lock(queueMutex_);
    check(vkQueueSubmit2(queue_, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit2(upload)");
    check(vkQueueWaitIdle(queue_), "vkQueueWaitIdle");
    vkFreeCommandBuffers(device_, pool_, 1, &cb);
}

uint32_t Renderer::allocSlot(VkImageView view) {
    if (freeSlots_.empty()) return kNoSlot;
    const uint32_t slot = freeSlots_.back();
    freeSlots_.pop_back();
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = 0;
    w.dstArrayElement = slot;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    return slot;
}

void Renderer::freeSlot(uint32_t slot) {
    destroy(slots_[slot]);
    freeSlots_.push_back(slot);
}

// Creates one image per source in a single device allocation (returned; the caller owns it), copies the texels
// through `staging` at `off` and generates mipmaps with blits. Appends one bindless slot per source to `slots`
// (kNoSlot when the table is full). Records into cb; the images are SHADER_READ_ONLY once cb has executed.
VkDeviceMemory Renderer::createTextures(VkCommandBuffer cb, const Buffer& staging, VkDeviceSize& off,
                                        std::span<const TexSrc> texs, bool mips, std::vector<uint32_t>& slots) {
    if (texs.empty()) return VK_NULL_HANDLE;
    std::vector<VkImage> imgs;
    std::vector<VkDeviceSize> offsets;
    std::vector<uint32_t> levels;
    VkDeviceSize total = 0;
    uint32_t bits = ~0u;
    for (const TexSrc& t : texs) {
        levels.push_back(mips ? uint32_t(std::bit_width(std::max(t.w, t.h))) : 1);
        imgs.push_back(newImage(t.w, t.h, kTexFormat, VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                levels.back()));
        VkMemoryRequirements r;
        vkGetImageMemoryRequirements(device_, imgs.back(), &r);
        total = (total + r.alignment - 1) / r.alignment * r.alignment;
        offsets.push_back(total);
        total += r.size;
        bits &= r.memoryTypeBits;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = total;
    ai.memoryTypeIndex = memoryType(bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory mem;
    check(vkAllocateMemory(device_, &ai, nullptr, &mem), "vkAllocateMemory(textures)");

    for (size_t i = 0; i < texs.size(); ++i) {
        const TexSrc& t = texs[i];
        VkImage img = imgs[i];
        check(vkBindImageMemory(device_, img, mem, offsets[i]), "vkBindImageMemory(texture)");
        const VkDeviceSize bytes = VkDeviceSize(t.w) * t.h * 4;
        std::memcpy(static_cast<uint8_t*>(staging.map) + off, t.rgba, bytes);

        constexpr auto C = VK_IMAGE_ASPECT_COLOR_BIT;
        constexpr auto DST = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, SRC = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       READ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        constexpr auto XFER = VK_PIPELINE_STAGE_2_TRANSFER_BIT, FRAG = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        imageBarrier(cb, img, C, 0, levels[i], VK_IMAGE_LAYOUT_UNDEFINED, DST, VK_PIPELINE_STAGE_2_NONE, 0, XFER,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{};
        copy.bufferOffset = off;
        copy.imageSubresource = {C, 0, 0, 1};
        copy.imageExtent = {t.w, t.h, 1};
        vkCmdCopyBufferToImage(cb, staging.buf, img, DST, 1, &copy);
        off += bytes;
        int32_t w = int32_t(t.w), h = int32_t(t.h);
        for (uint32_t m = 1; m < levels[i]; ++m) {
            imageBarrier(cb, img, C, m - 1, 1, DST, SRC, XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, XFER, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkImageBlit blit{};
            blit.srcSubresource = {C, m - 1, 0, 1};
            blit.srcOffsets[1] = {w, h, 1};
            w = std::max(w / 2, 1);
            h = std::max(h / 2, 1);
            blit.dstSubresource = {C, m, 0, 1};
            blit.dstOffsets[1] = {w, h, 1};
            vkCmdBlitImage(cb, img, SRC, img, DST, 1, &blit, VK_FILTER_LINEAR);
            imageBarrier(cb, img, C, m - 1, 1, SRC, READ, XFER, VK_ACCESS_2_TRANSFER_READ_BIT, FRAG, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        imageBarrier(cb, img, C, levels[i] - 1, 1, DST, READ, XFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, FRAG,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        Image im{img, newView(img, kTexFormat, C, levels[i]), VK_NULL_HANDLE};
        const uint32_t slot = allocSlot(im.view);
        if (slot == kNoSlot) destroy(im);
        else slots_[slot] = im;
        slots.push_back(slot);
    }
    return mem;
}

// ---------------------------------------------------------------- init / teardown

Renderer::Renderer() = default;

Renderer::~Renderer() {
    // BBTIME_VK_PRESENT_LOG=1: statistics of the CPU time between consecutive presents of 3-D frames (frame-generation
    // pacing; the first 32 are skipped as warm-up).
    if (const char* e = std::getenv("BBTIME_VK_PRESENT_LOG"); e && *e && *e != '0' && presentTimes_.size() > 64) {
        std::vector<double> d;
        for (size_t i = 32; i < presentTimes_.size(); ++i) d.push_back(double(presentTimes_[i] - presentTimes_[i - 1]) * 1e-6); // skip warm-up
        double mean = 0, var = 0;
        for (double x : d) mean += x / double(d.size());
        for (double x : d) var += (x - mean) * (x - mean) / double(d.size());
        std::ranges::sort(d);
        double tm = 0, tv = 0; // without the longest 1% (hitches: shader compiles, level events)
        const size_t n = d.size() - d.size() / 100;
        for (size_t i = 0; i < n; ++i) tm += d[i] / double(n);
        for (size_t i = 0; i < n; ++i) tv += (d[i] - tm) * (d[i] - tm) / double(n);
        std::println(stderr, "vk: present intervals n {} mean {:.3f} ms stddev {:.3f} ms | trimmed 99%: mean {:.3f} stddev {:.3f} cv {:.2f} | "
                     "p5 {:.3f} p50 {:.3f} p95 {:.3f}", d.size(), mean, std::sqrt(var), tm, std::sqrt(tv), std::sqrt(tv) / tm,
                     d[d.size() / 20], d[d.size() / 2], d[d.size() * 19 / 20]);
    }
    if (device_) {
        try {
            stopPresenter();
        } catch (const std::exception& e) {
            std::println(stderr, "vk: present thread: {}", e.what());
        }
        vkDeviceWaitIdle(device_);
        freeScene();
        fsr_.reset();
        destroyFsrTargets();
        for (uint32_t s = 0; s < slots_.size(); ++s) destroy(slots_[s]);
        if (pipelineCache_) {
            size_t n = 0;
            vkGetPipelineCacheData(device_, pipelineCache_, &n, nullptr);
            std::vector<char> data(n);
            if (n && vkGetPipelineCacheData(device_, pipelineCache_, &n, data.data()) == VK_SUCCESS) {
                // Atomic replace: several processes (parallel ctest) start / exit at once and must never read a
                // half-written cache.
                std::error_code ec;
                fs::create_directories(pipelineCachePath_.parent_path(), ec);
                const fs::path tmp = pipelineCachePath_.string() + std::format(".{}.tmp", SDL_GetCurrentThreadID());
                if (std::ofstream(tmp, std::ios::binary).write(data.data(), std::streamsize(n))) fs::rename(tmp, pipelineCachePath_, ec);
                else fs::remove(tmp, ec);
            }
            vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
        }
        for (Frame& f : frames_) {
            destroy(f.stream);
            for (Buffer* b : {&f.light, &f.geos, &f.instances, &f.scratch}) destroy(*b);
            if (rt_) destroy(f.actorBlas), destroy(f.tlas);
            for (Buffer& b : f.garbage) destroy(b);
            vkDestroyFence(device_, f.fence, nullptr);
            vkDestroySemaphore(device_, f.acquired, nullptr);
            for (int k = 0; k < 2; ++k) {
                vkDestroyFence(device_, f.copyFence[k], nullptr);
                vkDestroySemaphore(device_, f.copyAcquired[k], nullptr);
            }
        }
        destroySwapchain();
        for (auto& row : pipe3D_)
            for (VkPipeline p : row) vkDestroyPipeline(device_, p, nullptr);
        for (VkPipeline p : pipe2D_) vkDestroyPipeline(device_, p, nullptr);
        for (auto& kind : pipeLit_)
            for (auto& mv : kind)
                for (auto& row : mv)
                    for (VkPipeline p : row) vkDestroyPipeline(device_, p, nullptr);
        vkDestroyPipeline(device_, pipeShadow_, nullptr);
        for (VkImageView v : shadowLayers_) vkDestroyImageView(device_, v, nullptr);
        destroy(shadowMap_);
        vkDestroySampler(device_, shadowSampler_, nullptr);
        vkDestroyDescriptorPool(device_, lightPool_, nullptr);
        vkDestroyDescriptorSetLayout(device_, lightLayout_, nullptr);
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        vkDestroyDescriptorPool(device_, descPool_, nullptr);
        vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
        for (VkSampler s : samplers_) vkDestroySampler(device_, s, nullptr);
        vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyCommandPool(device_, presentPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (messenger_) {
        auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        destroyMessenger(instance_, messenger_, nullptr);
        std::println(stderr, "vk: {} validation messages", validationMessages_);
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
    if (window_) SDL_DestroyWindow(window_);
}

std::string Renderer::init(const WindowConfig& cfg, bool validation) try {
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (cfg.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    if (cfg.hidden) flags |= SDL_WINDOW_HIDDEN;
    window_ = SDL_CreateWindow(cfg.title, cfg.width, cfg.height, flags);
    if (!window_) return std::string("cannot create Vulkan window: ") + SDL_GetError();

    uint32_t n = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&n);
    if (!sdlExts) return std::string("SDL_Vulkan_GetInstanceExtensions: ") + SDL_GetError();
    std::vector<const char*> exts(sdlExts, sdlExts + n), layers;
    VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbg.pfnUserCallback = onDebug;
    dbg.pUserData = &validationMessages_;
    const VkBool32 on = VK_TRUE;
    const VkLayerSettingEXT syncval{"VK_LAYER_KHRONOS_validation", "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &on};
    VkLayerSettingsCreateInfoEXT settings{VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT, &dbg, 1, &syncval};
    if (validation) {
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        exts.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "bbtime";
    app.pEngineName = "bbtime";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pNext = validation ? &settings : nullptr;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    check(vkCreateInstance(&ici, nullptr, &instance_), validation ? "vkCreateInstance (validation layer installed?)" : "vkCreateInstance");
    if (validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        check(create(instance_, &dbg, nullptr, &messenger_), "vkCreateDebugUtilsMessengerEXT");
    }
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_))
        return std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError();

    // Device: Vulkan 1.3 + dynamic rendering, sync2, descriptor indexing, a graphics queue that can present.
    check(vkEnumeratePhysicalDevices(instance_, &n, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> gpus(n);
    check(vkEnumeratePhysicalDevices(instance_, &n, gpus.data()), "vkEnumeratePhysicalDevices");
    int bestScore = -1;
    for (VkPhysicalDevice g : gpus) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(g, &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
        vkGetPhysicalDeviceFeatures2(g, &f2);
        if (!f13.dynamicRendering || !f13.synchronization2 || !f13.shaderDemoteToHelperInvocation || !f12.runtimeDescriptorArray || !f12.descriptorBindingPartiallyBound ||
            !f12.descriptorBindingSampledImageUpdateAfterBind || !f12.descriptorBindingUpdateUnusedWhilePending ||
            !f12.shaderSampledImageArrayNonUniformIndexing)
            continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qs.data());
        for (uint32_t q = 0; q < qn; ++q) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(g, q, surface_, &present);
            if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
            if (score > bestScore) { bestScore = score; gpu_ = g; queueFamily_ = q; }
            break;
        }
    }
    if (!gpu_) return "no Vulkan 1.3 device with dynamic rendering, synchronization2, descriptor indexing and presentation";

    VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12};
    vkGetPhysicalDeviceProperties2(gpu_, &p2);
    const VkPhysicalDeviceLimits& lim = p2.properties.limits;
    vkGetPhysicalDeviceMemoryProperties(gpu_, &memProps_);
    VkPhysicalDeviceFeatures base;
    vkGetPhysicalDeviceFeatures(gpu_, &base);
    vendor_ = vendorName(p2.properties.vendorID);
    rendererName_ = p2.properties.deviceName;
    const uint32_t api = p2.properties.apiVersion;
    version_ = std::format("Vulkan {}.{}.{} {} {}", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api),
                           p12.driverName, p12.driverInfo);
    if (base.samplerAnisotropy) maxAniso_ = lim.maxSamplerAnisotropy;
    const VkSampleCountFlags counts = lim.framebufferColorSampleCounts & lim.framebufferDepthSampleCounts;
    for (int s = std::bit_floor(unsigned(std::max(cfg.msaaSamples, 1))); s > 1 && msaaMax_ == VK_SAMPLE_COUNT_1_BIT; s /= 2)
        if (counts & unsigned(s)) msaaMax_ = VkSampleCountFlagBits(s);
    msaaSamples_ = msaaMax_ > 1 ? int(msaaMax_) : 0;
    maxSlots_ = std::min({4096u, p12.maxPerStageDescriptorUpdateAfterBindSampledImages, p12.maxDescriptorSetUpdateAfterBindSampledImages});

    // Ray tracing: ray queries from the fragment shader (shadows, AO). The RT pipeline extension is only reported.
    check(vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &n, nullptr), "vkEnumerateDeviceExtensionProperties");
    std::vector<VkExtensionProperties> dexts(n);
    check(vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &n, dexts.data()), "vkEnumerateDeviceExtensionProperties");
    auto has = [&](std::string_view e) { return std::ranges::any_of(dexts, [&](const auto& x) { return e == x.extensionName; }); };
    VkPhysicalDeviceRayQueryFeaturesKHR rqf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rqf};
    VkPhysicalDeviceVulkan12Features q12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &asf};
    VkPhysicalDeviceFeatures2 qf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q12};
    vkGetPhysicalDeviceFeatures2(gpu_, &qf);
    const bool rtExts = has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && has(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                        has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    const char* noRt = std::getenv("BBTIME_VK_NO_RT");
    const bool rtForcedOff = noRt && *noRt && *noRt != '0';
    rt_ = rtExts && asf.accelerationStructure && rqf.rayQuery && q12.bufferDeviceAddress && !rtForcedOff;
    rtProbe_ = std::format("RT: accel_struct {} ray_query {} rt_pipeline {}{}", has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) ? "yes" : "no",
                           has(VK_KHR_RAY_QUERY_EXTENSION_NAME) ? "yes" : "no", has(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) ? "yes" : "no",
                           rtForcedOff ? " (off: BBTIME_VK_NO_RT)" : rt_ ? "" : " (unsupported: shadow maps)");
    diag_ = rtProbe_;
    if (rt_) {
        VkPhysicalDeviceAccelerationStructurePropertiesKHR asp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 pp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &asp};
        vkGetPhysicalDeviceProperties2(gpu_, &pp);
        scratchAlign_ = std::max<VkDeviceSize>(asp.minAccelerationStructureScratchOffsetAlignment, 1);
    }
    // FSR 3.1.4 (third_party/fidelityfx-sdk, the etiyami requirements): the FP32/wave32 blobs write and read formatless
    // storage images and use compute quad / arithmetic subgroup operations; the upscaler's resources and the RGBA8
    // display images need storage; the output is blitted into the swapchain; motion vectors need independent blend.
    {
        VkPhysicalDeviceSubgroupProperties sp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 pp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sp};
        vkGetPhysicalDeviceProperties2(gpu_, &pp);
        auto storage = [&](VkFormat f, VkFormatFeatureFlags more = 0) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(gpu_, f, &fp);
            const VkFormatFeatureFlags want = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | more;
            return (fp.optimalTilingFeatures & want) == want;
        };
        VkSurfaceCapabilitiesKHR caps;
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        const char* noFsr = std::getenv("BBTIME_VK_NO_FSR");
        const bool fsrForcedOff = noFsr && *noFsr && *noFsr != '0';
        constexpr VkSubgroupFeatureFlags ops = VK_SUBGROUP_FEATURE_QUAD_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
        fsrOk_ = !fsrForcedOff && base.shaderStorageImageWriteWithoutFormat && base.shaderStorageImageReadWithoutFormat &&
                 base.independentBlend && (sp.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) && (sp.supportedOperations & ops) == ops &&
                 storage(VK_FORMAT_R16G16B16A16_SFLOAT) && storage(VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) &&
                 storage(VK_FORMAT_R32_SFLOAT) && storage(VK_FORMAT_R32_UINT) &&
                 storage(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) &&
                 (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        fsrProbe_ = fsrForcedOff ? "FSR: off (BBTIME_VK_NO_FSR)" : fsrOk_ ? "FSR 3.1.4: yes" : "FSR 3.1.4: unsupported";
    }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceRayQueryFeaturesKHR erq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    erq.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR eas{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &erq};
    eas.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, rt_ ? &eas : nullptr};
    e13.dynamicRendering = e13.synchronization2 = VK_TRUE;
    e13.shaderDemoteToHelperInvocation = VK_TRUE; // glslc lowers `discard` to demote for Vulkan 1.3 (required feature)
    VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &e13};
    e12.runtimeDescriptorArray = e12.descriptorBindingPartiallyBound = e12.descriptorBindingSampledImageUpdateAfterBind =
        e12.descriptorBindingUpdateUnusedWhilePending = e12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    e12.bufferDeviceAddress = rt_;
    VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e12};
    e2.features.samplerAnisotropy = base.samplerAnisotropy;
    e2.features.shaderStorageImageWriteWithoutFormat = e2.features.shaderStorageImageReadWithoutFormat = e2.features.independentBlend = fsrOk_;
    const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                             VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &e2};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = rt_ ? 4 : 1;
    dci.ppEnabledExtensionNames = devExts;
    check(vkCreateDevice(gpu_, &dci, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    if (fsrOk_ && !Fsr::load(instance_)) {
        fsrOk_ = false;
        fsrProbe_ = "FSR 3.1.4: Vulkan entry points missing";
    }
    if (fsrOk_) fsr_ = std::make_unique<Fsr>(instance_, gpu_, device_);
    diag_ = rtProbe_ + " | " + fsrProbe_;
    if (rt_) {
        auto fn = [&]<class F>(F& out, const char* name) { out = reinterpret_cast<F>(vkGetDeviceProcAddr(device_, name)); };
        fn(vkCreateAS_, "vkCreateAccelerationStructureKHR");
        fn(vkDestroyAS_, "vkDestroyAccelerationStructureKHR");
        fn(vkASSizes_, "vkGetAccelerationStructureBuildSizesKHR");
        fn(vkCmdBuildAS_, "vkCmdBuildAccelerationStructuresKHR");
        fn(vkASAddress_, "vkGetAccelerationStructureDeviceAddressKHR");
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily_;
    check(vkCreateCommandPool(device_, &pci, nullptr, &pool_), "vkCreateCommandPool");
    check(vkCreateCommandPool(device_, &pci, nullptr, &presentPool_), "vkCreateCommandPool(present thread)");

    // Surface format: 8-bit UNORM like the GL default framebuffer (no sRGB conversion).
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR");
    std::vector<VkSurfaceFormatKHR> fmts(n);
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, fmts.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR");
    swapFormat_ = fmts.empty() ? VK_FORMAT_UNDEFINED : fmts[0].format;
    for (VkFormat want : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM})
        for (const VkSurfaceFormatKHR& f : fmts)
            if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) swapFormat_ = want;

    // Samplers (immutable in the set layout): same four as the GL renderer.
    auto sampler = [&](VkFilter filter, VkSamplerMipmapMode mip, VkSamplerAddressMode wrap, float aniso, float maxLod) {
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = ci.minFilter = filter;
        ci.mipmapMode = mip;
        ci.addressModeU = ci.addressModeV = ci.addressModeW = wrap;
        ci.anisotropyEnable = aniso > 1.0f;
        ci.maxAnisotropy = aniso;
        ci.maxLod = maxLod;
        VkSampler s;
        check(vkCreateSampler(device_, &ci, nullptr, &s), "vkCreateSampler");
        return s;
    };
    samplers_[kSampNearest] = sampler(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT, 1.0f, 0.0f);
    samplers_[kSampTrilinear] = sampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, maxAniso_, VK_LOD_CLAMP_NONE);
    samplers_[kSampUiNearest] = sampler(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f, 0.0f);
    samplers_[kSampUiLinear] = sampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f, 0.0f);
    samplers_[kSampLinear] = sampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT, 1.0f, 0.0f);

    // One bindless set: binding 0 = every texture (update-after-bind), binding 1 = the four samplers.
    VkDescriptorSetLayoutBinding binds[2]{};
    binds[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxSlots_, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    binds[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLER, 5, VK_SHADER_STAGE_FRAGMENT_BIT, samplers_};
    const VkDescriptorBindingFlags bindFlags[2] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                                       VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = 2;
    bfi.pBindingFlags = bindFlags;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfi};
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    lci.bindingCount = 2;
    lci.pBindings = binds;
    check(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &setLayout_), "vkCreateDescriptorSetLayout");
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxSlots_}, {VK_DESCRIPTOR_TYPE_SAMPLER, 5}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    dpi.maxSets = 1;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(device_, &dpi, nullptr, &descPool_), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsa.descriptorPool = descPool_;
    dsa.descriptorSetCount = 1;
    dsa.pSetLayouts = &setLayout_;
    check(vkAllocateDescriptorSets(device_, &dsa, &set_), "vkAllocateDescriptorSets");
    slots_.assign(maxSlots_, {});
    for (uint32_t s = maxSlots_; s-- > 0;) freeSlots_.push_back(s); // pop_back hands out 0, 1, 2, ...

    // Set 1, one per frame in flight: Enhanced lighting / FSR. 0 = LightUbo (also the motion-vector matrices),
    // 1 = cascaded shadow map (compare sampler),
    // ray tracing only: 2 = TLAS, 3 = geometry table for alpha-tested hits (RtGeo).
    {
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
        ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // outside a cascade = lit
        ci.compareEnable = VK_TRUE;
        ci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        check(vkCreateSampler(device_, &ci, nullptr, &shadowSampler_), "vkCreateSampler(shadow)");
        const VkShaderStageFlags fs = VK_SHADER_STAGE_FRAGMENT_BIT;
        const VkDescriptorSetLayoutBinding lb[4] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, fs | VK_SHADER_STAGE_VERTEX_BIT, nullptr},
                                                    {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fs, &shadowSampler_},
                                                    {2, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, fs, nullptr},
                                                    {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, fs, nullptr}};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = rt_ ? 4 : 2;
        li.pBindings = lb;
        check(vkCreateDescriptorSetLayout(device_, &li, nullptr, &lightLayout_), "vkCreateDescriptorSetLayout(light)");
        const VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFrames}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFrames},
                                            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kFrames}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFrames}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = kFrames;
        pi.poolSizeCount = rt_ ? 4 : 2;
        pi.pPoolSizes = ps;
        check(vkCreateDescriptorPool(device_, &pi, nullptr, &lightPool_), "vkCreateDescriptorPool(light)");

        // Shadow map: kCascades layers of D32, sampled as one array.
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D;
        ic.format = kDepthFormat;
        ic.extent = {kShadowSize, kShadowSize, 1};
        ic.mipLevels = 1;
        ic.arrayLayers = kCascades;
        ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        check(vkCreateImage(device_, &ic, nullptr, &shadowMap_.img), "vkCreateImage(shadow)");
        VkMemoryRequirements r;
        vkGetImageMemoryRequirements(device_, shadowMap_.img, &r);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = r.size;
        ai.memoryTypeIndex = memoryType(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device_, &ai, nullptr, &shadowMap_.mem), "vkAllocateMemory(shadow)");
        check(vkBindImageMemory(device_, shadowMap_.img, shadowMap_.mem, 0), "vkBindImageMemory(shadow)");
        VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vc.image = shadowMap_.img;
        vc.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vc.format = kDepthFormat;
        vc.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kCascades};
        check(vkCreateImageView(device_, &vc, nullptr, &shadowMap_.view), "vkCreateImageView(shadow)");
        vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vc.subresourceRange.layerCount = 1;
        for (uint32_t c = 0; c < kCascades; ++c) {
            vc.subresourceRange.baseArrayLayer = c;
            check(vkCreateImageView(device_, &vc, nullptr, &shadowLayers_[c]), "vkCreateImageView(shadow layer)");
        }
        // Sampled before the first shadow pass only by frames that never draw it: start in the sampled layout.
        VkCommandBuffer cb = beginOneShot();
        imageBarrier(cb, shadowMap_.img, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kCascades);
        endOneShot(cb);
    }

    VkPushConstantRange pcr{kPushStages, 0, sizeof(Push3D)};
    const VkDescriptorSetLayout setLayouts[2] = {setLayout_, lightLayout_};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 2;
    plci.pSetLayouts = setLayouts;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    check(vkCreatePipelineLayout(device_, &plci, nullptr, &layout_), "vkCreatePipelineLayout");

    // Pipeline cache persisted across runs; the driver rejects data from another device/driver by itself.
    pipelineCachePath_ = cacheDir() / "vk_pipeline_cache.bin";
    std::vector<char> cacheData;
    if (std::ifstream in(pipelineCachePath_, std::ios::binary); in) cacheData.assign(std::istreambuf_iterator<char>(in), {});
    VkPipelineCacheCreateInfo cci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    cci.initialDataSize = cacheData.size();
    cci.pInitialData = cacheData.data();
    if (vkCreatePipelineCache(device_, &cci, nullptr, &pipelineCache_) != VK_SUCCESS) {
        cci.initialDataSize = 0;
        check(vkCreatePipelineCache(device_, &cci, nullptr, &pipelineCache_), "vkCreatePipelineCache");
    }
    createPipelines();

    for (Frame& f : frames_) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device_, &ai, &f.cmd), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(device_, &fci, nullptr, &f.fence), "vkCreateFence");
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device_, &sci, nullptr, &f.acquired), "vkCreateSemaphore");
        VkFenceCreateInfo unsignaled{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkCommandBufferAllocateInfo pa = ai;
        pa.commandPool = presentPool_;
        pa.commandBufferCount = 2;
        check(vkAllocateCommandBuffers(device_, &pa, f.copyCmd), "vkAllocateCommandBuffers(present)");
        for (int k = 0; k < 2; ++k) {
            check(vkCreateSemaphore(device_, &sci, nullptr, &f.copyAcquired[k]), "vkCreateSemaphore");
            check(vkCreateFence(device_, &unsignaled, nullptr, &f.copyFence[k]), "vkCreateFence");
        }
        f.stream = makeBuffer(1 << 20, kStreamUsage | (rt_ ? kRtInput : 0), true);
        f.light = makeBuffer(sizeof(LightUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        VkDescriptorSetAllocateInfo la{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        la.descriptorPool = lightPool_;
        la.descriptorSetCount = 1;
        la.pSetLayouts = &lightLayout_;
        check(vkAllocateDescriptorSets(device_, &la, &f.lightSet), "vkAllocateDescriptorSets(light)");
        const VkDescriptorBufferInfo bi{f.light.buf, 0, sizeof(LightUbo)};
        const VkDescriptorImageInfo ii{VK_NULL_HANDLE, shadowMap_.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstSet = w[1].dstSet = f.lightSet;
        w[0].descriptorCount = w[1].descriptorCount = 1;
        w[0].dstBinding = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &bi;
        w[1].dstBinding = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo = &ii;
        vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    }

    Texture white{.name = "white", .width = 1, .height = 1, .rgba = {255, 255, 255, 255}};
    if (createImage(white) != kSlotWhite || createImage(render::fontAtlas()) != kSlotFont)
        return "cannot create white/font textures";
    SDL_GetWindowSizeInPixels(window_, &w_, &h_);
    return {};
} catch (const std::exception& e) {
    return e.what();
}

void Renderer::createPipelines() {
    auto module = [&](std::span<const uint32_t> code) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size_bytes();
        ci.pCode = code.data();
        VkShaderModule m;
        check(vkCreateShaderModule(device_, &ci, nullptr, &m), "vkCreateShaderModule");
        return m;
    };
    VkShaderModule sv = module(kSceneVert), sf = module(kSceneFrag), uv = module(kUiVert), uf = module(kUiFrag),
                   lv = module(kSceneLitVert), mf = module(kSceneMapFrag), rf = rt_ ? module(kSceneRtFrag) : VK_NULL_HANDLE,
                   hv = module(kShadowVert), hf = module(kShadowFrag), vv = fsrOk_ ? module(kSceneMvVert) : VK_NULL_HANDLE,
                   vf = fsrOk_ ? module(kSceneMvFrag) : VK_NULL_HANDLE, mvf = fsrOk_ ? module(kSceneMapMvFrag) : VK_NULL_HANDLE,
                   rvf = fsrOk_ && rt_ ? module(kSceneRtMvFrag) : VK_NULL_HANDLE;

    // Scene vertex input: binding 0 Vertex; lit variants add binding 1 smooth normals (R8G8B8A8_SNORM), motion-vector
    // variants also binding 2 = the previous frame's Vertex array (position only).
    const VkVertexInputBindingDescription vb3[3] = {{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
                                                    {1, sizeof(uint32_t), VK_VERTEX_INPUT_RATE_VERTEX},
                                                    {2, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    const VkVertexInputAttributeDescription va3[5] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)},
                                                      {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
                                                      {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, rgba)},
                                                      {3, 1, VK_FORMAT_R8G8B8A8_SNORM, 0},
                                                      {4, 2, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)}};
    const VkVertexInputBindingDescription vb2{0, sizeof(V2), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription va2[4] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(V2, x)},
                                                      {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(V2, u)},
                                                      {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(V2, rgba)},
                                                      {3, 0, VK_FORMAT_R32_UINT, offsetof(V2, texSamp)}};

    // Blend modes as in the GL renderer; destination alpha is kept (window never turns see-through).
    auto blend = [](bool on, VkBlendFactor dst, VkBlendOp op) {
        VkPipelineColorBlendAttachmentState b{};
        b.blendEnable = on;
        b.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        b.dstColorBlendFactor = dst;
        b.colorBlendOp = op;
        b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        b.alphaBlendOp = VK_BLEND_OP_ADD;
        b.colorWriteMask = 0xF;
        return b;
    };
    const VkPipelineColorBlendAttachmentState blends[4] = {
        blend(false, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD),                // Opaque + AlphaTest
        blend(true, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD),  // Alpha (and the 2D layer)
        blend(true, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD),                  // Additive
        blend(true, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_REVERSE_SUBTRACT)};    // Subtract: dst - src

    // shadow: depth-only caster pass into a shadow-map layer (no culling, slope-scaled depth bias). inputs: scene vertex
    // bindings (1 Vertex, 2 + normals, 3 + previous positions = motion vectors into a second colour attachment).
    auto make = [&](VkShaderModule vert, VkShaderModule frag, bool ui, int blendIdx, VkSampleCountFlagBits samples, bool shadow = false,
                    int inputs = 1) {
        const bool mv = inputs == 3;
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
        st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = ui ? 1 : uint32_t(inputs);
        vi.pVertexBindingDescriptions = ui ? &vb2 : vb3;
        vi.vertexAttributeDescriptionCount = ui ? 4 : shadow ? 2 : uint32_t(2 + inputs);
        vi.pVertexAttributeDescriptions = ui ? va2 : va3;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // engine space front faces; y flipped by the negative viewport
        rs.lineWidth = 1.0f;
        rs.depthBiasEnable = shadow;
        rs.depthBiasConstantFactor = 1.0f;
        rs.depthBiasSlopeFactor = 2.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = samples;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = !ui;
        ds.depthWriteEnable = !ui && blendIdx == 0;
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        // Motion vectors: written by depth-writing surfaces only (translucent ones keep what lies behind them). The FSR
        // reactive mask: MAX of every surface's value.
        VkPipelineColorBlendAttachmentState att[3] = {blends[blendIdx], blend(false, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD),
                                                      blend(true, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_MAX)};
        att[1].colorWriteMask = blendIdx == 0 ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT : 0;
        att[2].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        att[2].alphaBlendOp = VK_BLEND_OP_MAX;
        att[2].colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = shadow ? 0 : mv ? 3 : 1;
        cb.pAttachments = att;
        const VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 3;
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        const VkFormat formats[3] = {swapFormat_, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R8_UNORM};
        ri.colorAttachmentCount = shadow ? 0 : mv ? 3 : 1;
        ri.pColorAttachmentFormats = formats;
        ri.depthAttachmentFormat = kDepthFormat;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
        ci.stageCount = 2;
        ci.pStages = st;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = layout_;
        VkPipeline p;
        check(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &ci, nullptr, &p), "vkCreateGraphicsPipelines");
        return p;
    };
    for (int m = 0; m < 2; ++m) {
        if (m == 1 && msaaMax_ == VK_SAMPLE_COUNT_1_BIT) break;
        const VkSampleCountFlagBits samples = m ? msaaMax_ : VK_SAMPLE_COUNT_1_BIT;
        for (int b = 0; b < 4; ++b) {
            pipe3D_[m][b] = make(sv, sf, false, b, samples);
            pipeLit_[1][0][m][b] = make(lv, mf, false, b, samples, false, 2);
            if (rt_) pipeLit_[2][0][m][b] = make(lv, rf, false, b, samples, false, 2);
        }
        pipe2D_[m] = make(uv, uf, true, 1, samples);
    }
    for (int b = 0; fsrOk_ && b < 4; ++b) { // FSR renders without MSAA
        pipeLit_[0][1][0][b] = make(vv, vf, false, b, VK_SAMPLE_COUNT_1_BIT, false, 3);
        pipeLit_[1][1][0][b] = make(vv, mvf, false, b, VK_SAMPLE_COUNT_1_BIT, false, 3);
        if (rt_) pipeLit_[2][1][0][b] = make(vv, rvf, false, b, VK_SAMPLE_COUNT_1_BIT, false, 3);
    }
    pipeShadow_ = make(hv, hf, false, 0, VK_SAMPLE_COUNT_1_BIT, true);
    for (VkShaderModule s : {sv, sf, uv, uf, lv, mf, rf, hv, hf, vv, vf, mvf, rvf}) vkDestroyShaderModule(device_, s, nullptr);
}

// ---------------------------------------------------------------- swapchain

void Renderer::destroySwapchain() {
    for (VkImageView v : swapViews_) vkDestroyImageView(device_, v, nullptr);
    for (VkSemaphore s : presentReady_) vkDestroySemaphore(device_, s, nullptr);
    swapViews_.clear();
    presentReady_.clear();
    swapImages_.clear();
    destroy(depth_);
    destroy(msaaColor_);
    destroy(msaaDepth_);
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void Renderer::recreateSwapchain() {
    waitIdle();
    VkSurfaceCapabilitiesKHR caps;
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) // Wayland: the application picks the size
        ext = {std::clamp(uint32_t(w_), caps.minImageExtent.width, caps.maxImageExtent.width),
               std::clamp(uint32_t(h_), caps.minImageExtent.height, caps.maxImageExtent.height)};
    if (!ext.width || !ext.height) return; // minimized: stay dirty, frames are skipped

    uint32_t n = 0;
    check(vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &n, nullptr), "vkGetPhysicalDeviceSurfacePresentModesKHR");
    std::vector<VkPresentModeKHR> modes(n);
    check(vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &n, modes.data()), "vkGetPhysicalDeviceSurfacePresentModesKHR");
    presentMode_ = VK_PRESENT_MODE_FIFO_KHR; // always supported
    const auto prefs = vsync_ ? std::vector{VK_PRESENT_MODE_FIFO_RELAXED_KHR}
                              : std::vector{VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR};
    for (VkPresentModeKHR want : prefs | std::views::reverse)
        if (std::ranges::contains(modes, want)) presentMode_ = want;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    const uint32_t want = caps.minImageCount + 1;
    ci.minImageCount = caps.maxImageCount ? std::min(want, caps.maxImageCount) : want;
    ci.imageFormat = swapFormat_;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    swapReadable_ = caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (swapReadable_ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0) |
                    (fsrOk_ ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0); // FSR output / generated frames are copied in
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    ci.presentMode = presentMode_;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain_;
    VkSwapchainKHR sc;
    check(vkCreateSwapchainKHR(device_, &ci, nullptr, &sc), "vkCreateSwapchainKHR");
    destroySwapchain();
    swapchain_ = sc;
    extent_ = ext;
    swapW_ = w_;
    swapH_ = h_;
    swapVsync_ = vsync_;

    check(vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr), "vkGetSwapchainImagesKHR");
    swapImages_.resize(n);
    check(vkGetSwapchainImagesKHR(device_, swapchain_, &n, swapImages_.data()), "vkGetSwapchainImagesKHR");
    for (VkImage img : swapImages_) {
        swapViews_.push_back(newView(img, swapFormat_, VK_IMAGE_ASPECT_COLOR_BIT, 1));
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device_, &sci, nullptr, &presentReady_.emplace_back()), "vkCreateSemaphore");
    }
    depth_ = makeTarget(kDepthFormat, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    if (msaaMax_ > VK_SAMPLE_COUNT_1_BIT) {
        msaaColor_ = makeTarget(swapFormat_, msaaMax_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT);
        msaaDepth_ = makeTarget(kDepthFormat, msaaMax_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT);
    }
    swapDirty_ = false;
}

void Renderer::setVsync(bool on) { vsync_ = on; }

void Renderer::setFullscreen(bool on) { SDL_SetWindowFullscreen(window_, on); }

// ---------------------------------------------------------------- scene

void Renderer::freeScene() {
    for (uint32_t s : sceneSlots_)
        if (s != kNoSlot) freeSlot(s);
    sceneSlots_.clear();
    texSlot_.clear();
    texAnims_.clear();
    meshes_.clear();
    sceneMeshCount_ = 0;
    actorVerts_.clear();
    actorIdx_.clear();
    actorLayout_.clear();
    prevActorLayout_.clear();
    prevValid_ = false;
    destroy(sceneVbo_);
    destroy(sceneIbo_);
    destroy(sceneNrm_);
    destroy(sceneBlas_);
    sceneGeos_.clear();
    actorGeos_.clear();
    if (sceneMem_) vkFreeMemory(device_, sceneMem_, nullptr);
    sceneMem_ = VK_NULL_HANDLE;
}

void Renderer::addMeshes(std::span<const Mesh> ms, bool actor, size_t casters, std::vector<Vertex>& verts, std::vector<uint32_t>& idx) {
    for (size_t i = 0; i < ms.size(); ++i) {
        const Mesh& m = ms[i];
        if (m.vertices.empty() || m.indices.empty()) continue;
        glm::vec3 lo(m.vertices[0].pos[0], m.vertices[0].pos[1], m.vertices[0].pos[2]), hi = lo;
        for (const Vertex& v : m.vertices) {
            lo = glm::min(lo, glm::vec3(v.pos[0], v.pos[1], v.pos[2]));
            hi = glm::max(hi, glm::vec3(v.pos[0], v.pos[1], v.pos[2]));
        }
        const uint32_t tex = m.texture >= 0 && size_t(m.texture) < texSlot_.size() ? texSlot_[size_t(m.texture)] : kSlotWhite;
        const bool animOk = m.textureAnim >= 0 && size_t(m.textureAnim) < texAnims_.size() && !texAnims_[size_t(m.textureAnim)].frames.empty();
        // Sun shadows: TB sky domes (tb_level.cpp "sky…" groups) and HUD models neither cast nor receive; additive /
        // subtractive glows only add light; translucent surfaces receive but do not cast.
        const bool world = actor ? i < casters : !m.name.starts_with("sky");
        const bool solid = m.blend == Blend::Opaque || m.blend == Blend::AlphaTest;
        meshes_.push_back({tex, m.blend, m.twoSided, actor, world && solid, world && (solid || m.blend == Blend::Alpha), int32_t(verts.size()),
                           uint32_t(idx.size()), uint32_t(m.indices.size()), (lo + hi) * 0.5f, animOk ? m.textureAnim : -1, 0,
                           // FSR reactive: LiT type-4 sprite effects (apps/bbtime poseActors names them) and animated textures
                           (actor && m.name == "sprite") || animOk, m.zone});
        if (actor) actorLayout_.push_back(uint32_t(m.vertices.size()));
        if (!actor && world && solid) {
            casterLo_ = glm::min(casterLo_, lo);
            casterHi_ = glm::max(casterHi_, hi);
        }
        verts.insert(verts.end(), m.vertices.begin(), m.vertices.end());
        idx.insert(idx.end(), m.indices.begin(), m.indices.end());
    }
}

void Renderer::setActorMeshes(std::span<const Mesh> ms, size_t shadowCasters) {
    meshes_.resize(sceneMeshCount_);
    // Motion vectors pair this call's vertices with the previous call's (same mesh layout = same posed models).
    std::swap(prevActorVerts_, actorVerts_);
    std::swap(prevActorLayout_, actorLayout_);
    actorVerts_.clear();
    actorIdx_.clear();
    actorLayout_.clear();
    actorNrm_.clear(); // computed by drawScene when a lit / motion-vector pipeline needs it
    addMeshes(ms, true, shadowCasters, actorVerts_, actorIdx_);
}

// One BLAS triangle geometry for mesh m (vertex buffer at vtx, index buffer at idx) and its scene.frag Geo entry.
void Renderer::blasGeometry(const GpuMesh& m, VkDeviceAddress vtx, VkDeviceAddress idx, uint32_t maxVertex,
                            std::vector<VkAccelerationStructureGeometryKHR>& geoms,
                            std::vector<VkAccelerationStructureBuildRangeInfoKHR>& ranges, std::vector<RtGeo>& table) const {
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    // Alpha-tested casters stay non-opaque: scene.frag's ray loop tests their texel alpha.
    g.flags = m.blend == Blend::Opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
    auto& t = g.geometry.triangles;
    t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    t.vertexData.deviceAddress = vtx;
    t.vertexStride = sizeof(Vertex);
    t.maxVertex = maxVertex;
    t.indexType = VK_INDEX_TYPE_UINT32;
    t.indexData.deviceAddress = idx;
    geoms.push_back(g);
    ranges.push_back({m.indexCount / 3, m.firstIndex * uint32_t(sizeof(uint32_t)), uint32_t(m.baseVertex), 0});
    table.push_back({vtx, idx, m.firstIndex, m.baseVertex, m.texture, 0});
}

// Builds (or rebuilds) `out` from geoms/ranges, growing its buffer and `scratch` as needed. Recorded into cb; the
// caller orders it before readers. `out`'s previous structure must no longer be in use.
void Renderer::buildAccel(VkCommandBuffer cb, VkAccelerationStructureTypeKHR type, std::span<const VkAccelerationStructureGeometryKHR> geoms,
                          std::span<const VkAccelerationStructureBuildRangeInfoKHR> ranges, bool fastBuild, Frame::Accel& out,
                          Buffer& scratch) {
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = type;
    bi.flags = fastBuild ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = uint32_t(geoms.size());
    bi.pGeometries = geoms.data();
    std::vector<uint32_t> counts;
    for (const auto& r : ranges) counts.push_back(r.primitiveCount);
    VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkASSizes_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, counts.data(), &sz);
    if (out.as) vkDestroyAS_(device_, out.as, nullptr);
    out.as = VK_NULL_HANDLE;
    growBuffer(out.buf, sz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = out.buf.buf;
    ci.size = sz.accelerationStructureSize;
    ci.type = type;
    check(vkCreateAS_(device_, &ci, nullptr, &out.as), "vkCreateAccelerationStructureKHR");
    growBuffer(scratch, sz.buildScratchSize + scratchAlign_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false,
               &frames_[frame_].garbage);
    bi.dstAccelerationStructure = out.as;
    bi.scratchData.deviceAddress = (address(scratch.buf) + scratchAlign_ - 1) / scratchAlign_ * scratchAlign_;
    const VkAccelerationStructureBuildRangeInfoKHR* pr = ranges.data();
    vkCmdBuildAS_(cb, 1, &bi, &pr);
}

// Ray-traced reflections (Enhanced): neither game has material data beyond texture + blend mode, so reflective surfaces
// are picked from them (docs/ROADMAP.md "Enhanced sun shadows and ray tracing"):
// the surface colour = texture mean (opaque texels) x mean vertex colour x Scene::colorScale (TB textures are grey and
// coloured by their vertices), contrast = the texture's luma standard deviation.
// - water: translucent (Alpha / Additive) or animated surfaces whose colour is blue-dominant -> strength 0.6, roughness
//   0.03 (LiT L02A1 / L03A water, TB VIQ01 ice water);
// - ice: opaque, strongly blue (b > 1.5 r, b > 1.25 g), luma > 0.55, contrast < 0.1 -> strength 0.35, roughness 0.05 (TB VICASG01);
// - polished floor: opaque, luma > 0.6, saturation < 0.25, contrast < 0.1 -> strength 0.25, roughness 0.08 (TB ARCA01
//   marble square, AZBOSS01).
// All only on surfaces facing up (area-weighted mean face normal y > 0.9) covering > 4 character units² of area.
void Renderer::reflectiveMaterials(const Scene& scene) {
    struct Stat { glm::vec3 mean{0.0f}; float sat = 0.0f, contrast = 0.0f; bool ok = false; };
    std::vector<Stat> tex(scene.textures.size());
    auto stat = [&](int32_t t) -> const Stat& {
        static const Stat none;
        if (t < 0 || size_t(t) >= tex.size()) return none;
        Stat& st = tex[size_t(t)];
        const Texture& x = scene.textures[size_t(t)];
        if (st.ok || x.rgba.empty()) return st;
        double sum[3] = {}, sum2 = 0, n = 0;
        for (size_t i = 0; i + 3 < x.rgba.size(); i += 4) {
            if (x.rgba[i + 3] < 128) continue;
            const float r = x.rgba[i] / 255.0f, g = x.rgba[i + 1] / 255.0f, b = x.rgba[i + 2] / 255.0f, l = 0.299f * r + 0.587f * g + 0.114f * b;
            sum[0] += r, sum[1] += g, sum[2] += b, sum2 += double(l) * l, n += 1;
        }
        if (n > 0) {
            st.mean = glm::vec3(float(sum[0] / n), float(sum[1] / n), float(sum[2] / n));
            const float l = 0.299f * st.mean.r + 0.587f * st.mean.g + 0.114f * st.mean.b;
            st.contrast = std::sqrt(std::max(0.0, sum2 / n - double(l) * l));
            st.sat = std::max({st.mean.r, st.mean.g, st.mean.b}) - std::min({st.mean.r, st.mean.g, st.mean.b});
        }
        st.ok = true;
        return st;
    };
    const bool dump = std::getenv("BBTIME_VK_MATERIALS") != nullptr;
    size_t k = 0;
    for (const Mesh& m : scene.meshes) {
        if (m.vertices.empty() || m.indices.empty()) continue;
        GpuMesh& g = meshes_[k++];
        glm::vec3 nsum(0.0f);
        float area = 0.0f;
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            auto p = [&](size_t j) { const Vertex& v = m.vertices[m.indices[i + j]]; return glm::vec3(v.pos[0], v.pos[1], v.pos[2]); };
            const glm::vec3 c = glm::cross(p(1) - p(0), p(2) - p(0));
            nsum += c * 0.5f;
            area += glm::length(c) * 0.5f;
        }
        const float up = area > 0.0f ? (m.twoSided ? std::abs(nsum.y) : nsum.y) / area : 0.0f; // two-sided: either side up
        if (up < 0.9f || area < 4.0f * kCharUnit * kCharUnit || m.name.starts_with("sky")) continue;
        // Animated surfaces: the mean over their frames (some start on an empty frame).
        Stat st;
        int32_t t = m.texture;
        if (m.textureAnim >= 0 && size_t(m.textureAnim) < scene.textureAnims.size() && !scene.textureAnims[size_t(m.textureAnim)].frames.empty()) {
            const auto& frames = scene.textureAnims[size_t(m.textureAnim)].frames;
            t = frames[0];
            for (int32_t f : frames) {
                const Stat& x = stat(f);
                st.mean += x.mean / float(frames.size());
                st.contrast += x.contrast / float(frames.size());
            }
        } else {
            st = stat(t);
        }
        glm::vec3 vc(0.0f); // the surface colour the player sees: texture mean x vertex colour mean (TB textures are grey)
        for (const Vertex& v : m.vertices) vc += glm::vec3(v.rgba[0], v.rgba[1], v.rgba[2]) / 255.0f;
        vc /= float(m.vertices.size());
        const glm::vec3 c = glm::min(st.mean * vc * scene.colorScale, glm::vec3(1.0f));
        const float luma = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
        const float sat = std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b});
        float strength = 0.0f, rough = 0.0f;
        if ((m.blend == Blend::Alpha || m.blend == Blend::Additive || (m.textureAnim >= 0 && m.blend != Blend::Subtract)) && c.b > c.r * 1.15f &&
            c.b > c.g * 0.95f)
            strength = 0.6f, rough = 0.03f;
        else if (m.blend == Blend::Opaque && st.contrast < 0.1f && luma > 0.55f && c.b > c.r * 1.5f && c.b > c.g * 1.25f) strength = 0.35f, rough = 0.05f;
        else if (m.blend == Blend::Opaque && luma > 0.6f && sat < 0.25f && st.contrast < 0.1f) strength = 0.25f, rough = 0.08f;
        if (dump)
            std::println(stderr, "material {} blend {} anim {} area {:.0f} up {:.2f} centre {:.0f} {:.0f} {:.0f} tex {} rgb {:.2f} {:.2f} {:.2f} "
                         "sat {:.2f} contrast {:.3f} -> {}", m.name, int(m.blend), m.textureAnim, area, up, g.center.x, g.center.y, g.center.z,
                         t, c.r, c.g, c.b, sat, st.contrast, strength);
        if (strength > 0.0f) g.material = glm::packHalf2x16(glm::vec2(strength, rough));
    }
}

void Renderer::uploadScene(const Scene& scene) {
    waitIdle();
    freeScene();
    colorScale_ = scene.colorScale;
    texGamma_ = scene.pcTexGamma;
    linearOriginal_ = scene.game == GameId::LostInTime;
    sunPending_ = true;

    std::vector<std::vector<uint8_t>> bled;
    std::vector<TexSrc> srcs;
    std::vector<int> srcOf(scene.textures.size(), -1);
    VkDeviceSize bytes = 0;
    for (size_t i = 0; i < scene.textures.size(); ++i) {
        const Texture& t = scene.textures[i];
        if (!t.width || !t.height || t.rgba.size() != size_t(t.width) * t.height * 4) continue;
        const uint8_t* px = t.rgba.data();
        if (t.hasAlpha) px = bled.emplace_back(render::bleedTransparent(t)).data();
        srcOf[i] = int(srcs.size());
        srcs.push_back({t.width, t.height, px});
        bytes += t.rgba.size();
    }
    for (const Mesh& m : scene.meshes)
        if (!m.vertices.empty() && !m.indices.empty())
            bytes += m.vertices.size() * (sizeof(Vertex) + sizeof(uint32_t)) + m.indices.size() * sizeof(uint32_t);
    if (!bytes) return;

    Buffer staging = makeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    VkCommandBuffer cb = beginOneShot();
    VkDeviceSize off = 0;
    sceneMem_ = createTextures(cb, staging, off, srcs, true, sceneSlots_);
    uint32_t full = 0;
    for (int s : srcOf) {
        const uint32_t slot = s < 0 ? kSlotWhite : sceneSlots_[size_t(s)];
        full += slot == kNoSlot;
        texSlot_.push_back(slot == kNoSlot ? kSlotWhite : slot);
    }
    if (full) std::println(stderr, "vk: bindless table full ({} slots): {} textures drawn white", maxSlots_, full);
    for (const TextureAnim& a : scene.textureAnims) {
        TexAnim& g = texAnims_.emplace_back(TexAnim{{}, a.startFrame, std::max(a.holdTicks, 1u)});
        for (int32_t f : a.frames) g.frames.push_back(f >= 0 && size_t(f) < texSlot_.size() ? texSlot_[size_t(f)] : kSlotWhite);
    }

    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    casterLo_ = glm::vec3(std::numeric_limits<float>::max());
    casterHi_ = -casterLo_;
    addMeshes(scene.meshes, false, SIZE_MAX, verts, idx);
    sceneMeshCount_ = meshes_.size();
    zones_ = scene.zones;
    reflectiveMaterials(scene);
    std::vector<TriRange> ranges;
    for (const GpuMesh& m : meshes_)
        if (m.blend != Blend::Additive && m.blend != Blend::Subtract) ranges.push_back({m.firstIndex, m.indexCount, m.baseVertex});
    const std::vector<uint32_t> normals = smoothNormals(verts, idx, ranges);
    const VkDeviceSize vBytes = verts.size() * sizeof(Vertex), iBytes = idx.size() * sizeof(uint32_t), nBytes = normals.size() * sizeof(uint32_t);
    Buffer scratch;
    if (vBytes) {
        const VkBufferUsageFlags rtUse = rt_ ? kRtInput : 0;
        sceneVbo_ = makeBuffer(vBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rtUse, false);
        sceneIbo_ = makeBuffer(iBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rtUse, false);
        std::memcpy(static_cast<uint8_t*>(staging.map) + off, verts.data(), vBytes);
        std::memcpy(static_cast<uint8_t*>(staging.map) + off + vBytes, idx.data(), iBytes);
        sceneNrm_ = makeBuffer(nBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        std::memcpy(static_cast<uint8_t*>(staging.map) + off + vBytes + iBytes, normals.data(), nBytes);
        const VkBufferCopy cv{off, 0, vBytes}, ci{off + vBytes, 0, iBytes}, cn{off + vBytes + iBytes, 0, nBytes};
        vkCmdCopyBuffer(cb, staging.buf, sceneVbo_.buf, 1, &cv);
        vkCmdCopyBuffer(cb, staging.buf, sceneIbo_.buf, 1, &ci);
        vkCmdCopyBuffer(cb, staging.buf, sceneNrm_.buf, 1, &cn);
        memoryBarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | (rt_ ? VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : 0),
                      VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | (rt_ ? VK_ACCESS_2_SHADER_READ_BIT : 0));
        if (rt_) { // static scene BLAS: one geometry per shadow-casting mesh, built once per level
            std::vector<VkAccelerationStructureGeometryKHR> geoms;
            std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
            const VkDeviceAddress va = address(sceneVbo_.buf), ia = address(sceneIbo_.buf);
            for (const GpuMesh& m : meshes_)
                if (m.caster) blasGeometry(m, va, ia, uint32_t(verts.size() - 1), geoms, ranges, sceneGeos_);
            if (!geoms.empty()) {
                buildAccel(cb, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geoms, ranges, false, sceneBlas_, scratch);
                memoryBarrier(cb, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                              VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
            }
        }
    }
    endOneShot(cb);
    destroy(staging);
    destroy(scratch);
}

uint32_t Renderer::createImage(const Texture& t) {
    if (!device_ || !t.width || !t.height || t.rgba.size() != size_t(t.width) * t.height * 4) return 0;
    Buffer staging = makeBuffer(t.rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    VkCommandBuffer cb = beginOneShot();
    VkDeviceSize off = 0;
    std::vector<uint32_t> slot;
    const TexSrc src{t.width, t.height, t.rgba.data()};
    VkDeviceMemory mem = createTextures(cb, staging, off, {&src, 1}, false, slot);
    endOneShot(cb);
    destroy(staging);
    if (slot[0] == kNoSlot) {
        vkFreeMemory(device_, mem, nullptr);
        return 0;
    }
    slots_[slot[0]].mem = mem;
    return slot[0];
}

void Renderer::destroyImage(uint32_t image) {
    if (!image || image >= slots_.size() || !slots_[image].img) return;
    waitIdle();
    freeSlot(image);
}

// ---------------------------------------------------------------- frame

void Renderer::beginFrame(const Settings& s) {
    SDL_GetWindowSizeInPixels(window_, &w_, &h_);
    drawCalls_ = 0;
    batch_.clear();
    recording_ = submitted_ = passOpen_ = genPending_ = fsrScene_ = sceneDrawn_ = false;
    uiDraws_.clear();
    if (w_ <= 0 || h_ <= 0) return;
    // FSR (Enhanced, not the 4:3 3-D menus): upscaling, or frame generation alone (= FSR native AA, which produces the
    // dilated depth / motion frame interpolation needs).
    fsrMode_ = fsrOk_ && s.fidelity == Fidelity::Enhanced && !s.frame43 && (s.upscaling != Upscaling::Off || s.frameGeneration);
    fgMode_ = fsrMode_ && s.frameGeneration;
    if (presentStale_.exchange(false)) swapDirty_ = true;
    if (!fgMode_) stopPresenter(); // the swapchain returns to this thread
    if (swapDirty_ || w_ != swapW_ || h_ != swapH_ || vsync_ != swapVsync_) recreateSwapchain();
    if (swapDirty_) return;
    if (fsrMode_) updateFsr(s);
    if (fgMode_) startPresenter();

    Frame& f = frames_[frame_];
    if (fgMode_) { // this slot's images are free once the present thread copied them out
        std::unique_lock lock(jobMutex_);
        jobCv_.wait(lock, [&] { return jobsDone_ >= slotJob_[frame_] || presenterError_; });
        if (presenterError_) std::rethrow_exception(std::exchange(presenterError_, nullptr));
    }
    for (int k = 0; k < 2; ++k) // the present thread's copies out of this slot (also after frame generation ended)
        if (f.copyPending[k]) {
            check(vkWaitForFences(device_, 1, &f.copyFence[k], VK_TRUE, UINT64_MAX), "vkWaitForFences(copy)");
            f.copyPending[k] = false;
        }
    check(vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    for (Buffer& b : f.garbage) destroy(b);
    f.garbage.clear();
    f.used = 0;
    if (fgMode_) { // frame generation: render into the slot's real-frame image, the present thread shows it
        target_ = f.realUi.img;
        targetView_ = f.realUi.view;
    } else {
        const VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &imageIndex_);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            swapDirty_ = true; // skip this frame, the next beginFrame recreates
            return;
        }
        if (r == VK_SUBOPTIMAL_KHR) swapDirty_ = true;
        else check(r, "vkAcquireNextImageKHR");
        target_ = swapImages_[imageIndex_];
        targetView_ = swapViews_[imageIndex_];
    }
    check(vkResetFences(device_, 1, &f.fence), "vkResetFences");
    check(vkResetCommandBuffer(f.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(f.cmd, &bi), "vkBeginCommandBuffer");
    w_ = int(extent_.width);
    h_ = int(extent_.height);

    msaaOn_ = !fsrMode_ && s.fidelity == Fidelity::Enhanced && s.msaa && msaaMax_ > VK_SAMPLE_COUNT_1_BIT; // FSR is the AA
    const Image& depth = msaaOn_ ? msaaDepth_ : depth_;
    constexpr auto COLOR_OUT = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    constexpr auto COLOR_W = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    constexpr auto DEPTH_STAGES = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    constexpr auto DEPTH_RW = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    imageBarrier(f.cmd, target_, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 COLOR_OUT | VK_PIPELINE_STAGE_2_COPY_BIT, 0, COLOR_OUT, COLOR_W);
    imageBarrier(f.cmd, depth.img, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 DEPTH_STAGES, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, DEPTH_STAGES, DEPTH_RW);
    if (msaaOn_)
        imageBarrier(f.cmd, msaaColor_.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, COLOR_OUT, COLOR_W, COLOR_OUT, COLOR_W);

    beginPass(true);
    recording_ = true;
}

// Opens the frame's dynamic-rendering pass on its target (swapchain image or Frame::realUi; resolved from the MSAA target when on). clear: the
// frame's first pass; else a resume after drawScene's shadow / acceleration-structure work: colour is kept (the MSAA
// target is always stored for that), depth is cleared (drawScene clears its viewport anyway).
void Renderer::beginPass(bool clear) {
    VkCommandBuffer cb = frames_[frame_].cmd;
    const Image& depth = msaaOn_ ? msaaDepth_ : depth_;
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = msaaOn_ ? msaaColor_.view : targetView_;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    if (msaaOn_) {
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = targetView_;
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = depth.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depthAtt;
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    passOpen_ = true;
}

Renderer::Image Renderer::makeImage(VkExtent2D ext, VkFormat fmt, VkImageUsageFlags usage) {
    Image im;
    im.img = newImage(ext.width, ext.height, fmt, VK_SAMPLE_COUNT_1_BIT, usage, 1);
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(device_, im.img, &r);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = memoryType(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device_, &ai, nullptr, &im.mem), "vkAllocateMemory(FSR)");
    check(vkBindImageMemory(device_, im.img, im.mem, 0), "vkBindImageMemory(FSR)");
    im.view = newView(im.img, fmt, fmt == kDepthFormat ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 1);
    return im;
}

void Renderer::destroyFsrTargets() {
    for (Image* i : {&fsrColor_, &fsrMotion_, &fsrReactive_, &fsrDepth_, &fsrDilatedDepth_, &fsrDilatedMotion_, &fsrPrevDepth_, &fsrOutput_, &genImage_})
        destroy(*i);
    for (Frame& f : frames_) destroy(f.genUi), destroy(f.realUi);
    fsrRender_ = fsrDisplay_ = {};
}

namespace {
// FSR image usages (Fsr::Image::usage must match the images' create info).
constexpr VkImageUsageFlags kFsrInput = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageUsageFlags kFsrDepth = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageUsageFlags kFsrShared = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkImageUsageFlags kFsrDisplay = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
constexpr VkFormat kFsrDisplayFormat = VK_FORMAT_R8G8B8A8_UNORM;
} // namespace

// (Re)creates the FSR targets and contexts when the window size, quality mode or frame generation changed.
void Renderer::updateFsr(const Settings& s) {
    const Upscaling q = s.upscaling == Upscaling::Off ? Upscaling::NativeAa : s.upscaling;
    if (fsr_->created() && fsrScale_ == q && fsrFg_ == fgMode_ && fsrDisplay_.width == extent_.width && fsrDisplay_.height == extent_.height)
        return;
    waitIdle();
    fsr_->destroy();
    destroyFsrTargets();
    const float ratio = q == Upscaling::Quality ? 1.5f : q == Upscaling::Balanced ? 1.7f : q == Upscaling::Performance ? 2.0f : 1.0f;
    fsrDisplay_ = extent_;
    fsrRender_ = {std::max(1u, uint32_t(float(extent_.width) / ratio + 0.5f)), std::max(1u, uint32_t(float(extent_.height) / ratio + 0.5f))};
    fsrColor_ = makeImage(fsrRender_, swapFormat_, kFsrInput);
    fsrMotion_ = makeImage(fsrRender_, VK_FORMAT_R16G16_SFLOAT, kFsrInput);
    fsrReactive_ = makeImage(fsrRender_, VK_FORMAT_R8_UNORM, kFsrInput);
    fsrDepth_ = makeImage(fsrRender_, kDepthFormat, kFsrDepth);
    fsrDilatedDepth_ = makeImage(fsrRender_, VK_FORMAT_R32_SFLOAT, kFsrShared);
    fsrDilatedMotion_ = makeImage(fsrRender_, VK_FORMAT_R16G16_SFLOAT, kFsrShared);
    fsrPrevDepth_ = makeImage(fsrRender_, VK_FORMAT_R32_UINT, kFsrShared);
    fsrOutput_ = makeImage(fsrDisplay_, kFsrDisplayFormat, kFsrDisplay);
    if (fgMode_) {
        genImage_ = makeImage(fsrDisplay_, kFsrDisplayFormat, kFsrDisplay);
        for (Frame& f : frames_)
            for (Image* i : {&f.genUi, &f.realUi})
                *i = makeImage(fsrDisplay_, swapFormat_,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }
    // The shared upscaler resources stay in GENERAL (FidelityFX UNORDERED_ACCESS) between dispatches.
    VkCommandBuffer cb = beginOneShot();
    for (const Image* i : {&fsrDilatedDepth_, &fsrDilatedMotion_, &fsrPrevDepth_})
        imageBarrier(cb, i->img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    endOneShot(cb);
    fsr_->create(fsrRender_, fsrDisplay_, fgMode_, kFsrDisplayFormat);
    fsrScale_ = q;
    fsrFg_ = fgMode_;
    fsrReset_ = true;
    diag_.clear(); // drawScene rewrites the overlay line
}

// End of an FSR scene: upscale into fsrOutput_, interpolate the in-between frame (frame generation), copy the
// upscaled frame into the swapchain image and reopen the frame's pass there for the 2D layer.
void Renderer::fsrResolve(const Settings& s) {
    VkCommandBuffer cb = frames_[frame_].cmd;
    constexpr auto C = VK_IMAGE_ASPECT_COLOR_BIT;
    constexpr auto COLOR_OUT = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    constexpr auto CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr auto COPY = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
    constexpr auto READ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdEndRendering(cb);
    for (const Image* i : {&fsrColor_, &fsrMotion_, &fsrReactive_})
        imageBarrier(cb, i->img, C, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, READ, COLOR_OUT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    imageBarrier(cb, fsrDepth_.img, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, READ,
                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // Previous frame's blit / interpolation read fsrOutput_ (and wrote genImage_): this frame overwrites them.
    imageBarrier(cb, fsrOutput_.img, C, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, COPY | CS, 0, CS,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const uint64_t now = SDL_GetTicksNS();
    Fsr::Upscale u;
    auto img = [](const Image& i, VkFormat f, VkExtent2D e, VkImageUsageFlags use) { return Fsr::Image{i.img, f, e, use}; };
    u.color = img(fsrColor_, swapFormat_, fsrRender_, kFsrInput);
    u.motion = img(fsrMotion_, VK_FORMAT_R16G16_SFLOAT, fsrRender_, kFsrInput);
    u.reactive = img(fsrReactive_, VK_FORMAT_R8_UNORM, fsrRender_, kFsrInput);
    u.depth = img(fsrDepth_, kDepthFormat, fsrRender_, kFsrDepth);
    u.dilatedDepth = img(fsrDilatedDepth_, VK_FORMAT_R32_SFLOAT, fsrRender_, kFsrShared);
    u.dilatedMotion = img(fsrDilatedMotion_, VK_FORMAT_R16G16_SFLOAT, fsrRender_, kFsrShared);
    u.prevDepth = img(fsrPrevDepth_, VK_FORMAT_R32_UINT, fsrRender_, kFsrShared);
    u.output = img(fsrOutput_, kFsrDisplayFormat, fsrDisplay_, kFsrDisplay);
    u.jitterX = fsrJitter_[0];
    u.jitterY = fsrJitter_[1];
    u.frameMs = fsrLastNs_ && !fsrReset_ ? std::clamp(float(double(now - fsrLastNs_) * 1e-6), 0.1f, 1000.0f) : 16.7f;
    u.zNear = s.zNear;
    u.zFar = s.zFar;
    u.vfovRad = s.vfovRad();
    u.reset = fsrReset_;
    u.sharpness = s.upscaling == Upscaling::Off || s.upscaling == Upscaling::NativeAa ? 0.0f : 0.3f; // RCAS against the upscale blur
    fsr_->upscale(cb, u);
    fsrLastNs_ = now;
    fsrReset_ = false;
    imageBarrier(cb, fsrOutput_.img, C, 0, 1, VK_IMAGE_LAYOUT_GENERAL, READ, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS | COPY,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    if (fgMode_ && fsr_->generation()) {
        imageBarrier(cb, genImage_.img, C, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, COPY | CS, 0, CS,
                     VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        const bool valid = fsr_->interpolate(cb, img(fsrOutput_, kFsrDisplayFormat, fsrDisplay_, kFsrDisplay),
                                             img(genImage_, kFsrDisplayFormat, fsrDisplay_, kFsrDisplay), u);
        if (valid) { // the generated frame, with this frame's 2D layer added at submit (Frame::genUi)
            imageBarrier(cb, genImage_.img, C, 0, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, CS,
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, COPY, VK_ACCESS_2_TRANSFER_READ_BIT);
            const Image& genUi = frames_[frame_].genUi;
            imageBarrier(cb, genUi.img, C, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, COPY | COLOR_OUT, 0, COPY,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT);
            const VkImageBlit blit{{C, 0, 0, 1}, {{0, 0, 0}, {int32_t(fsrDisplay_.width), int32_t(fsrDisplay_.height), 1}},
                                   {C, 0, 0, 1}, {{0, 0, 0}, {int32_t(fsrDisplay_.width), int32_t(fsrDisplay_.height), 1}}};
            vkCmdBlitImage(cb, genImage_.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, genUi.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_NEAREST);
            imageBarrier(cb, genUi.img, C, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, COPY,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, COLOR_OUT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            genPending_ = true;
        }
    }
    // Upscaled frame -> swapchain image (format conversion by the blit), then the frame's pass resumes on it.
    VkImage swap = target_;
    imageBarrier(cb, fsrOutput_.img, C, 0, 1, READ, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, CS, 0, COPY, VK_ACCESS_2_TRANSFER_READ_BIT);
    imageBarrier(cb, swap, C, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, COLOR_OUT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, COPY, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkImageBlit blit{{C, 0, 0, 1}, {{0, 0, 0}, {int32_t(fsrDisplay_.width), int32_t(fsrDisplay_.height), 1}},
                           {C, 0, 0, 1}, {{0, 0, 0}, {int32_t(extent_.width), int32_t(extent_.height), 1}}};
    vkCmdBlitImage(cb, fsrOutput_.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    imageBarrier(cb, swap, C, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, COPY,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, COLOR_OUT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    beginPass(false);
}

void Renderer::drawScene(const Camera& cam, const Settings& s) {
    if (meshes_.empty() || !recording_) return;
    flush2D();
    sceneDrawn_ = true;
    Frame& f = frames_[frame_];
    VkCommandBuffer cb = f.cmd;
    const bool original = s.fidelity == Fidelity::Original;
    if (sunPending_) { // camera yaw 0 looks down -z: "behind" = (sin yaw, 0, cos yaw)
        const float yaw = cam.yaw + kSunSide;
        toSun_ = glm::vec3(std::sin(yaw) * std::cos(kSunElevation), std::sin(kSunElevation), std::cos(yaw) * std::cos(kSunElevation));
        sunPending_ = false;
    }
    const bool enhanced = s.fidelity == Fidelity::Enhanced && !s.frame43;
    Shadows mode = dynamicShadows(s) ? s.shadows : Shadows::Off;
    if (mode >= Shadows::Rt && !rt_) mode = Shadows::Map; // graceful fallback: no ray queries on this device
    const bool refl = enhanced && s.reflections && rt_;    // ray-traced reflections need ray queries (else off)
    const bool rtPath = rt_ && (mode >= Shadows::Rt || refl);
    const bool lit = mode != Shadows::Off || refl;
    const bool fsr = fsrMode_ && fsr_->created();          // beginFrame: Enhanced, !frame43, FSR wanted and usable
    if (mode != drawnMode_ || refl != reflectionsOn_ || fsr != fsrScene_ || diag_.empty()) {
        static constexpr const char* kNames[] = {"off", "shadow map (4 cascades)", "ray-query", "ray-query + RT AO"};
        drawnMode_ = mode;
        reflectionsOn_ = refl;
        diag_ = std::format("{} | {} | shadows {}  reflections {}", rtProbe_, fsrProbe_, kNames[int(mode)], refl ? "RT" : "off");
        if (fsr)
            diag_ += std::format("\nFSR {}x{} -> {}x{}  frame generation {}", fsrRender_.width, fsrRender_.height, fsrDisplay_.width,
                                 fsrDisplay_.height, fgMode_ ? "on (2 presents per frame)" : "off");
    }
    fsrScene_ = fsr;
    // FSR renders the full window at the render resolution (frame43 / pillarbox never apply: Enhanced, !frame43).
    const render::Viewport vp = fsr ? render::Viewport{0, 0, int(fsrRender_.width), int(fsrRender_.height)}
                                : (original && s.pillarbox) || s.frame43 ? render::fitAspect(w_, h_, 4.0f / 3.0f)
                                                                         : render::Viewport{0, 0, w_, h_};
    const int top = (fsr ? vp.h : h_) - vp.y - vp.h; // fitAspect is bottom-left based like GL; Vulkan is top-left
    const VkRect2D rect{{vp.x, top}, {uint32_t(vp.w), uint32_t(vp.h)}};

    glm::mat4 clipFix(1.0f); // GL clip z [-w, w] -> Vulkan [0, w]
    clipFix[2][2] = 0.5f;
    clipFix[3][2] = 0.5f;
    const glm::mat4 view = cam.view();
    const glm::mat4 proj = render::projection(s.vfovRad(), float(vp.w) / float(vp.h), s.zNear, s.zFar);
    Push3D pc{};
    const glm::mat4 viewProj = clipFix * proj * view;
    const bool zoned = render::walkZones(zones_, proj * view, zoneView_, zoneTable_, zoneDrawn_);
    auto shown = [&](const GpuMesh& m) { return !zoned || m.zone < 0 || zoneDrawn_[size_t(m.zone)]; };
    pc.viewProj = viewProj;
    float jitter[2] = {0.0f, 0.0f};
    if (fsr) { // sub-pixel jitter of FSR's sequence, as a clip-space translation (NDC y up = pixel y down)
        Fsr::jitter(fsrFrame_++, fsrRender_, fsrDisplay_, jitter[0], jitter[1]);
        fsrJitter_[0] = jitter[0];
        fsrJitter_[1] = jitter[1];
        const glm::mat4 j = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f * jitter[0] / float(vp.w), -2.0f * jitter[1] / float(vp.h), 0.0f));
        pc.viewProj = clipFix * j * proj * view;
    }
    pc.viewZ = glm::vec4(view[0][2], view[1][2], view[2][2], view[3][2]);
    pc.fogColorScale = glm::vec4(s.fogColor[0], s.fogColor[1], s.fogColor[2], s.colorScale(colorScale_));
    pc.texGamma = s.texGamma(texGamma_);
    pc.fog = s.fog ? glm::vec2(s.fogStart, s.fogEnd) : glm::vec2(0.0f);
    pc.samp = original ? (linearOriginal_ ? kSampLinear : kSampNearest) : kSampTrilinear;
    // Scene geometry lives in device-local buffers; actor geometry is streamed into this frame's ring.
    VkBuffer geomV[2] = {sceneVbo_.buf, VK_NULL_HANDLE}, geomI[2] = {sceneIbo_.buf, VK_NULL_HANDLE};
    VkDeviceSize offV[2] = {0, 0}, offI[2] = {0, 0};
    // Lit / motion-vector pipelines: binding 1 smooth normals, binding 2 previous positions (scene: static = itself).
    VkBuffer geomN[2] = {sceneNrm_.buf, VK_NULL_HANDLE}, geomP[2] = {sceneVbo_.buf, VK_NULL_HANDLE};
    VkDeviceSize offN[2] = {0, 0}, offP[2] = {0, 0};
    if (!actorIdx_.empty()) {
        std::tie(geomV[1], offV[1]) = streamWrite(actorVerts_.data(), actorVerts_.size() * sizeof(Vertex));
        std::tie(geomI[1], offI[1]) = streamWrite(actorIdx_.data(), actorIdx_.size() * sizeof(uint32_t));
        if (lit || fsr) {
            if (actorNrm_.empty()) {
                std::vector<TriRange> ranges;
                for (size_t i = sceneMeshCount_; i < meshes_.size(); ++i)
                    if (meshes_[i].blend != Blend::Additive && meshes_[i].blend != Blend::Subtract)
                        ranges.push_back({meshes_[i].firstIndex, meshes_[i].indexCount, meshes_[i].baseVertex});
                actorNrm_ = smoothNormals(actorVerts_, actorIdx_, ranges);
            }
            std::tie(geomN[1], offN[1]) = streamWrite(actorNrm_.data(), actorNrm_.size() * sizeof(uint32_t));
        }
        if (fsr) { // same posed models as last frame: their previous vertices, else no actor motion
            const std::vector<Vertex>& prev = prevActorLayout_ == actorLayout_ ? prevActorVerts_ : actorVerts_;
            std::tie(geomP[1], offP[1]) = streamWrite(prev.data(), prev.size() * sizeof(Vertex));
        }
    }
    const uint32_t bindings = fsr ? 3 : lit ? 2 : 1;
    int boundGeom = -1;
    auto bindGeom = [&](const GpuMesh& m) {
        if (int g = m.actor; g != boundGeom) {
            const VkBuffer bufs[3] = {geomV[g], geomN[g], geomP[g]};
            const VkDeviceSize offs[3] = {offV[g], offN[g], offP[g]};
            vkCmdBindVertexBuffers(cb, 0, bindings, bufs, offs);
            vkCmdBindIndexBuffer(cb, geomI[g], offI[g], VK_INDEX_TYPE_UINT32);
            boundGeom = g;
        }
    };

    // Shadow map, acceleration structures and FSR's offscreen targets: recorded outside the frame's render pass.
    const bool outside = mode == Shadows::Map || rtPath || fsr;
    if (outside) {
        vkCmdEndRendering(cb);
        passOpen_ = false;
    }
    if (lit || fsr) {
        LightUbo L{};
        L.curViewProj = viewProj;
        L.prevViewProj = prevValid_ && !fsrReset_ ? prevViewProj_ : viewProj;
        prevViewProj_ = viewProj;
        prevValid_ = true;
        L.reflect = refl;
        L.temporal = fsr; // per-pixel random AO / rough-reflection rays only where FSR's history averages them
        const bool haveCasters = casterLo_.x <= casterHi_.x;
        const glm::vec3 sceneMid = haveCasters ? (casterLo_ + casterHi_) * 0.5f : cam.pos;
        const float sceneRadius = (haveCasters ? glm::length(casterHi_ - casterLo_) * 0.5f : 0.0f) + 4.0f * kCharUnit;
        L.sun = glm::vec4(toSun_, kShadowBrightness);
        L.eye = glm::vec4(cam.pos, 1.0f);
        L.rt = glm::vec4(kRayOffset, 2.0f * sceneRadius + glm::length(cam.pos - sceneMid), kAoRadius, kAoStrength);
        L.mode = uint32_t(mode);
        L.rayMin = kRayMin;
        const float far = std::max(s.fog && s.fogEnd > s.fogStart ? std::min(s.fogEnd, s.zFar) : s.zFar, s.zNear * 2.0f);
        for (uint32_t c = 0; c < kCascades; ++c) L.splits[int(c)] = std::min(kCascadeEnd[c], far);

        if (mode == Shadows::Map) {
            // Cascades: bounding sphere of each view-frustum slice (rotation-invariant radius), light-space centre snapped
            // to whole texels so shadows do not shimmer while the camera moves; depth spans every caster toward the sun.
            const glm::mat4 lightView = glm::lookAt(glm::vec3(0.0f), -toSun_, glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::mat4 inv = glm::inverse(view);
            const float tanH = 1.0f / proj[0][0], tanV = 1.0f / proj[1][1];
            float casterTop = -std::numeric_limits<float>::max();
            for (int k = 0; haveCasters && k < 8; ++k) {
                const glm::vec3 corner((k & 1) ? casterHi_.x : casterLo_.x, (k & 2) ? casterHi_.y : casterLo_.y, (k & 4) ? casterHi_.z : casterLo_.z);
                casterTop = std::max(casterTop, (lightView * glm::vec4(corner, 1.0f)).z);
            }
            imageBarrier(cb, shadowMap_.img, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kCascades);
            Push3D sp{};
            sp.samp = kSampTrilinear;
            for (uint32_t c = 0; c < kCascades; ++c) {
                const float d0 = c ? L.splits[int(c) - 1] : s.zNear, d1 = std::max(L.splits[int(c)], d0 + 1.0f);
                glm::vec3 corners[8], centre(0.0f);
                for (int k = 0; k < 8; ++k) {
                    const float d = (k & 4) ? d1 : d0;
                    corners[k] = glm::vec3(inv * glm::vec4(((k & 1) ? 1.0f : -1.0f) * tanH * d, ((k & 2) ? 1.0f : -1.0f) * tanV * d, -d, 1.0f));
                    centre += corners[k] * 0.125f;
                }
                float r = 0.0f;
                for (const glm::vec3& p : corners) r = std::max(r, glm::length(p - centre));
                r = std::ceil(r / 16.0f) * 16.0f;
                const float texel = 2.0f * r / float(kShadowSize);
                glm::vec3 lc = glm::vec3(lightView * glm::vec4(centre, 1.0f));
                lc.x = std::floor(lc.x / texel) * texel;
                lc.y = std::floor(lc.y / texel) * texel;
                const float zTop = std::max(casterTop, lc.z + r) + kCharUnit, zBottom = lc.z - r - kCharUnit;
                L.cascade[c] = clipFix * glm::ortho(lc.x - r, lc.x + r, lc.y - r, lc.y + r, -zTop, -zBottom) * lightView;
                L.texel[int(c)] = texel;

                VkRenderingAttachmentInfo d{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                d.imageView = shadowLayers_[c];
                d.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                d.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                d.clearValue.depthStencil = {1.0f, 0};
                VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                ri.renderArea = {{0, 0}, {kShadowSize, kShadowSize}};
                ri.layerCount = 1;
                ri.pDepthAttachment = &d;
                vkCmdBeginRendering(cb, &ri);
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeShadow_);
                const VkViewport svp{0.0f, 0.0f, float(kShadowSize), float(kShadowSize), 0.0f, 1.0f};
                vkCmdSetViewport(cb, 0, 1, &svp);
                vkCmdSetScissor(cb, 0, 1, &ri.renderArea);
                vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
                sp.viewProj = L.cascade[c];
                vkCmdPushConstants(cb, layout_, kPushStages, 0, sizeof sp, &sp);
                for (const GpuMesh& m : meshes_) {
                    if (!m.caster) continue;
                    bindGeom(m);
                    sp.alphaRef = m.blend == Blend::AlphaTest ? 0.5f : -1.0f;
                    sp.tex = m.texture;
                    vkCmdPushConstants(cb, layout_, kPushStages, offsetof(Push3D, alphaRef), 16, &sp.alphaRef);
                    vkCmdDrawIndexed(cb, m.indexCount, 1, m.firstIndex, m.baseVertex, 0);
                }
                vkCmdEndRendering(cb);
            }
            imageBarrier(cb, shadowMap_.img, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kCascades);
        }
        if (rtPath) {
            // Ray tracing: actor BLAS (posed this frame) + TLAS {scene BLAS, actor BLAS}, rebuilt every frame.
            // Last use of this frame slot's structures / scratch: the fragment shaders and builds two frames ago.
            memoryBarrier(cb, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                          VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                          VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
            std::vector<VkAccelerationStructureGeometryKHR> geoms;
            std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
            actorGeos_.clear();
            if (!actorIdx_.empty()) {
                const VkDeviceAddress va = address(geomV[1]) + offV[1], ia = address(geomI[1]) + offI[1];
                for (size_t i = sceneMeshCount_; i < meshes_.size(); ++i)
                    if (meshes_[i].caster) blasGeometry(meshes_[i], va, ia, uint32_t(actorVerts_.size() - 1), geoms, ranges, actorGeos_);
            }
            if (!geoms.empty()) {
                buildAccel(cb, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geoms, ranges, true, f.actorBlas, f.scratch);
                memoryBarrier(cb, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                              VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                              VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
            }
            VkAccelerationStructureInstanceKHR inst[2]{};
            uint32_t n = 0;
            auto instance = [&](VkAccelerationStructureKHR as, uint32_t firstGeo) {
                VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
                ai.accelerationStructure = as;
                VkAccelerationStructureInstanceKHR& x = inst[n++];
                x.transform.matrix[0][0] = x.transform.matrix[1][1] = x.transform.matrix[2][2] = 1.0f;
                x.instanceCustomIndex = firstGeo; // scene.frag: Geo index = custom index + geometry index
                x.mask = 0xFF;
                x.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
                x.accelerationStructureReference = vkASAddress_(device_, &ai);
            };
            if (sceneBlas_.as) instance(sceneBlas_.as, 0);
            if (!geoms.empty()) instance(f.actorBlas.as, uint32_t(sceneGeos_.size()));
            growBuffer(f.instances, sizeof inst,
                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
            std::memcpy(f.instances.map, inst, sizeof inst);
            VkAccelerationStructureGeometryKHR tg{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            tg.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            tg.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
            tg.geometry.instances.data.deviceAddress = address(f.instances.buf);
            const VkAccelerationStructureBuildRangeInfoKHR tr{n, 0, 0, 0};
            buildAccel(cb, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, {&tg, 1}, {&tr, 1}, true, f.tlas, f.scratch);
            memoryBarrier(cb, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);

            const size_t geoBytes = (sceneGeos_.size() + actorGeos_.size()) * sizeof(RtGeo);
            growBuffer(f.geos, std::max<size_t>(geoBytes, sizeof(RtGeo)), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            if (!sceneGeos_.empty()) std::memcpy(f.geos.map, sceneGeos_.data(), sceneGeos_.size() * sizeof(RtGeo));
            if (!actorGeos_.empty())
                std::memcpy(static_cast<RtGeo*>(f.geos.map) + sceneGeos_.size(), actorGeos_.data(), actorGeos_.size() * sizeof(RtGeo));
            VkWriteDescriptorSetAccelerationStructureKHR wa{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
            wa.accelerationStructureCount = 1;
            wa.pAccelerationStructures = &f.tlas.as;
            const VkDescriptorBufferInfo gi{f.geos.buf, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &wa}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
            w[0].dstSet = w[1].dstSet = f.lightSet;
            w[0].descriptorCount = w[1].descriptorCount = 1;
            w[0].dstBinding = 2;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            w[1].dstBinding = 3;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[1].pBufferInfo = &gi;
            vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
        }
        std::memcpy(f.light.map, &L, sizeof L);
    }
    const VkClearColorValue fogClear{{s.fogColor[0], s.fogColor[1], s.fogColor[2], 1.0f}};
    if (fsr) { // the scene at render resolution into colour + motion + depth (cleared by the pass)
        constexpr auto COLOR_OUT = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        constexpr auto DEPTH = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        // Previous frame's readers: FSR's compute passes.
        for (const Image* i : {&fsrColor_, &fsrMotion_, &fsrReactive_})
            imageBarrier(cb, i->img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, COLOR_OUT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        imageBarrier(cb, fsrDepth_.img, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, DEPTH,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo col[3]{{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},
                                         {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
        col[0].imageView = fsrColor_.view;
        col[1].imageView = fsrMotion_.view;
        col[2].imageView = fsrReactive_.view;
        for (VkRenderingAttachmentInfo& a : col) {
            a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        col[0].clearValue.color = fogClear;
        VkRenderingAttachmentInfo dep{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        dep.imageView = fsrDepth_.view;
        dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        dep.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        dep.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        dep.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, fsrRender_};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 3;
        ri.pColorAttachments = col;
        ri.pDepthAttachment = &dep;
        vkCmdBeginRendering(cb, &ri);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    } else if (outside) {
        // Resume the frame's pass: colour (incl. the MSAA resolve) and depth of the first pass come before its loads.
        memoryBarrier(cb, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        beginPass(false);
    }
    if (lit || fsr) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 1, 1, &f.lightSet, 0, nullptr);
    boundGeom = -1;

    if (!fsr) {
        VkClearAttachment clears[2]{};
        clears[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clears[0].clearValue.color = fogClear;
        clears[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        clears[1].clearValue.depthStencil = {1.0f, 0};
        const VkClearRect clearRect{rect, 0, 1};
        vkCmdClearAttachments(cb, 2, clears, 1, &clearRect);
    }
    // Negative height flips y so the GL-convention projection (camera.h) and CCW front faces carry over.
    const VkViewport viewport{float(vp.x), float(top + vp.h), float(vp.w), -float(vp.h), 0.0f, 1.0f};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &rect);
    vkCmdPushConstants(cb, layout_, kPushStages, 0, sizeof pc, &pc);

    VkPipeline bound = VK_NULL_HANDLE;
    auto draw = [&](const GpuMesh& m) {
        // Original Fidelity draws ABR 2 the way the PC exe's GL path does (luma alpha, SRC_ALPHA / 1-SRC_ALPHA), and
        // LiT's textured ABR 0/1/3 as bugs.exe's per-mode texel alpha (0x420d10), also alpha-blended.
        const bool pcSub = original && m.blend == Blend::Subtract;
        const bool pcSemi = original && linearOriginal_ && (m.blend == Blend::Alpha || m.blend == Blend::Additive) &&
                            (m.texture != kSlotWhite || m.textureAnim >= 0);
        const int b = m.blend == Blend::Alpha || pcSub || pcSemi ? 1 : m.blend == Blend::Additive ? 2 : m.blend == Blend::Subtract ? 3 : 0;
        const int kind = !lit ? 0 : rtPath ? 2 : 1;
        const VkPipeline p = fsr ? pipeLit_[kind][1][0][b] : kind == 0 ? pipe3D_[msaaOn_][b] : pipeLit_[kind][0][msaaOn_][b];
        if (p != bound) vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = p);
        bindGeom(m);
        vkCmdSetCullMode(cb, m.twoSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
        pc.alphaRef = m.blend == Blend::AlphaTest ? 0.5f : -1.0f;
        pc.flags = pcSemi ? 64u | 16u : uint32_t(m.blend == Blend::Additive || m.blend == Blend::Subtract) | (pcSub ? 2u : 0u);
        if (mode != Shadows::Off && m.receiver) pc.flags |= 4u | (m.blend != Blend::Alpha ? 8u : 0u);
        pc.material = refl ? m.material : 0u;
        if (m.blend == Blend::Alpha) pc.flags |= 16u;
        if (m.reactive) pc.flags |= 32u;
        if (m.textureAnim >= 0) {
            const TexAnim& a = texAnims_[size_t(m.textureAnim)];
            pc.tex = a.frames[(a.start + texTick_ / a.hold) % a.frames.size()];
        } else {
            pc.tex = m.texture;
        }
        vkCmdPushConstants(cb, layout_, kPushStages, offsetof(Push3D, alphaRef), sizeof(Push3D) - offsetof(Push3D, alphaRef), &pc.alphaRef);
        vkCmdDrawIndexed(cb, m.indexCount, 1, m.firstIndex, m.baseVertex, 0);
        ++drawCalls_;
    };
    // Same passes as the GL renderer: opaque, alpha-tested (depth-writing, unsorted), then translucent
    // back-to-front by mesh centre without depth writes.
    for (Blend pass : {Blend::Opaque, Blend::AlphaTest})
        for (const GpuMesh& m : meshes_)
            if (m.blend == pass && shown(m)) draw(m);
    std::vector<const GpuMesh*> translucent;
    for (const GpuMesh& m : meshes_)
        if (m.blend != Blend::Opaque && m.blend != Blend::AlphaTest && shown(m)) translucent.push_back(&m);
    std::ranges::sort(translucent, std::greater{}, [&](const GpuMesh* m) {
        const glm::vec3 d = m->center - cam.pos;
        return glm::dot(d, d);
    });
    for (const GpuMesh* m : translucent) draw(*m);
    if (fsr) fsrResolve(s);
}

// Copies into this frame's host-visible ring; returns the buffer and offset to bind.
std::pair<VkBuffer, VkDeviceSize> Renderer::streamWrite(const void* data, VkDeviceSize size) {
    Frame& f = frames_[frame_];
    if (f.used + size > f.stream.size) { // grow; the old buffer may still be referenced by this frame's commands
        f.garbage.push_back(f.stream);
        f.stream = makeBuffer(std::max(f.stream.size * 2, size), kStreamUsage | (rt_ ? kRtInput : 0), true);
        f.used = 0;
    }
    const VkDeviceSize offset = f.used;
    std::memcpy(static_cast<uint8_t*>(f.stream.map) + offset, data, size);
    f.used = (f.used + size + 15) & ~VkDeviceSize(15);
    return {f.stream.buf, offset};
}

void Renderer::quad2D(uint32_t tex, uint32_t sampler, float x, float y, float w, float h,
                      float u0, float v0, float u1, float v1, const uint8_t rgba[4]) {
    static constexpr uint8_t kWhite[4] = {255, 255, 255, 255};
    if (!rgba) rgba = kWhite;
    const uint32_t ts = tex << 2 | sampler;
    auto v = [&](float px, float py, float u, float vv) { return V2{px, py, u, vv, {rgba[0], rgba[1], rgba[2], rgba[3]}, ts}; };
    batch_.insert(batch_.end(), {v(x, y, u0, v0), v(x + w, y, u1, v0), v(x + w, y + h, u1, v1),
                                 v(x, y, u0, v0), v(x + w, y + h, u1, v1), v(x, y + h, u0, v1)});
}

void Renderer::drawImage(uint32_t image, float x, float y, float w, float h, bool smooth, const uint8_t rgba[4]) {
    if (image) quad2D(image, smooth ? kSampUiLinear : kSampUiNearest, x, y, w, h, 0, 0, 1, 1, rgba);
}

void Renderer::drawRect(float x, float y, float w, float h, const uint8_t rgba[4]) {
    quad2D(kSlotWhite, kSampUiNearest, x, y, w, h, 0, 0, 1, 1, rgba);
}

void Renderer::drawText(float x, float y, int scale, std::string_view text, const uint8_t rgba[4]) {
    const float g = float(kGlyph * scale);
    float cx = x;
    for (char ch : text) {
        if (ch == '\n') {
            cx = x;
            y += g + float(scale) * 2.0f;
            continue;
        }
        int c = static_cast<unsigned char>(ch);
        if (c < 32 || c > 127) c = '?';
        if (c != ' ') {
            const float u = float((c - 32) % 16) / 16.0f, v = float((c - 32) / 16) / 6.0f;
            quad2D(kSlotFont, kSampUiNearest, cx, y, g, g, u, v, u + 1.0f / 16.0f, v + 1.0f / 6.0f, rgba);
        }
        cx += g;
    }
}

// One draw for the whole 2D batch: the texture/sampler travel per vertex (bindless), so draw order is kept.
void Renderer::flush2D() {
    if (batch_.empty() || !recording_) {
        batch_.clear();
        return;
    }
    VkCommandBuffer cb = frames_[frame_].cmd;
    const auto [buf, off] = streamWrite(batch_.data(), batch_.size() * sizeof(V2));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe2D_[msaaOn_]);
    vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    const VkViewport viewport{0.0f, 0.0f, float(w_), float(h_), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    const float screen[2] = {float(std::max(w_, 1)), float(std::max(h_, 1))};
    vkCmdPushConstants(cb, layout_, kPushStages, 0, sizeof screen, screen);
    vkCmdBindVertexBuffers(cb, 0, 1, &buf, &off);
    vkCmdDraw(cb, uint32_t(batch_.size()), 1, 0, 0);
    ++drawCalls_;
    if (genPending_) uiDraws_.push_back({buf, off, uint32_t(batch_.size())});
    batch_.clear();
}

void Renderer::submitFrame(Buffer* readback, Buffer* genReadback) {
    Frame& f = frames_[frame_];
    flush2D();
    if (passOpen_) vkCmdEndRendering(f.cmd);
    passOpen_ = false;
    if (!fsrScene_) fsrReset_ = true; // FSR history and frame interpolation restart after any frame without them
    VkImage img = target_;
    // The swapchain image is presented from here; frame generation's real-frame image is copied by the present thread.
    const VkImageLayout last = fgMode_ ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    constexpr auto C = VK_IMAGE_ASPECT_COLOR_BIT;
    constexpr auto COLOR_OUT = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    auto copyOut = [&](VkImage from, Buffer* to) {
        VkBufferImageCopy copy{};
        copy.imageSubresource = {C, 0, 0, 1};
        copy.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(f.cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->buf, 1, &copy);
    };
    if (genPending_) { // this frame's 2D layer onto the generated frame too (the scene is interpolated, the HUD never)
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = f.genUi.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthAtt.imageView = depth_.view;
        depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAtt.clearValue.depthStencil = {1.0f, 0};
        memoryBarrier(f.cmd, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, extent_};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        ri.pDepthAttachment = &depthAtt;
        vkCmdBeginRendering(f.cmd, &ri);
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
        vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe2D_[0]);
        vkCmdSetCullMode(f.cmd, VK_CULL_MODE_NONE);
        const VkViewport viewport{0.0f, 0.0f, float(w_), float(h_), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, extent_};
        vkCmdSetViewport(f.cmd, 0, 1, &viewport);
        vkCmdSetScissor(f.cmd, 0, 1, &scissor);
        const float screen[2] = {float(std::max(w_, 1)), float(std::max(h_, 1))};
        vkCmdPushConstants(f.cmd, layout_, kPushStages, 0, sizeof screen, screen);
        for (const UiDraw& d : uiDraws_) {
            vkCmdBindVertexBuffers(f.cmd, 0, 1, &d.buf, &d.off);
            vkCmdDraw(f.cmd, d.count, 1, 0, 0);
        }
        vkCmdEndRendering(f.cmd);
        imageBarrier(f.cmd, f.genUi.img, C, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, COLOR_OUT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        if (genReadback) copyOut(f.genUi.img, genReadback);
    }
    if (readback) {
        imageBarrier(f.cmd, img, C, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, COLOR_OUT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        copyOut(img, readback);
        if (!fgMode_)
            imageBarrier(f.cmd, img, C, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_NONE, 0);
    } else {
        imageBarrier(f.cmd, img, C, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, last, COLOR_OUT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     fgMode_ ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_NONE, fgMode_ ? VK_ACCESS_2_TRANSFER_READ_BIT : 0);
    }
    if (readback || genReadback)
        memoryBarrier(f.cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                      VK_ACCESS_2_HOST_READ_BIT);
    check(vkEndCommandBuffer(f.cmd), "vkEndCommandBuffer");
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.acquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = fgMode_ ? VK_NULL_HANDLE : presentReady_[imageIndex_];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = f.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = fgMode_ ? 0 : 1; // frame generation: no swapchain image in this submission
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = fgMode_ ? 0 : 1;
    si.pSignalSemaphoreInfos = &signal;
    {
        std::lock_guard lock(queueMutex_);
        check(vkQueueSubmit2(queue_, 1, &si, f.fence), "vkQueueSubmit2");
    }
    recording_ = false;
    submitted_ = true;
}

namespace {
bool writeShot(const std::filesystem::path& path, const uint8_t* src, uint32_t w, uint32_t h, bool bgra) {
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        rgb[i * 3 + 0] = src[i * 4 + (bgra ? 2 : 0)];
        rgb[i * 3 + 1] = src[i * 4 + 1];
        rgb[i * 3 + 2] = src[i * 4 + (bgra ? 0 : 2)];
    }
    if (path.extension() == ".png") {
        SDL_Surface* s = SDL_CreateSurfaceFrom(int(w), int(h), SDL_PIXELFORMAT_RGB24, rgb.data(), int(w * 3));
        const bool ok = s && SDL_SavePNG(s, path.string().c_str());
        if (!ok) std::println(stderr, "screenshot: {}", SDL_GetError());
        SDL_DestroySurface(s);
        return ok;
    }
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << w << ' ' << h << "\n255\n";
    out.write(reinterpret_cast<const char*>(rgb.data()), std::streamsize(rgb.size()));
    return bool(out);
}
} // namespace

bool Renderer::screenshot(const std::filesystem::path& path) {
    if (!recording_ || !swapReadable_) return false;
    const uint32_t w = extent_.width, h = extent_.height;
    Buffer rb = makeBuffer(VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true), gb;
    const char* env = std::getenv("BBTIME_VK_SHOT_GENERATED");
    const bool gen = genPending_ && env && *env && *env != '0';
    if (gen) gb = makeBuffer(VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    submitFrame(&rb, gen ? &gb : nullptr);
    check(vkWaitForFences(device_, 1, &frames_[frame_].fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    const bool bgra = swapFormat_ == VK_FORMAT_B8G8R8A8_UNORM;
    bool ok = writeShot(path, static_cast<const uint8_t*>(rb.map), w, h, bgra);
    if (gen) {
        fs::path g = path;
        g.replace_filename(path.stem().string() + "-generated" + path.extension().string());
        ok = writeShot(g, static_cast<const uint8_t*>(gb.map), w, h, bgra) && ok;
        destroy(gb);
    }
    destroy(rb);
    return ok;
}

void Renderer::present() {
    if (recording_) submitFrame(nullptr);
    if (!submitted_) return;
    submitted_ = false;
    genShown_ = false;
    if (fgMode_) { // frame generation: the present thread shows this slot's generated (if any) and real images
        genShown_ = genPending_;
        {
            std::lock_guard lock(jobMutex_);
            jobs_.push_back({frame_, genPending_, SDL_GetTicksNS()});
            slotJob_[frame_] = ++jobsQueued_;
        }
        jobCv_.notify_all();
        frame_ = (frame_ + 1) % kFrames;
        return;
    }
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &presentReady_[imageIndex_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex_;
    VkResult r;
    {
        std::lock_guard lock(queueMutex_);
        r = vkQueuePresentKHR(queue_, &pi);
    }
    if (sceneDrawn_) presentTimes_.push_back(SDL_GetTicksNS());
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapDirty_ = true;
    else check(r, "vkQueuePresentKHR");
    frame_ = (frame_ + 1) % kFrames;
}

// ---------------------------------------------------------------- frame-generation present thread

void Renderer::startPresenter() {
    if (presenter_.joinable()) return;
    presenterStop_ = false;
    frameIntervalMs_ = 0.0;
    lastJobNs_ = 0;
    presenter_ = std::thread([this] {
        for (;;) {
            PresentJob j;
            {
                std::unique_lock lock(jobMutex_);
                jobCv_.wait(lock, [&] { return presenterStop_ || !jobs_.empty(); });
                if (jobs_.empty()) return; // stop requested and nothing left
                j = jobs_.front();
            }
            std::exception_ptr err;
            try {
                presentJob(j);
            } catch (...) {
                err = std::current_exception();
            }
            {
                std::lock_guard lock(jobMutex_);
                jobs_.pop_front();
                ++jobsDone_;
                if (err && !presenterError_) presenterError_ = err;
            }
            jobCv_.notify_all();
        }
    });
}

void Renderer::drainPresents() {
    std::unique_lock lock(jobMutex_);
    jobCv_.wait(lock, [&] { return jobsDone_ == jobsQueued_; });
    if (presenterError_) std::rethrow_exception(std::exchange(presenterError_, nullptr));
}

void Renderer::stopPresenter() {
    if (!presenter_.joinable()) return;
    {
        std::lock_guard lock(jobMutex_);
        presenterStop_ = true;
    }
    jobCv_.notify_all();
    presenter_.join();
    if (presenterError_) std::rethrow_exception(std::exchange(presenterError_, nullptr));
}

void Renderer::waitIdle() {
    drainPresents();
    std::lock_guard lock(queueMutex_);
    check(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
}

// Present thread: copies src (k 0 = generated, 1 = real frame) into a freshly acquired swapchain image and presents
// it. The frame's rendering was submitted to the same queue before the job was queued, so a barrier orders it.
bool Renderer::showImage(Frame& f, int k, const Image& src) {
    uint32_t img = 0;
    const VkResult a = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.copyAcquired[k], VK_NULL_HANDLE, &img);
    if (a == VK_ERROR_OUT_OF_DATE_KHR) {
        presentStale_ = true;
        return false;
    }
    if (a == VK_SUBOPTIMAL_KHR) presentStale_ = true;
    else check(a, "vkAcquireNextImageKHR(present thread)");
    VkCommandBuffer cb = f.copyCmd[k];
    check(vkResetFences(device_, 1, &f.copyFence[k]), "vkResetFences(present thread)");
    check(vkResetCommandBuffer(cb, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");
    constexpr auto C = VK_IMAGE_ASPECT_COLOR_BIT;
    memoryBarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT);
    imageBarrier(cb, swapImages_[img], C, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = {C, 0, 0, 1};
    copy.extent = {extent_.width, extent_.height, 1};
    vkCmdCopyImage(cb, src.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImages_[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    imageBarrier(cb, swapImages_[img], C, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    check(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.copyAcquired[k];
    wait.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = presentReady_[img];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cb;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &presentReady_[img];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &img;
    VkResult r;
    {
        std::lock_guard lock(queueMutex_);
        check(vkQueueSubmit2(queue_, 1, &si, f.copyFence[k]), "vkQueueSubmit2(present thread)");
        r = vkQueuePresentKHR(queue_, &pi);
    }
    presentTimes_.push_back(SDL_GetTicksNS());
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) presentStale_ = true;
    else check(r, "vkQueuePresentKHR(present thread)");
    return true;
}

// One rendered frame: its generated image now, its real image half a rendered-frame interval later (the interval
// smoothed over the job arrival times), so shown frames are evenly spaced with or without vsync. A backlog (the next
// frame already waiting) skips the wait.
void Renderer::presentJob(const PresentJob& j) {
    using Clock = std::chrono::steady_clock;
    if (lastJobNs_) {
        const double ms = double(j.ns - lastJobNs_) * 1e-6;
        frameIntervalMs_ = frameIntervalMs_ > 0.0 && ms < 4.0 * frameIntervalMs_ ? frameIntervalMs_ + (ms - frameIntervalMs_) * 0.1 : ms;
    }
    lastJobNs_ = j.ns;
    Frame& f = frames_[j.slot];
    bool used[2] = {false, false};
    if (j.gen) {
        used[0] = showImage(f, 0, f.genUi);
        bool backlog;
        {
            std::lock_guard lock(jobMutex_);
            backlog = jobs_.size() > 1;
        }
        if (used[0] && !backlog && frameIntervalMs_ > 0.0)
            std::this_thread::sleep_until(Clock::now() + std::chrono::duration<double, std::milli>(std::min(frameIntervalMs_, 100.0) * 0.5));
    }
    used[1] = showImage(f, 1, f.realUi);
    f.copyPending[0] = used[0]; // beginFrame waits these fences before the slot's images are drawn again
    f.copyPending[1] = used[1];
}

} // namespace bb::render_vk
