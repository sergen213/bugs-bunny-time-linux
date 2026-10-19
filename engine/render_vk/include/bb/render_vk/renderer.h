// SDL3 window + Vulkan 1.3 renderer for bb::Scene (dynamic rendering, synchronization2, bindless textures).
// Same interface shape as bb::render::Renderer (bb/render/renderer.h) so the app can swap backends with a
// template parameter; WindowConfig, Settings, Camera and Fidelity are shared with bb_render.
#pragma once
#include "bb/render/renderer.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bb::render_vk {

using render::Camera;
using render::Fidelity;
using render::Settings;
using render::WindowConfig;

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer();

    // Creates the window, instance, device and swapchain. validation = VK_LAYER_KHRONOS_validation (+ sync
    // validation); every message is printed to stderr as "VK-VALIDATION ...". Empty string on success.
    std::string init(const WindowConfig& cfg, bool validation = false);
    SDL_Window* window() const { return window_; }
    void setVsync(bool on); // on: FIFO_RELAXED (adaptive) else FIFO; off: IMMEDIATE else MAILBOX else FIFO
    void setFullscreen(bool on);

    // Named like the GL renderer's so app code stays backend-agnostic: vendor, device name, API/driver version.
    std::string_view glVendor() const { return vendor_; }
    std::string_view glRenderer() const { return rendererName_; }
    std::string_view glVersion() const { return version_; }
    std::string_view diagnostics() const { return diag_; } // extra overlay line: ray tracing capability probe
    float maxAnisotropy() const { return maxAniso_; }
    int msaaSamples() const { return msaaSamples_; }
    uint32_t validationMessages() const { return validationMessages_; }

    void uploadScene(const Scene& scene);
    // Dynamic actor meshes (engine space, Mesh::texture indexes the uploaded scene's textures). Kept until the next
    // call or uploadScene; drawScene streams them and draws them in the same passes as the scene meshes.
    void setActorMeshes(std::span<const Mesh> meshes);
    // 30 Hz simulation tick selecting the current frame of Scene::textureAnims (Mesh::textureAnim).
    void setTextureTick(uint32_t tick) { texTick_ = tick; }

    // 2D images: returns a bindless slot handle, 0 on invalid input.
    uint32_t createImage(const Texture& tex);
    void destroyImage(uint32_t image);

    void beginFrame(const Settings& s);
    void drawScene(const Camera& cam, const Settings& s);
    void drawImage(uint32_t image, float x, float y, float w, float h, bool smooth, const uint8_t rgba[4] = nullptr);
    void drawRect(float x, float y, float w, float h, const uint8_t rgba[4]);
    void drawText(float x, float y, int scale, std::string_view text, const uint8_t rgba[4]);
    static constexpr int kGlyph = 8;

    // Copies the swapchain image to the host (submits the frame). Call after drawing, before present().
    bool screenshot(const std::filesystem::path& path);
    void present();

    int width() const { return w_; }
    int height() const { return h_; }
    int drawCalls() const { return drawCalls_; }

private:
    struct Buffer { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void* map = nullptr; VkDeviceSize size = 0; };
    struct Image { VkImage img = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; };
    struct Frame { VkCommandBuffer cmd = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE; VkSemaphore acquired = VK_NULL_HANDLE;
                   Buffer stream; VkDeviceSize used = 0; std::vector<Buffer> garbage; };
    struct GpuMesh { uint32_t texture; Blend blend; bool twoSided, actor; int32_t baseVertex; uint32_t firstIndex, indexCount; glm::vec3 center; int32_t textureAnim; };
    struct TexAnim { std::vector<uint32_t> frames; uint32_t start, hold; }; // frames as bindless slots
    struct TexSrc { uint32_t w, h; const uint8_t* rgba; };
    struct V2 { float x, y, u, v; uint8_t rgba[4]; uint32_t texSamp; };

    Buffer makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host);
    void destroy(Buffer& b);
    VkImage newImage(uint32_t w, uint32_t h, VkFormat fmt, VkSampleCountFlagBits samples, VkImageUsageFlags usage, uint32_t mips);
    VkImageView newView(VkImage img, VkFormat fmt, VkImageAspectFlags aspect, uint32_t mips);
    Image makeTarget(VkFormat fmt, VkSampleCountFlagBits samples, VkImageUsageFlags usage);
    void destroy(Image& i);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want) const;
    VkCommandBuffer beginOneShot();
    void endOneShot(VkCommandBuffer cb);
    uint32_t allocSlot(VkImageView view);
    void freeSlot(uint32_t slot);
    VkDeviceMemory createTextures(VkCommandBuffer cb, const Buffer& staging, VkDeviceSize& off, std::span<const TexSrc> texs,
                                  bool mips, std::vector<uint32_t>& slots);
    void addMeshes(std::span<const Mesh> meshes, bool actor, std::vector<Vertex>& verts, std::vector<uint32_t>& idx);
    void createPipelines();
    void recreateSwapchain();
    void destroySwapchain();
    void submitFrame(Buffer* readback);
    std::pair<VkBuffer, VkDeviceSize> streamWrite(const void* data, VkDeviceSize size);
    void flush2D();
    void quad2D(uint32_t tex, uint32_t sampler, float x, float y, float w, float h,
                float u0, float v0, float u1, float v1, const uint8_t rgba[4]);
    void freeScene();

    SDL_Window* window_ = nullptr;
    std::string vendor_, rendererName_, version_, diag_;
    float maxAniso_ = 1.0f;
    int msaaSamples_ = 0;
    int w_ = 0, h_ = 0, drawCalls_ = 0;
    bool vsync_ = true;
    uint32_t validationMessages_ = 0;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps_{};
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    std::filesystem::path pipelineCachePath_;

    VkSampler samplers_[4]{};
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipe3D_[2][4]{}; // [msaa][Opaque+AlphaTest, Alpha, Additive, Subtract]
    VkPipeline pipe2D_[2]{};
    uint32_t maxSlots_ = 0;
    std::vector<Image> slots_;  // bindless binding 0; slot 0 = white, 1 = font
    std::vector<uint32_t> freeSlots_;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapFormat_ = VK_FORMAT_UNDEFINED;
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent_{};
    bool swapReadable_ = false, swapDirty_ = true, swapVsync_ = true;
    int swapW_ = 0, swapH_ = 0; // window size the swapchain was made for
    std::vector<VkImage> swapImages_;
    std::vector<VkImageView> swapViews_;
    std::vector<VkSemaphore> presentReady_; // per swapchain image
    Image depth_, msaaColor_, msaaDepth_;
    VkSampleCountFlagBits msaaMax_ = VK_SAMPLE_COUNT_1_BIT;

    Frame frames_[2];
    uint32_t frame_ = 0, imageIndex_ = 0;
    bool recording_ = false, submitted_ = false, msaaOn_ = false;

    VkDeviceMemory sceneMem_ = VK_NULL_HANDLE;
    Buffer sceneVbo_, sceneIbo_;
    std::vector<uint32_t> sceneSlots_;  // per created texture
    std::vector<uint32_t> texSlot_;     // per Scene::textures index (white when undecodable)
    std::vector<GpuMesh> meshes_;       // scene meshes, then actor meshes (actor = true)
    std::vector<TexAnim> texAnims_;
    uint32_t texTick_ = 0;
    size_t sceneMeshCount_ = 0;
    std::vector<Vertex> actorVerts_;
    std::vector<uint32_t> actorIdx_;
    float colorScale_ = 1.0f;

    std::vector<V2> batch_;
};

} // namespace bb::render_vk
