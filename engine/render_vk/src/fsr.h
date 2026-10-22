// AMD FSR 3.1.4 upscaler + frame interpolation for the Vulkan renderer (third_party/fidelityfx-sdk, integrated as in
// /mnt/1st4TB/etiyami native/vulkan_effects.cpp). Only the FidelityFX contexts live here; the renderer owns and
// transitions every image and passes them per dispatch.
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

namespace bb::render_vk {

class Fsr {
public:
    struct Image {
        VkImage img = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        VkImageUsageFlags usage = 0;
    };
    // Per-frame upscale inputs (render resolution) and output (display resolution). Layouts on entry and exit:
    // color / motion / reactive / depth SHADER_READ_ONLY_OPTIMAL, dilated* / prevDepth / output GENERAL.
    struct Upscale {
        Image color, depth, motion, reactive, dilatedDepth, dilatedMotion, prevDepth, output; // reactive: R8 mask, also T&C
        float jitterX = 0, jitterY = 0;  // pixels, as applied to the projection
        float frameMs = 16.7f, zNear = 0.1f, zFar = 1000.0f, vfovRad = 1.0f;
        bool reset = false;
        float sharpness = 0.0f;          // 0 = RCAS off
    };

    // Resolves the entry points the FidelityFX backend calls (once per instance). False: FSR unavailable.
    static bool load(VkInstance instance);
    Fsr(VkInstance instance, VkPhysicalDevice gpu, VkDevice device);
    ~Fsr();
    Fsr(const Fsr&) = delete;
    Fsr& operator=(const Fsr&) = delete;

    // (Re)creates the contexts. generation: also optical flow + frame interpolation at display size, with
    // `generationFormat` as the interpolation source / target format. Call with no FSR work pending on the GPU.
    void create(VkExtent2D render, VkExtent2D display, bool generation, VkFormat generationFormat);
    void destroy() noexcept;
    bool created() const;
    bool generation() const;
    VkExtent2D render() const { return render_; }
    VkExtent2D display() const { return display_; }

    void upscale(VkCommandBuffer cb, const Upscale& u);
    // Interpolates between the previous and this frame's `source` (display size, SHADER_READ_ONLY) into `target`
    // (GENERAL), reusing this frame's upscale shared resources. Call right after upscale(). Returns false when the
    // interpolation history was reset (target holds no valid in-between frame).
    bool interpolate(VkCommandBuffer cb, const Image& source, const Image& target, const Upscale& u);

    static void jitter(uint32_t frame, VkExtent2D render, VkExtent2D display, float& x, float& y);

private:
    struct Impl;
    VkExtent2D render_{}, display_{};
    std::unique_ptr<Impl> impl_;
};

} // namespace bb::render_vk
