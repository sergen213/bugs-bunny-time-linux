// SDL3 window + Vulkan 1.3 renderer for bb::Scene (dynamic rendering, synchronization2, bindless textures).
// Same interface shape as bb::render::Renderer (bb/render/renderer.h) so the app can swap backends with a
// template parameter; WindowConfig, Settings, Camera and Fidelity are shared with bb_render.
#pragma once
#include "bb/render/renderer.h"
#include "bb/render/water_waves.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace bb::render_vk {

using render::Camera;
using render::Fidelity;
using render::Shadows;
using render::Upscaling;
class Fsr;
using render::Settings;
using render::WindowConfig;

class Renderer {
public:
    Renderer();
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
    std::string_view diagnostics() const { return diag_; } // extra overlay line: ray tracing support + shadow mode
    bool rayTracing() const { return rt_; }                  // ray queries usable (else Shadows::Rt/RtAo -> Map)
    bool fsrSupported() const { return fsrOk_; }             // FSR 3.1.4 upscaling + frame generation usable
    bool frameGenerated() const { return genShown_; }        // the last present() showed a generated frame first
    bool fsrActive() const { return fsrScene_; }             // the current / last frame's scene went through FSR
    // Enhanced sun shadows drawn with these settings (any mode; the app then skips LiT's blob shadows).
    static bool dynamicShadows(const Settings& s) {
        return s.fidelity == Fidelity::Enhanced && !s.frame43 && s.shadows != Shadows::Off;
    }
    float maxAnisotropy() const { return maxAniso_; }
    int msaaSamples() const { return msaaSamples_; }
    uint32_t validationMessages() const { return validationMessages_; }

    void uploadScene(const Scene& scene);
    // Dynamic actor meshes (engine space, Mesh::texture indexes the uploaded scene's textures). Kept until the next
    // call or uploadScene; drawScene streams them and draws them in the same passes as the scene meshes.
    // meshes[0, shadowCasters) are world actors that cast and receive the Enhanced sun shadows; the rest (HUD
    // models) neither.
    void setActorMeshes(std::span<const Mesh> meshes, size_t shadowCasters = SIZE_MAX);
    // 30 Hz simulation tick selecting the current frame of Scene::textureAnims (Mesh::textureAnim).
    void setTextureTick(uint32_t tick) { texTick_ = tick; }
    // LiT zone/portal visibility (bb/render/zones.h), as the GL renderer: only the rasterised passes skip hidden
    // zones; the shadow map and the ray-tracing BLAS keep every zone.
    void setZoneView(const render::ZoneView& v) { zoneView_ = v; }
    const std::vector<render::ZoneEntry>& zoneTable() const { return zoneTable_; }
    // Enhanced water (Settings::water): bodies wading / falling in this frame (positions, see render::WaterBody) and the
    // seconds since the previous call. Drives the wave field drawScene steps; the game simulation never reads it back.
    void setWaterBodies(std::span<const render::WaterBody> bodies, float dt);

    // 2D images: returns a bindless slot handle, 0 on invalid input.
    uint32_t createImage(const Texture& tex);
    void destroyImage(uint32_t image);

    void beginFrame(const Settings& s);
    void drawScene(const Camera& cam, const Settings& s);
    void drawImage(uint32_t image, float x, float y, float w, float h, bool smooth, const uint8_t rgba[4] = nullptr);
    void drawRect(float x, float y, float w, float h, const uint8_t rgba[4]);
    void drawText(float x, float y, int scale, std::string_view text, const uint8_t rgba[4]);
    static constexpr int kGlyph = 8;

    // Copies the swapchain image to the host (submits the frame). Call after drawing, before present(). With frame
    // generation and BBTIME_VK_SHOT_GENERATED=1 the frame interpolated before it (with this frame's 2D layer) is
    // also written, as <stem>-generated<ext>.
    bool screenshot(const std::filesystem::path& path);
    void present();

    int width() const { return w_; }
    int height() const { return h_; }
    int drawCalls() const { return drawCalls_; }

private:
    struct Buffer { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void* map = nullptr; VkDeviceSize size = 0; };
    struct Image { VkImage img = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; };
    struct Frame { VkCommandBuffer cmd = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE; VkSemaphore acquired = VK_NULL_HANDLE;
                   Buffer stream; VkDeviceSize used = 0; std::vector<Buffer> garbage;
                   // Enhanced lighting: light UBO + set 1; ray tracing: per-frame actor BLAS, TLAS and their inputs
                   Buffer light, geos, instances, scratch;
                   struct Accel { VkAccelerationStructureKHR as = VK_NULL_HANDLE; Buffer buf; } actorBlas, tlas;
                   VkDescriptorSet lightSet = VK_NULL_HANDLE;
                   // Frame generation: the frame renders into realUi (scene + 2-D) and genUi (interpolated scene + 2-D);
                   // the present thread copies [0] genUi and [1] realUi into swapchain images (presentPool_).
                   Image realUi, genUi;
                   VkCommandBuffer copyCmd[2]{}; VkFence copyFence[2]{}; VkSemaphore copyAcquired[2]{}; bool copyPending[2]{}; };
    // caster: drawn into the shadow map / BLAS; receiver: shadowed (and AO for opaque + alpha-tested)
    // material: packHalf2x16(reflection strength, roughness) for ray-traced reflections, 0 = none
    // water: Enhanced stylised water surface (reflectiveMaterials: translucent / animated blue up-facing, flat navy planes);
    // sheen: a translucent layer just over such a plane that the stylised water replaces (not drawn with it); bed: such a
    // plane under a higher water surface (drawn as is, the Enhanced water's fill covers it)
    struct GpuMesh { uint32_t texture; Blend blend; bool twoSided, actor, caster, receiver; int32_t baseVertex; uint32_t firstIndex, indexCount; glm::vec3 center; int32_t textureAnim; uint32_t material; bool reactive; int16_t zone; bool water, sheen, bed; };
    struct UiDraw { VkBuffer buf; VkDeviceSize off; uint32_t count; };
    struct RtGeo { uint64_t vtx, idx; uint32_t first; int32_t base; uint32_t tex, pad; }; // scene.frag Geo
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
    void addMeshes(std::span<const Mesh> meshes, bool actor, size_t casters, std::vector<Vertex>& verts, std::vector<uint32_t>& idx);
    std::vector<glm::vec3> reflectiveMaterials(const Scene& scene);  // returns each scene mesh's texture mean colour
    void waterTargets(VkExtent2D ext);
    bool waterWaves(const Camera& cam, glm::vec4& field);
    void waterCopy(VkImage color, VkImage depth, bool depthResolved);
    void updateFsr(const Settings& s);
    void fsrResolve(const Settings& s);
    void destroyFsrTargets();
    Image makeImage(VkExtent2D ext, VkFormat fmt, VkImageUsageFlags usage);
    // clear: the frame's first pass (else colour is loaded). Enhanced water: storeDepth keeps the depth (with MSAA also
    // resolved, sample 0, into the water depth copy at the end of the pass), loadDepth resumes on it.
    void beginPass(bool clear, bool storeDepth = false, bool loadDepth = false);
    void buildAccel(VkCommandBuffer cb, VkAccelerationStructureTypeKHR type, std::span<const VkAccelerationStructureGeometryKHR> geoms,
                    std::span<const VkAccelerationStructureBuildRangeInfoKHR> ranges, bool fastBuild, Frame::Accel& out, Buffer& scratch);
    void blasGeometry(const GpuMesh& m, VkDeviceAddress vtx, VkDeviceAddress idx, uint32_t maxVertex,
                      std::vector<VkAccelerationStructureGeometryKHR>& geoms, std::vector<VkAccelerationStructureBuildRangeInfoKHR>& ranges,
                      std::vector<RtGeo>& table) const;
    void growBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host, std::vector<Buffer>* garbage = nullptr);
    VkDeviceAddress address(VkBuffer b) const;
    void destroy(Frame::Accel& a);
    void createPipelines();
    void recreateSwapchain();
    void destroySwapchain();
    void submitFrame(Buffer* readback, Buffer* genReadback = nullptr);
    // Frame-generation present thread (etiyami's pattern): owns the swapchain while frame generation runs, shows each
    // frame's generated image and then its real one half a rendered-frame interval later.
    struct PresentJob { uint32_t slot; bool gen; uint64_t ns; };
    void startPresenter();
    void stopPresenter();
    void drainPresents(); // wait until every queued present job ran; rethrows a present-thread failure
    void waitIdle();      // drainPresents + vkDeviceWaitIdle under the queue lock
    void presentJob(const PresentJob& j);
    bool showImage(Frame& f, int k, const Image& src);
    std::pair<VkBuffer, VkDeviceSize> streamWrite(const void* data, VkDeviceSize size);
    void flush2D();
    void quad2D(uint32_t tex, uint32_t sampler, float x, float y, float w, float h,
                float u0, float v0, float u1, float v1, const uint8_t rgba[4]);
    void freeScene();

    SDL_Window* window_ = nullptr;
    std::string vendor_, rendererName_, version_, diag_, rtProbe_, fsrProbe_;
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

    VkSampler samplers_[5]{};
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipe3D_[2][4]{}; // [msaa][Opaque+AlphaTest, Alpha, Additive, Subtract]
    VkPipeline pipe2D_[2]{};
    // Enhanced lighting: [kind: unlit, shadow map, ray queries][motion vectors][msaa][blend]. mv = 0: scene_lit.vert +
    // scene_map/rt.frag (unlit = pipe3D_); mv = 1 (FSR, no MSAA): scene_mv.vert + scene_mv/map_mv/rt_mv.frag with a
    // second colour attachment for motion vectors.
    VkPipeline pipeLit_[3][2][2][4]{};
    VkPipeline pipeShadow_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout lightLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool lightPool_ = VK_NULL_HANDLE;
    VkSampler shadowSampler_ = VK_NULL_HANDLE;
    Image shadowMap_;                   // kCascades-layer depth array (view = all layers)
    VkImageView shadowLayers_[4]{};     // one attachment view per cascade
    Shadows drawnMode_ = Shadows::Off;  // last frame's effective mode (diagnostics line)

    // Ray tracing (VK_KHR_acceleration_structure + VK_KHR_ray_query), off without the extensions or with
    // BBTIME_VK_NO_RT=1. Static scene BLAS built at uploadScene; actor BLAS + TLAS rebuilt per frame (Frame).
    bool rt_ = false;
    bool reflectionsOn_ = false;  // last drawScene ran ray-traced reflections (diagnostics)
    VkDeviceSize scratchAlign_ = 256;
    PFN_vkCreateAccelerationStructureKHR vkCreateAS_ = nullptr;
    PFN_vkDestroyAccelerationStructureKHR vkDestroyAS_ = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR vkASSizes_ = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAS_ = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR vkASAddress_ = nullptr;
    Frame::Accel sceneBlas_;
    std::vector<RtGeo> sceneGeos_, actorGeos_;  // BLAS geometry order = scene.frag Geo index
    glm::vec3 casterLo_{0.0f}, casterHi_{0.0f}; // scene caster bounds (shadow depth range, ray length)
    glm::vec3 toSun_{0.0f, 1.0f, 0.0f};            // unit vector toward the level's sun
    bool sunPending_ = false;                      // toSun_ follows the first drawScene camera after uploadScene
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
    bool recording_ = false, submitted_ = false, msaaOn_ = false, passOpen_ = false;

    VkDeviceMemory sceneMem_ = VK_NULL_HANDLE;
    Buffer sceneVbo_, sceneIbo_;
    std::vector<uint32_t> sceneSlots_;  // per created texture
    std::vector<uint32_t> texSlot_;     // per Scene::textures index (white when undecodable)
    std::vector<GpuMesh> meshes_;       // scene meshes, then actor meshes (actor = true)
    std::vector<TexAnim> texAnims_;
    uint32_t texTick_ = 0;
    std::vector<WorldZone> zones_;  // Scene::zones of the uploaded scene
    render::ZoneView zoneView_;
    std::vector<render::ZoneEntry> zoneTable_;
    std::vector<uint8_t> zoneDrawn_;
    size_t sceneMeshCount_ = 0;
    std::vector<Vertex> actorVerts_;
    std::vector<uint32_t> actorIdx_;
    // Smooth per-vertex normals (R8G8B8A8_SNORM, vertex binding 1): angle-weighted over welded positions with a
    // crease angle (smoothNormals in renderer.cpp). Scene: device-local at uploadScene; actors: per setActorMeshes.
    Buffer sceneNrm_;
    std::vector<uint32_t> actorNrm_;
    // Motion vectors (FSR): last frame's actor vertices (binding 2 prev position) when its mesh layout matches.
    std::vector<Vertex> prevActorVerts_;
    std::vector<uint32_t> actorLayout_, prevActorLayout_;  // vertex count per actor mesh
    glm::mat4 prevViewProj_{1.0f};
    bool prevValid_ = false;

    // FSR 3.1.4 (src/fsr.h). fsrMode_: this frame renders the scene at fsrRender_ into fsrColor_/fsrMotion_/fsrDepth_,
    // upscales into fsrOutput_ and blits that to the swapchain; with frame generation also interpolates genImage_
    // (previous..this frame), copies it to genUi_ and draws the frame's 2D layer onto it for present().
    std::unique_ptr<Fsr> fsr_;
    bool fsrOk_ = false, fsrMode_ = false, fgMode_ = false, genPending_ = false, genShown_ = false, fsrReset_ = true;
    Upscaling fsrScale_ = Upscaling::Off;
    bool fsrFg_ = false;
    VkExtent2D fsrRender_{}, fsrDisplay_{};
    uint32_t fsrFrame_ = 0;
    uint64_t fsrLastNs_ = 0;
    float fsrJitter_[2] = {0.0f, 0.0f};
    Image fsrColor_, fsrMotion_, fsrReactive_, fsrDepth_, fsrDilatedDepth_, fsrDilatedMotion_, fsrPrevDepth_, fsrOutput_, genImage_;
    std::vector<UiDraw> uiDraws_;  // 2D batches recorded after this frame's FSR scene (replayed onto Frame::genUi)
    VkImage target_ = VK_NULL_HANDLE;     // this frame's colour target: the swapchain image, or Frame::realUi (FG)
    VkImageView targetView_ = VK_NULL_HANDLE;
    std::mutex queueMutex_;               // queue_ (submits, presents, waits) while the present thread exists
    std::mutex jobMutex_;
    std::condition_variable jobCv_;
    std::deque<PresentJob> jobs_;
    uint64_t jobsQueued_ = 0, jobsDone_ = 0, slotJob_[2]{};
    bool presenterStop_ = false;
    std::exception_ptr presenterError_;
    std::thread presenter_;
    std::atomic<bool> presentStale_{false};  // the present thread saw an out-of-date / suboptimal swapchain
    VkCommandPool presentPool_ = VK_NULL_HANDLE;
    double frameIntervalMs_ = 0.0;        // present thread: smoothed time between rendered frames
    uint64_t lastJobNs_ = 0;
    bool fsrScene_ = false;        // drawScene ran the FSR path this frame
    bool sceneDrawn_ = false;      // drawScene ran this frame
    std::vector<uint64_t> presentTimes_; // SDL_GetTicksNS after each vkQueuePresentKHR (BBTIME_VK_PRESENT_LOG)
    float colorScale_ = 1.0f, texGamma_ = 1.0f;  // Scene::colorScale / Scene::pcTexGamma of the uploaded scene
    bool linearOriginal_ = false;  // LiT: bugs.exe GL sets GL_LINEAR min/mag (0x4220c4), no mipmaps; PC ABR texels (0x420d10)

    // Enhanced water: surfaces / shore walls of the uploaded scene, the camera-following wave field, the bodies of
    // this frame (setWaterBodies) and their previous positions. Bindless slots: copies of the opaque scene's colour
    // and depth (refraction, absorption, shoreline; per frame), the wave heights (R16F, per frame), the procedural
    // normal map (init). waterTime_: seconds of water animation.
    struct WaterTrack { uint32_t id; glm::vec3 feet; float bob; bool seen; };
    render::WaterSurfaces waterSurf_;
    std::unique_ptr<render::WaveField> waves_;
    std::vector<render::WaterBody> waterBodies_;
    std::vector<WaterTrack> waterTracks_;
    std::vector<uint16_t> waveHalf_;
    float waterDt_ = 0.0f, waterTime_ = 0.0f;
    bool waterCartoon_ = false;  // the Enhanced water toon-shaded (Water::Cartoon; diagnostics line)
    bool waterOn_ = false;  // this drawScene draws the Enhanced water
    uint32_t waterColorSlot_ = kNoWater, waterDepthSlot_ = kNoWater, waveSlot_ = kNoWater, waterNormalSlot_ = kNoWater;
    VkExtent2D waterExtent_{};
    static constexpr uint32_t kNoWater = UINT32_MAX;
    size_t seaFill_ = SIZE_MAX;  // meshes_ index of the open-water fill plane (uploadScene), SIZE_MAX = none

    std::vector<V2> batch_;
};

} // namespace bb::render_vk
