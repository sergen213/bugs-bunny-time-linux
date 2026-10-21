#include "bb/render_vk/renderer.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <print>
#include <ranges>
#include <tuple>
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
    float texGamma;  // Settings::texGamma (1 = off)
};
static_assert(sizeof(Push3D) == 124 && offsetof(Push3D, alphaRef) == 104);

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::format("{} failed (VkResult {})", what, int(r)));
}

void imageBarrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, uint32_t mip, uint32_t mips,
                  VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, mip, mips, 0, 1};
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
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
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

Renderer::~Renderer() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        freeScene();
        for (uint32_t s = 0; s < slots_.size(); ++s) destroy(slots_[s]);
        if (pipelineCache_) {
            size_t n = 0;
            vkGetPipelineCacheData(device_, pipelineCache_, &n, nullptr);
            std::vector<char> data(n);
            if (n && vkGetPipelineCacheData(device_, pipelineCache_, &n, data.data()) == VK_SUCCESS) {
                std::error_code ec;
                fs::create_directories(pipelineCachePath_.parent_path(), ec);
                std::ofstream(pipelineCachePath_, std::ios::binary).write(data.data(), std::streamsize(n));
            }
            vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
        }
        for (Frame& f : frames_) {
            destroy(f.stream);
            for (Buffer& b : f.garbage) destroy(b);
            vkDestroyFence(device_, f.fence, nullptr);
            vkDestroySemaphore(device_, f.acquired, nullptr);
        }
        destroySwapchain();
        for (auto& row : pipe3D_)
            for (VkPipeline p : row) vkDestroyPipeline(device_, p, nullptr);
        for (VkPipeline p : pipe2D_) vkDestroyPipeline(device_, p, nullptr);
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        vkDestroyDescriptorPool(device_, descPool_, nullptr);
        vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
        for (VkSampler s : samplers_) vkDestroySampler(device_, s, nullptr);
        vkDestroyCommandPool(device_, pool_, nullptr);
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

    // Ray tracing: probe only (diagnostics overlay); implementation is a later wave.
    check(vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &n, nullptr), "vkEnumerateDeviceExtensionProperties");
    std::vector<VkExtensionProperties> dexts(n);
    check(vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &n, dexts.data()), "vkEnumerateDeviceExtensionProperties");
    auto has = [&](std::string_view e) { return std::ranges::any_of(dexts, [&](const auto& x) { return e == x.extensionName; }); };
    diag_ = std::format("RT probe (unused): accel_struct {} ray_query {} rt_pipeline {}", has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) ? "yes" : "no",
                        has(VK_KHR_RAY_QUERY_EXTENSION_NAME) ? "yes" : "no", has(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) ? "yes" : "no");

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e13.dynamicRendering = e13.synchronization2 = VK_TRUE;
    e13.shaderDemoteToHelperInvocation = VK_TRUE; // glslc lowers `discard` to demote for Vulkan 1.3 (required feature)
    VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &e13};
    e12.runtimeDescriptorArray = e12.descriptorBindingPartiallyBound = e12.descriptorBindingSampledImageUpdateAfterBind =
        e12.descriptorBindingUpdateUnusedWhilePending = e12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e12};
    e2.features.samplerAnisotropy = base.samplerAnisotropy;
    const char* devExt = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &e2};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = &devExt;
    check(vkCreateDevice(gpu_, &dci, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily_;
    check(vkCreateCommandPool(device_, &pci, nullptr, &pool_), "vkCreateCommandPool");

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

    VkPushConstantRange pcr{kPushStages, 0, sizeof(Push3D)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout_;
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
        f.stream = makeBuffer(1 << 20, kStreamUsage, true);
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
    VkShaderModule sv = module(kSceneVert), sf = module(kSceneFrag), uv = module(kUiVert), uf = module(kUiFrag);

    const VkVertexInputBindingDescription vb3{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription va3[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)},
                                                      {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
                                                      {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, rgba)}};
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

    auto make = [&](bool ui, int blendIdx, VkSampleCountFlagBits samples) {
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, ui ? uv : sv, "main", nullptr};
        st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ui ? uf : sf, "main", nullptr};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = ui ? &vb2 : &vb3;
        vi.vertexAttributeDescriptionCount = ui ? 4 : 3;
        vi.pVertexAttributeDescriptions = ui ? va2 : va3;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // engine space front faces; y flipped by the negative viewport
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = samples;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = !ui;
        ds.depthWriteEnable = !ui && blendIdx == 0;
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &blends[blendIdx];
        const VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 3;
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &swapFormat_;
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
        for (int b = 0; b < 4; ++b) pipe3D_[m][b] = make(false, b, samples);
        pipe2D_[m] = make(true, 1, samples);
    }
    for (VkShaderModule s : {sv, sf, uv, uf}) vkDestroyShaderModule(device_, s, nullptr);
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
    check(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
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
    ci.minImageCount = caps.maxImageCount ? std::min(caps.minImageCount + 1, caps.maxImageCount) : caps.minImageCount + 1;
    ci.imageFormat = swapFormat_;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    swapReadable_ = caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (swapReadable_ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
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
    destroy(sceneVbo_);
    destroy(sceneIbo_);
    if (sceneMem_) vkFreeMemory(device_, sceneMem_, nullptr);
    sceneMem_ = VK_NULL_HANDLE;
}

void Renderer::addMeshes(std::span<const Mesh> ms, bool actor, std::vector<Vertex>& verts, std::vector<uint32_t>& idx) {
    for (const Mesh& m : ms) {
        if (m.vertices.empty() || m.indices.empty()) continue;
        glm::vec3 lo(m.vertices[0].pos[0], m.vertices[0].pos[1], m.vertices[0].pos[2]), hi = lo;
        for (const Vertex& v : m.vertices) {
            lo = glm::min(lo, glm::vec3(v.pos[0], v.pos[1], v.pos[2]));
            hi = glm::max(hi, glm::vec3(v.pos[0], v.pos[1], v.pos[2]));
        }
        const uint32_t tex = m.texture >= 0 && size_t(m.texture) < texSlot_.size() ? texSlot_[size_t(m.texture)] : kSlotWhite;
        const bool animOk = m.textureAnim >= 0 && size_t(m.textureAnim) < texAnims_.size() && !texAnims_[size_t(m.textureAnim)].frames.empty();
        meshes_.push_back({tex, m.blend, m.twoSided, actor, int32_t(verts.size()), uint32_t(idx.size()), uint32_t(m.indices.size()),
                           (lo + hi) * 0.5f, animOk ? m.textureAnim : -1});
        verts.insert(verts.end(), m.vertices.begin(), m.vertices.end());
        idx.insert(idx.end(), m.indices.begin(), m.indices.end());
    }
}

void Renderer::setActorMeshes(std::span<const Mesh> ms) {
    meshes_.resize(sceneMeshCount_);
    actorVerts_.clear();
    actorIdx_.clear();
    addMeshes(ms, true, actorVerts_, actorIdx_);
}

void Renderer::uploadScene(const Scene& scene) {
    check(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
    freeScene();
    colorScale_ = scene.colorScale;
    texGamma_ = scene.pcTexGamma;
    linearOriginal_ = scene.game == GameId::LostInTime;

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
        if (!m.vertices.empty() && !m.indices.empty()) bytes += m.vertices.size() * sizeof(Vertex) + m.indices.size() * sizeof(uint32_t);
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
    addMeshes(scene.meshes, false, verts, idx);
    sceneMeshCount_ = meshes_.size();
    const VkDeviceSize vBytes = verts.size() * sizeof(Vertex), iBytes = idx.size() * sizeof(uint32_t);
    if (vBytes) {
        sceneVbo_ = makeBuffer(vBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        sceneIbo_ = makeBuffer(iBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        std::memcpy(static_cast<uint8_t*>(staging.map) + off, verts.data(), vBytes);
        std::memcpy(static_cast<uint8_t*>(staging.map) + off + vBytes, idx.data(), iBytes);
        const VkBufferCopy cv{off, 0, vBytes}, ci{off + vBytes, 0, iBytes};
        vkCmdCopyBuffer(cb, staging.buf, sceneVbo_.buf, 1, &cv);
        vkCmdCopyBuffer(cb, staging.buf, sceneIbo_.buf, 1, &ci);
        memoryBarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
                      VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT);
    }
    endOneShot(cb);
    destroy(staging);
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
    check(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
    freeSlot(image);
}

// ---------------------------------------------------------------- frame

void Renderer::beginFrame(const Settings& s) {
    SDL_GetWindowSizeInPixels(window_, &w_, &h_);
    drawCalls_ = 0;
    batch_.clear();
    recording_ = submitted_ = false;
    if (w_ <= 0 || h_ <= 0) return;
    if (swapDirty_ || w_ != swapW_ || h_ != swapH_ || vsync_ != swapVsync_) recreateSwapchain();
    if (swapDirty_) return;

    Frame& f = frames_[frame_];
    check(vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    for (Buffer& b : f.garbage) destroy(b);
    f.garbage.clear();
    f.used = 0;
    const VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &imageIndex_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        swapDirty_ = true; // skip this frame, the next beginFrame recreates
        return;
    }
    if (r == VK_SUBOPTIMAL_KHR) swapDirty_ = true;
    else check(r, "vkAcquireNextImageKHR");
    check(vkResetFences(device_, 1, &f.fence), "vkResetFences");
    check(vkResetCommandBuffer(f.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(f.cmd, &bi), "vkBeginCommandBuffer");
    w_ = int(extent_.width);
    h_ = int(extent_.height);

    msaaOn_ = s.fidelity == Fidelity::Enhanced && s.msaa && msaaMax_ > VK_SAMPLE_COUNT_1_BIT;
    const Image& depth = msaaOn_ ? msaaDepth_ : depth_;
    constexpr auto COLOR_OUT = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    constexpr auto COLOR_W = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    constexpr auto DEPTH_STAGES = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    constexpr auto DEPTH_RW = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    imageBarrier(f.cmd, swapImages_[imageIndex_], VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, COLOR_OUT, 0, COLOR_OUT, COLOR_W);
    imageBarrier(f.cmd, depth.img, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 DEPTH_STAGES, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, DEPTH_STAGES, DEPTH_RW);
    if (msaaOn_)
        imageBarrier(f.cmd, msaaColor_.img, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, COLOR_OUT, COLOR_W, COLOR_OUT, COLOR_W);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = msaaOn_ ? msaaColor_.view : swapViews_[imageIndex_];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = msaaOn_ ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    if (msaaOn_) {
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = swapViews_[imageIndex_];
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
    vkCmdBeginRendering(f.cmd, &ri);
    vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    recording_ = true;
}

void Renderer::drawScene(const Camera& cam, const Settings& s) {
    if (meshes_.empty() || !recording_) return;
    flush2D();
    VkCommandBuffer cb = frames_[frame_].cmd;
    const bool original = s.fidelity == Fidelity::Original;
    const render::Viewport vp = (original && s.pillarbox) || s.frame43 ? render::fitAspect(w_, h_, 4.0f / 3.0f) : render::Viewport{0, 0, w_, h_};
    const int top = h_ - vp.y - vp.h; // fitAspect is bottom-left based like GL; Vulkan is top-left
    const VkRect2D rect{{vp.x, top}, {uint32_t(vp.w), uint32_t(vp.h)}};
    VkClearAttachment clears[2]{};
    clears[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clears[0].clearValue.color = {{s.fogColor[0], s.fogColor[1], s.fogColor[2], 1.0f}};
    clears[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    clears[1].clearValue.depthStencil = {1.0f, 0};
    const VkClearRect clearRect{rect, 0, 1};
    vkCmdClearAttachments(cb, 2, clears, 1, &clearRect);
    // Negative height flips y so the GL-convention projection (camera.h) and CCW front faces carry over.
    const VkViewport viewport{float(vp.x), float(top + vp.h), float(vp.w), -float(vp.h), 0.0f, 1.0f};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &rect);

    glm::mat4 clipFix(1.0f); // GL clip z [-w, w] -> Vulkan [0, w]
    clipFix[2][2] = 0.5f;
    clipFix[3][2] = 0.5f;
    const glm::mat4 view = cam.view();
    Push3D pc{};
    pc.viewProj = clipFix * render::projection(s.vfovRad(), float(vp.w) / float(vp.h), s.zNear, s.zFar) * view;
    pc.viewZ = glm::vec4(view[0][2], view[1][2], view[2][2], view[3][2]);
    pc.fogColorScale = glm::vec4(s.fogColor[0], s.fogColor[1], s.fogColor[2], s.colorScale(colorScale_));
    pc.texGamma = s.texGamma(texGamma_);
    pc.fog = s.fog ? glm::vec2(s.fogStart, s.fogEnd) : glm::vec2(0.0f);
    pc.samp = original ? (linearOriginal_ ? kSampLinear : kSampNearest) : kSampTrilinear;
    vkCmdPushConstants(cb, layout_, kPushStages, 0, sizeof pc, &pc);
    // Scene geometry lives in device-local buffers; actor geometry is streamed into this frame's ring.
    VkBuffer geomV[2] = {sceneVbo_.buf, VK_NULL_HANDLE}, geomI[2] = {sceneIbo_.buf, VK_NULL_HANDLE};
    VkDeviceSize offV[2] = {0, 0}, offI[2] = {0, 0};
    if (!actorIdx_.empty()) {
        std::tie(geomV[1], offV[1]) = streamWrite(actorVerts_.data(), actorVerts_.size() * sizeof(Vertex));
        std::tie(geomI[1], offI[1]) = streamWrite(actorIdx_.data(), actorIdx_.size() * sizeof(uint32_t));
    }

    VkPipeline bound = VK_NULL_HANDLE;
    int boundGeom = -1;
    auto draw = [&](const GpuMesh& m) {
        // Original Fidelity draws ABR 2 the way the PC exe's GL path does (luma alpha, SRC_ALPHA / 1-SRC_ALPHA).
        const bool pcSub = original && m.blend == Blend::Subtract;
        const int b = m.blend == Blend::Alpha || pcSub ? 1 : m.blend == Blend::Additive ? 2 : m.blend == Blend::Subtract ? 3 : 0;
        if (VkPipeline p = pipe3D_[msaaOn_][b]; p != bound) vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = p);
        if (int g = m.actor; g != boundGeom) {
            vkCmdBindVertexBuffers(cb, 0, 1, &geomV[g], &offV[g]);
            vkCmdBindIndexBuffer(cb, geomI[g], offI[g], VK_INDEX_TYPE_UINT32);
            boundGeom = g;
        }
        vkCmdSetCullMode(cb, m.twoSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
        pc.alphaRef = m.blend == Blend::AlphaTest ? 0.5f : -1.0f;
        pc.flags = (m.blend == Blend::Additive || m.blend == Blend::Subtract) | (pcSub ? 2u : 0u);
        if (m.textureAnim >= 0) {
            const TexAnim& a = texAnims_[size_t(m.textureAnim)];
            pc.tex = a.frames[(a.start + texTick_ / a.hold) % a.frames.size()];
        } else {
            pc.tex = m.texture;
        }
        vkCmdPushConstants(cb, layout_, kPushStages, offsetof(Push3D, alphaRef), 16, &pc.alphaRef);
        vkCmdDrawIndexed(cb, m.indexCount, 1, m.firstIndex, m.baseVertex, 0);
        ++drawCalls_;
    };
    // Same passes as the GL renderer: opaque, alpha-tested (depth-writing, unsorted), then translucent
    // back-to-front by mesh centre without depth writes.
    for (Blend pass : {Blend::Opaque, Blend::AlphaTest})
        for (const GpuMesh& m : meshes_)
            if (m.blend == pass) draw(m);
    std::vector<const GpuMesh*> translucent;
    for (const GpuMesh& m : meshes_)
        if (m.blend != Blend::Opaque && m.blend != Blend::AlphaTest) translucent.push_back(&m);
    std::ranges::sort(translucent, std::greater{}, [&](const GpuMesh* m) {
        const glm::vec3 d = m->center - cam.pos;
        return glm::dot(d, d);
    });
    for (const GpuMesh* m : translucent) draw(*m);
}

// Copies into this frame's host-visible ring; returns the buffer and offset to bind.
std::pair<VkBuffer, VkDeviceSize> Renderer::streamWrite(const void* data, VkDeviceSize size) {
    Frame& f = frames_[frame_];
    if (f.used + size > f.stream.size) { // grow; the old buffer may still be referenced by this frame's commands
        f.garbage.push_back(f.stream);
        f.stream = makeBuffer(std::max(f.stream.size * 2, size), kStreamUsage, true);
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
    batch_.clear();
}

void Renderer::submitFrame(Buffer* readback) {
    Frame& f = frames_[frame_];
    flush2D();
    vkCmdEndRendering(f.cmd);
    VkImage img = swapImages_[imageIndex_];
    constexpr auto C = VK_IMAGE_ASPECT_COLOR_BIT;
    constexpr auto COLOR_OUT = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (readback) {
        imageBarrier(f.cmd, img, C, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, COLOR_OUT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {C, 0, 0, 1};
        copy.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(f.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->buf, 1, &copy);
        memoryBarrier(f.cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                      VK_ACCESS_2_HOST_READ_BIT);
        imageBarrier(f.cmd, img, C, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_NONE, 0);
    } else {
        imageBarrier(f.cmd, img, C, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, COLOR_OUT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    }
    check(vkEndCommandBuffer(f.cmd), "vkEndCommandBuffer");
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.acquired;
    wait.stageMask = COLOR_OUT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = presentReady_[imageIndex_];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = f.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    check(vkQueueSubmit2(queue_, 1, &si, f.fence), "vkQueueSubmit2");
    recording_ = false;
    submitted_ = true;
}

bool Renderer::screenshot(const std::filesystem::path& path) {
    if (!recording_ || !swapReadable_) return false;
    const uint32_t w = extent_.width, h = extent_.height;
    Buffer rb = makeBuffer(VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    submitFrame(&rb);
    check(vkWaitForFences(device_, 1, &frames_[frame_].fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    const bool bgra = swapFormat_ == VK_FORMAT_B8G8R8A8_UNORM;
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    const auto* src = static_cast<const uint8_t*>(rb.map);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        rgb[i * 3 + 0] = src[i * 4 + (bgra ? 2 : 0)];
        rgb[i * 3 + 1] = src[i * 4 + 1];
        rgb[i * 3 + 2] = src[i * 4 + (bgra ? 0 : 2)];
    }
    destroy(rb);
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

void Renderer::present() {
    if (recording_) submitFrame(nullptr);
    if (!submitted_) return;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &presentReady_[imageIndex_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex_;
    const VkResult r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapDirty_ = true;
    else check(r, "vkQueuePresentKHR");
    submitted_ = false;
    frame_ = (frame_ + 1) % kFrames;
}

} // namespace bb::render_vk
