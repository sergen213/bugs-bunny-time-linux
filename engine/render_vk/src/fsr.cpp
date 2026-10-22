#include "fsr.h"

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <yami_ffx_vulkan.h>

#include <array>
#include <cstdlib>
#include <cstddef>
#include <format>
#include <print>
#include <stdexcept>
#include <vector>

namespace bb::render_vk {
namespace {

void ffx(FfxErrorCode r, const char* what) {
    if (r != FFX_OK) throw std::runtime_error(std::format("{}: FidelityFX error {}", what, int(r)));
}

FfxResource resource(const Fsr::Image& i, FfxResourceStates state) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = i.format;
    ci.extent = {i.extent.width, i.extent.height, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = i.usage;
    return ffxGetResourceVK(i.img, ffxGetImageResourceDescriptionVK(i.img, ci), nullptr, state);
}

// BBTIME_VK_NO_REACTIVE=1: no reactive / transparency / shading-change mask (before/after comparisons).
bool reactiveOff() {
    static const bool off = [] { const char* e = std::getenv("BBTIME_VK_NO_REACTIVE"); return e && *e && *e != '0'; }();
    return off;
}

// Bugs is ~300 units tall (MODELS.md §3), a ~1.2 m cartoon rabbit: 0.004 m per unit.
constexpr float kMetresPerUnit = 0.004f;

} // namespace

struct Fsr::Impl {
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    VkDeviceContext vkDevice{};
    std::vector<std::byte> scratch, genScratch;
    FfxInterface generationBackend{};
    std::unique_ptr<FfxFsr3UpscalerContext> upscaler;
    std::unique_ptr<FfxOpticalflowContext> flow;
    std::unique_ptr<FfxFrameInterpolationContext> interpolation;
    FfxUInt32 flowEffect = 0;
    bool flowEffectCreated = false;
    std::array<FfxResourceInternal, 2> flowOutput{{{-1}, {-1}}};
    uint64_t frameId = 0;
    bool chain = false;  // the previous frame was interpolated too (else the next interpolation resets)

    FfxInterface backend(std::vector<std::byte>& mem, size_t contexts) {
        mem.assign(ffxGetScratchMemorySizeVK(gpu, contexts), std::byte{0}); // the backend refuses a stale reference count
        FfxInterface r{};
        ffx(ffxGetInterfaceVK(&r, ffxGetDeviceVK(&vkDevice), mem.data(), mem.size(), contexts), "FSR Vulkan backend");
        yami_ffx_clamp_capabilities(r);
        return r;
    }
};

bool Fsr::load(VkInstance instance) { return yami_ffx_load_vulkan(vkGetInstanceProcAddr, instance); }

Fsr::Fsr(VkInstance instance, VkPhysicalDevice gpu, VkDevice device) : impl_(std::make_unique<Impl>()) {
    impl_->instance = instance;
    impl_->gpu = gpu;
    impl_->device = device;
    impl_->vkDevice = {device, gpu, yami_ffx_device_proc};
}

Fsr::~Fsr() { destroy(); }

bool Fsr::created() const { return impl_->upscaler != nullptr; }
bool Fsr::generation() const { return impl_->interpolation != nullptr; }

void Fsr::destroy() noexcept {
    Impl& m = *impl_;
    if (m.flowEffectCreated) {
        for (FfxResourceInternal& r : m.flowOutput) m.generationBackend.fpDestroyResource(&m.generationBackend, r, m.flowEffect);
        m.generationBackend.fpDestroyBackendContext(&m.generationBackend, m.flowEffect);
        m.flowEffectCreated = false;
        m.flowOutput = {{{-1}, {-1}}};
    }
    if (m.interpolation && ffxFrameInterpolationContextDestroy(m.interpolation.get()) != FFX_OK)
        std::println(stderr, "vk: frame interpolation destroy failed");
    if (m.flow && ffxOpticalflowContextDestroy(m.flow.get()) != FFX_OK) std::println(stderr, "vk: optical flow destroy failed");
    if (m.upscaler && ffxFsr3UpscalerContextDestroy(m.upscaler.get()) != FFX_OK) std::println(stderr, "vk: FSR destroy failed");
    m.interpolation.reset();
    m.flow.reset();
    m.upscaler.reset();
    m.chain = false;
}

void Fsr::create(VkExtent2D render, VkExtent2D display, bool generation, VkFormat generationFormat) {
    destroy();
    Impl& m = *impl_;
    FfxFsr3UpscalerContextDescription d{};
    d.backendInterface = m.backend(m.scratch, FFX_FSR3UPSCALER_CONTEXT_COUNT);
    d.maxRenderSize = {render.width, render.height};
    d.maxUpscaleSize = {display.width, display.height};
    d.flags = 0; // LDR UNORM colour, standard depth, render-resolution unjittered motion vectors
    auto up = std::make_unique<FfxFsr3UpscalerContext>();
    ffx(ffxFsr3UpscalerContextCreate(up.get(), &d), "FSR context");
    m.upscaler = std::move(up);
    render_ = render;
    display_ = display;
    if (!generation) return;
    m.generationBackend = m.backend(m.genScratch, 3);
    FfxOpticalflowContextDescription fd{};
    fd.backendInterface = m.generationBackend;
    fd.resolution = {display.width, display.height};
    auto flow = std::make_unique<FfxOpticalflowContext>();
    ffx(ffxOpticalflowContextCreate(flow.get(), &fd), "optical flow context");
    m.flow = std::move(flow);
    FfxFrameInterpolationContextDescription id{};
    id.backendInterface = m.generationBackend;
    id.maxRenderSize = {render.width, render.height};
    id.displaySize = {display.width, display.height};
    id.backBufferFormat = id.previousInterpolationSourceFormat = ffxGetSurfaceFormatVK(generationFormat);
    auto fi = std::make_unique<FfxFrameInterpolationContext>();
    ffx(ffxFrameInterpolationContextCreate(fi.get(), &id), "frame interpolation context");
    m.interpolation = std::move(fi);
    ffx(m.generationBackend.fpCreateBackendContext(&m.generationBackend, FFX_EFFECT_SHAREDRESOURCES, nullptr, &m.flowEffect),
        "optical flow shared resources");
    m.flowEffectCreated = true;
    FfxOpticalflowSharedResourceDescriptions out{};
    ffx(ffxOpticalflowGetSharedResourceDescriptions(m.flow.get(), &out), "optical flow resource descriptions");
    ffx(m.generationBackend.fpCreateResource(&m.generationBackend, &out.opticalFlowVector, m.flowEffect, &m.flowOutput[0]), "optical flow vector");
    ffx(m.generationBackend.fpCreateResource(&m.generationBackend, &out.opticalFlowSCD, m.flowEffect, &m.flowOutput[1]), "optical flow SCD");
}

void Fsr::jitter(uint32_t frame, VkExtent2D render, VkExtent2D display, float& x, float& y) {
    const int32_t phases = ffxFsr3UpscalerGetJitterPhaseCount(int32_t(render.width), int32_t(display.width));
    ffxFsr3UpscalerGetJitterOffset(&x, &y, int32_t(frame % uint32_t(phases)), phases);
}

void Fsr::upscale(VkCommandBuffer cb, const Upscale& u) {
    Impl& m = *impl_;
    FfxFsr3UpscalerDispatchDescription d{};
    d.commandList = ffxGetCommandListVK(cb);
    d.color = resource(u.color, FFX_RESOURCE_STATE_COMPUTE_READ);
    d.depth = resource(u.depth, FFX_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = resource(u.motion, FFX_RESOURCE_STATE_COMPUTE_READ);
    // Glows, translucency, sprites and reflections: cut history weight (reactive) and drop FSR's locks (transparency &
    // composition), the same mask as etiyami uses for both.
    if (!reactiveOff()) {
        d.reactive = resource(u.reactive, FFX_RESOURCE_STATE_COMPUTE_READ);
        d.transparencyAndComposition = d.reactive;
    }
    d.dilatedDepth = resource(u.dilatedDepth, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.dilatedMotionVectors = resource(u.dilatedMotion, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.reconstructedPrevNearestDepth = resource(u.prevDepth, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.output = resource(u.output, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {u.jitterX, u.jitterY};
    d.motionVectorScale = {float(u.color.extent.width), float(u.color.extent.height)}; // motion in UV units
    d.renderSize = {u.color.extent.width, u.color.extent.height};
    d.upscaleSize = {u.output.extent.width, u.output.extent.height};
    d.enableSharpening = u.sharpness > 0.0f;
    d.sharpness = u.sharpness;
    d.frameTimeDelta = u.frameMs;
    d.preExposure = 1.0f;
    d.reset = u.reset;
    d.cameraNear = u.zNear;
    d.cameraFar = u.zFar;
    d.cameraFovAngleVertical = u.vfovRad;
    d.viewSpaceToMetersFactor = kMetresPerUnit;
    ffx(ffxFsr3UpscalerContextDispatch(m.upscaler.get(), &d), "FSR dispatch");
}

bool Fsr::interpolate(VkCommandBuffer cb, const Image& source, const Image& target, const Upscale& u) {
    Impl& m = *impl_;
    const bool reset = u.reset || !m.chain;
    const FfxResource vector = m.generationBackend.fpGetResource(&m.generationBackend, m.flowOutput[0]);
    const FfxResource scd = m.generationBackend.fpGetResource(&m.generationBackend, m.flowOutput[1]);
    const FfxResource color = resource(source, FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxOpticalflowDispatchDescription fd{};
    fd.commandList = ffxGetCommandListVK(cb);
    fd.color = color;
    fd.opticalFlowVector = vector;
    fd.opticalFlowSCD = scd;
    fd.reset = reset;
    fd.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    ffx(ffxOpticalflowContextDispatch(m.flow.get(), &fd), "optical flow dispatch");
    FfxFrameInterpolationDispatchDescription d{};
    d.commandList = fd.commandList;
    d.displaySize = {display_.width, display_.height};
    d.renderSize = {render_.width, render_.height};
    d.currentBackBuffer = color;
    d.output = resource(target, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.interpolationRect = {0, 0, int(display_.width), int(display_.height)};
    d.opticalFlowVector = vector;
    d.opticalFlowSceneChangeDetection = scd;
    d.opticalFlowBlockSize = 8;
    d.opticalFlowScale = {1.0f / float(display_.width), 1.0f / float(display_.height)};
    d.cameraNear = u.zNear;
    d.cameraFar = u.zFar;
    d.cameraFovAngleVertical = u.vfovRad;
    d.viewSpaceToMetersFactor = kMetresPerUnit;
    d.frameTimeDelta = u.frameMs;
    d.reset = reset;
    d.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    d.frameID = ++m.frameId;
    d.dilatedDepth = resource(u.dilatedDepth, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.dilatedMotionVectors = resource(u.dilatedMotion, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.reconstructedPrevDepth = resource(u.prevDepth, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    // Local SDK patch 7 (third_party/fidelityfx-sdk README): where the mask is set the in-between frame takes this frame's
    // colour instead of cross-fading two states of a glow / sprite / reflection.
    if (!reactiveOff()) d.shadingChange = resource(u.reactive, FFX_RESOURCE_STATE_COMPUTE_READ);
    ffx(ffxFrameInterpolationDispatch(m.interpolation.get(), &d), "frame interpolation dispatch");
    m.chain = true;
    return !reset;
}

} // namespace bb::render_vk
