#include "gfx/Renderer.h"

#include "gfx/VulkanContext.h"
#include "platform/Window.h"
#include "core/Log.h"

#include <VkBootstrap.h>

namespace fe {
namespace {

// ---------------------------------------------------------------------------
// Utility: image layout transition
//
// In Vulkan, images cannot be read or written in an arbitrary layout. Before
// rendering we move the image to COLOR_ATTACHMENT_OPTIMAL; before presenting
// we move it to PRESENT_SRC_KHR. Each transition is a pipeline barrier that
// tells the GPU when earlier writes are visible to later operations.
//
// Using VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT and VK_ACCESS_2_MEMORY_*_BIT
// is deliberately broad: it serialises all commands, which is safe for every
// case and efficient enough at this stage. More precise masks (e.g. only
// waiting on the colour output stage) can be used later for performance.
// ---------------------------------------------------------------------------

void transitionImage(VkCommandBuffer cmd, VkImage image,
                     VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT
                          | VK_ACCESS_2_MEMORY_READ_BIT;
    barrier.oldLayout     = from;
    barrier.newLayout     = to;
    barrier.image         = image;

    // Target the colour aspect of the whole image (mip 0, layer 0).
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;

    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &barrier;

    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

// ---------------------------------------------------------------------------
// Destruction
// ---------------------------------------------------------------------------

Renderer::~Renderer() { shutdown(); }

void Renderer::shutdown() {
    if (!m_ctx || m_ctx->device() == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_ctx->device());

    for (auto& frame : m_frames) {
        if (frame.commandPool != VK_NULL_HANDLE)
            vkDestroyCommandPool(m_ctx->device(), frame.commandPool, nullptr);
        if (frame.imageAvailableSemaphore != VK_NULL_HANDLE)
            vkDestroySemaphore(m_ctx->device(), frame.imageAvailableSemaphore, nullptr);
        if (frame.renderFinishedSemaphore != VK_NULL_HANDLE)
            vkDestroySemaphore(m_ctx->device(), frame.renderFinishedSemaphore, nullptr);
        if (frame.inFlightFence != VK_NULL_HANDLE)
            vkDestroyFence(m_ctx->device(), frame.inFlightFence, nullptr);

        frame = FrameData{};
    }

    destroySwapchain();

    m_ctx    = nullptr;
    m_window = nullptr;
}

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

bool Renderer::init(const VulkanContext& ctx, Window& window) {
    m_ctx    = &ctx;
    m_window = &window;

    createSwapchain(static_cast<uint32_t>(window.width()),
                    static_cast<uint32_t>(window.height()));
    if (m_swapchain == VK_NULL_HANDLE) return false;

    // Each frame slot gets its own command pool so that reset is cheap and
    // independent: resetting one frame's pool does not affect another.
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = ctx.graphicsQueueFamily();

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    // Start signalled so that the very first beginFrame() does not wait
    // forever on a fence that was never submitted.
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        auto& frame = m_frames[i];

        if (vkCreateCommandPool(ctx.device(), &poolInfo, nullptr,
                                &frame.commandPool) != VK_SUCCESS) {
            FE_ERROR("Failed to create command pool for frame %u", i);
            return false;
        }

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool        = frame.commandPool;
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(ctx.device(), &allocInfo,
                                     &frame.commandBuffer) != VK_SUCCESS) {
            FE_ERROR("Failed to allocate command buffer for frame %u", i);
            return false;
        }

        if (vkCreateSemaphore(ctx.device(), &semInfo, nullptr,
                              &frame.imageAvailableSemaphore) != VK_SUCCESS ||
            vkCreateSemaphore(ctx.device(), &semInfo, nullptr,
                              &frame.renderFinishedSemaphore) != VK_SUCCESS) {
            FE_ERROR("Failed to create semaphores for frame %u", i);
            return false;
        }

        if (vkCreateFence(ctx.device(), &fenceInfo, nullptr,
                          &frame.inFlightFence) != VK_SUCCESS) {
            FE_ERROR("Failed to create fence for frame %u", i);
            return false;
        }
    }

    FE_INFO("Renderer initialised (%u frames in flight)", MAX_FRAMES_IN_FLIGHT);
    return true;
}

// ---------------------------------------------------------------------------
// Swapchain management
// ---------------------------------------------------------------------------

void Renderer::createSwapchain(uint32_t width, uint32_t height) {
    vkb::SwapchainBuilder builder{
        m_ctx->physicalDevice(),
        m_ctx->device(),
        m_ctx->surface()
    };

    auto result = builder
    .set_desired_format({VK_FORMAT_B8G8R8A8_SRGB,
                         VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
    .set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
    .set_desired_extent(width, height)
    .build();

    if (!result) {
        FE_ERROR("Failed to create swapchain: %s",
                 result.error().message().c_str());
        return;
    }

    vkb::Swapchain vkbSwapchain = result.value();
    m_swapchain       = vkbSwapchain.swapchain;
    m_swapchainFormat = vkbSwapchain.image_format;
    m_swapchainExtent = vkbSwapchain.extent;
    m_swapchainImages     = vkbSwapchain.get_images().value();
    m_swapchainImageViews = vkbSwapchain.get_image_views().value();

    FE_INFO("Swapchain created: %ux%u, %zu images",
            m_swapchainExtent.width, m_swapchainExtent.height,
            m_swapchainImages.size());
}

void Renderer::destroySwapchain() {
    if (!m_ctx) return;

    for (auto view : m_swapchainImageViews) {
        if (view != VK_NULL_HANDLE)
            vkDestroyImageView(m_ctx->device(), view, nullptr);
    }
    m_swapchainImageViews.clear();
    m_swapchainImages.clear();

    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_ctx->device(), m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

void Renderer::recreateSwapchain() {
    // Wait for all in-flight work to finish before touching swapchain objects
    // that may be referenced by pending command buffers.
    vkDeviceWaitIdle(m_ctx->device());
    destroySwapchain();
    createSwapchain(static_cast<uint32_t>(m_window->width()),
                    static_cast<uint32_t>(m_window->height()));
    FE_INFO("Swapchain recreated");
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------

bool Renderer::beginFrame() {
    // Nothing to render while the window has no area.
    if (m_window->isMinimized()) return false;

    auto& frame  = m_frames[m_currentFrame];
    VkDevice dev = m_ctx->device();

    // Block until the GPU has finished with this frame slot.
    vkWaitForFences(dev, 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX);

    // Ask the swapchain for the next image to render into.
    // imageAvailableSemaphore will be signalled when the image is safe to write.
    VkResult acquireResult = vkAcquireNextImageKHR(
        dev, m_swapchain, UINT64_MAX,
        frame.imageAvailableSemaphore, VK_NULL_HANDLE,
        &m_imageIndex);

    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        // The swapchain no longer matches the surface (resize, etc.).
        // Recreate and skip this frame.
        recreateSwapchain();
        return false;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
        FE_ERROR("Failed to acquire swapchain image");
        return false;
    }

    // Reset the fence only after we know we will submit work for this slot.
    // Resetting before the acquire check would leave it unsignalled if we
    // return early, causing the next wait to block forever.
    vkResetFences(dev, 1, &frame.inFlightFence);

    // Reset and open the command buffer.
    vkResetCommandBuffer(frame.commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);

    // Transition the swapchain image from whatever layout it is in to
    // COLOR_ATTACHMENT_OPTIMAL so we can render into it.
    // Using UNDEFINED as old layout tells the driver we do not care about
    // its previous contents, which allows it to skip any actual conversion.
    transitionImage(frame.commandBuffer,
                    m_swapchainImages[m_imageIndex],
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Begin dynamic rendering. The clear colour is the engine's background.
    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView   = m_swapchainImageViews[m_imageIndex];
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = {{0.08f, 0.08f, 0.10f, 1.0f}};

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea.offset    = {0, 0};
    renderingInfo.renderArea.extent    = m_swapchainExtent;
    renderingInfo.layerCount           = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments    = &colorAttachment;

    vkCmdBeginRendering(frame.commandBuffer, &renderingInfo);

    return true;
}

void Renderer::endFrame() {
    auto& frame = m_frames[m_currentFrame];

    // Close the dynamic rendering pass opened in beginFrame().
    vkCmdEndRendering(frame.commandBuffer);

    // Transition the image to PRESENT_SRC_KHR so the presentation engine
    // can display it. The GPU will not start this transition until all
    // rendering commands above have completed.
    transitionImage(frame.commandBuffer,
                    m_swapchainImages[m_imageIndex],
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    vkEndCommandBuffer(frame.commandBuffer);

    // Submit the command buffer to the graphics queue.
    // synchronization2 replaces the old VkSubmitInfo with a cleaner struct.
    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = frame.commandBuffer;

    // Wait for the swapchain image to be available before writing to it.
    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = frame.imageAvailableSemaphore;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    // Signal renderFinished when the GPU is done, so present knows when to show it.
    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = frame.renderFinishedSemaphore;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.waitSemaphoreInfoCount   = 1;
    submitInfo.pWaitSemaphoreInfos      = &waitInfo;
    submitInfo.commandBufferInfoCount   = 1;
    submitInfo.pCommandBufferInfos      = &cmdInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos    = &signalInfo;

    // inFlightFence is signalled when the GPU finishes — the CPU waits on
    // it at the start of the next frame that uses this slot.
    vkQueueSubmit2(m_ctx->graphicsQueue(), 1, &submitInfo, frame.inFlightFence);

    // Present the rendered image.
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores    = &frame.renderFinishedSemaphore;
    presentInfo.swapchainCount     = 1;
    presentInfo.pSwapchains        = &m_swapchain;
    presentInfo.pImageIndices      = &m_imageIndex;

    VkResult presentResult = vkQueuePresentKHR(m_ctx->presentQueue(), &presentInfo);

    // Recreate the swapchain if the surface changed. We also check the window's
    // resize flag here since some platforms do not always return SUBOPTIMAL.
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR ||
        presentResult == VK_SUBOPTIMAL_KHR ||
        m_window->consumeResizeFlag()) {
        recreateSwapchain();
    }

    m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

VkCommandBuffer Renderer::currentCommandBuffer() const {
    return m_frames[m_currentFrame].commandBuffer;
}

} // namespace fe
