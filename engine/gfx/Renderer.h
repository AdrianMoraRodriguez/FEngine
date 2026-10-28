#pragma once

/**
 * @file Renderer.h
 * @brief Swapchain management, per-frame resources and render passes.
 */

#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

namespace fe {

class VulkanContext;
class Window;
class OffscreenBuffer;

/**
 * @brief Per-frame GPU resources.
 *
 * Each frame in flight owns its own command pool and fence, so the CPU can
 * record the next frame while the GPU is still executing the previous one.
 */
struct FrameData {
    VkCommandPool   commandPool   = VK_NULL_HANDLE; ///< Pool for commandBuffer.
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE; ///< Primary command buffer.

    /// CPU-side fence. Waited on before reusing this frame slot.
    VkFence inFlightFence = VK_NULL_HANDLE;
};

/**
 * @brief Manages the swapchain, per-frame resources, and the frame loop.
 *
 * A frame is recorded as:
 * @code
 * if (!renderer.beginFrame()) continue;
 *
 * Renderer::beginOffscreenPass(cmd, offscreen);   // scene
 * // ... draw calls ...
 * Renderer::endOffscreenPass(cmd, offscreen);
 *
 * renderer.beginSwapchainPass();                  // UI on top
 * // ... ImGui draw data ...
 * renderer.endSwapchainPass();
 *
 * renderer.endFrame();
 * @endcode
 *
 * Uses Vulkan 1.3 dynamic rendering: no VkRenderPass or VkFramebuffer.
 *
 * ### Semaphore strategy
 * The two semaphores of a frame follow different rules, because they are
 * reused at different moments:
 *
 *   - **imageAvailable** (the swapchain tells the GPU an image is ready) is
 *     kept per *frame slot*. It is safe to reuse once the slot's fence has
 *     been waited on, which beginFrame() always does first.
 *   - **renderFinished** (the GPU tells the presentation engine the image is
 *     drawn) is kept per *swapchain image*. The presentation engine may still
 *     be reading it long after the fence is signalled, and the only moment
 *     that proves it is done is the moment the same image is acquired again.
 *     Keeping it per frame slot instead lets it be signalled again while a
 *     previous present still waits on it.
 */
class Renderer {
public:
    /// Maximum number of frames the GPU may process concurrently.
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    /**
     * @brief Creates the swapchain and all per-frame resources.
     *
     * @param ctx    Initialised Vulkan context.
     * @param window Window the swapchain presents to.
     * @return `true` on success.
     */
    bool init(const VulkanContext& ctx, Window& window);

    /// Destroys all resources. Safe to call if init() was never called.
    void shutdown();

    /**
     * @brief Begins a new frame and opens the command buffer.
     *
     * Waits for the current slot's fence, acquires a swapchain image and
     * transitions it for rendering. No render pass is opened here.
     *
     * @return `false` if the frame must be skipped (minimised window or
     *         out-of-date swapchain). The caller should `continue`.
     */
    bool beginFrame();

    /// Ends the frame: transitions the image, submits and presents.
    void endFrame();

    /// Opens a render pass on the swapchain image (cleared to the editor background).
    void beginSwapchainPass();

    /// Closes the render pass opened by beginSwapchainPass().
    void endSwapchainPass();

    /**
     * @brief Opens a render pass on the offscreen HDR colour + depth targets.
     *
     * Transitions the images and clears them (colour to the scene
     * background, as linear light; depth to 1.0). The finished 8-bit image is
     * not touched here: PostProcess::render() makes it from the HDR one.
     *
     * @param cmd       Command buffer from currentCommandBuffer().
     * @param offscreen Target buffer.
     */
    static void beginOffscreenPass(VkCommandBuffer cmd,
                                   const OffscreenBuffer& offscreen);

    /**
     * @brief Closes the offscreen pass and makes the HDR image readable.
     *
     * Transitions the HDR image to SHADER_READ_ONLY_OPTIMAL so PostProcess
     * can sample it.
     */
    static void endOffscreenPass(VkCommandBuffer cmd,
                                 const OffscreenBuffer& offscreen);

    /// Command buffer open for recording during the current frame.
    VkCommandBuffer currentCommandBuffer() const;

    /**
     * @brief Index of the frame slot being recorded, from 0 to
     *        MAX_FRAMES_IN_FLIGHT - 1.
     *
     * Resources that the CPU rewrites every frame (such as uniform buffers)
     * exist once per slot and are picked with this index. Valid between
     * beginFrame() and endFrame().
     */
    uint32_t currentFrameIndex() const { return m_currentFrame; }

    /// @return Current swapchain image extent in pixels.
    VkExtent2D swapchainExtent() const { return m_swapchainExtent; }

    /// @return Current swapchain image format.
    VkFormat swapchainFormat() const { return m_swapchainFormat; }

private:
    void createSwapchain(uint32_t width, uint32_t height);
    void destroySwapchain();
    void recreateSwapchain();

    const VulkanContext* m_ctx    = nullptr;
    Window*              m_window = nullptr;

    VkSwapchainKHR           m_swapchain       = VK_NULL_HANDLE;
    VkFormat                 m_swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D               m_swapchainExtent = {0, 0};
    std::vector<VkImage>     m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;

    /// One semaphore per frame slot, indexed by m_currentFrame.
    std::vector<VkSemaphore> m_imageAvailableSemaphores;

    /// One semaphore per swapchain image, indexed by m_imageIndex.
    /// Created and destroyed together with the swapchain.
    std::vector<VkSemaphore> m_renderFinishedSemaphores;

    FrameData m_frames[MAX_FRAMES_IN_FLIGHT];
    uint32_t  m_currentFrame = 0;
    uint32_t  m_imageIndex   = 0;
};

} // namespace fe
