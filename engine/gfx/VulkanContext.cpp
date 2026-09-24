#include "gfx/VulkanContext.h"
#include "core/Log.h"
#include "platform/Window.h"
#include <GLFW/glfw3.h>
#include <VkBootstrap.h>

#define GLFW_INCLUDE_NONE

namespace fe {
  // ----------------------------------------------------------------
  // Destruction
  // ----------------------------------------------------------------

  VulkanContext::~VulkanContext() {
    shutdown();
  }

  void VulkanContext::shutdown() {
    if (m_device != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(m_device);
      vkDestroyDevice(m_device, nullptr);
      m_device = VK_NULL_HANDLE;
    }

    if (m_surface != VK_NULL_HANDLE) {
      vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
      m_surface = VK_NULL_HANDLE;
    }

    if (m_debugMessenger != VK_NULL_HANDLE) {
      auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
      if (fn) {
        fn(m_instance, m_debugMessenger, nullptr);
      }
      m_debugMessenger = VK_NULL_HANDLE;
    }

    if (m_instance != VK_NULL_HANDLE) {
      vkDestroyInstance(m_instance, nullptr);
      m_instance = VK_NULL_HANDLE;
    }
  }

  // ----------------------------------------------------------------
  // Initialisation
  // ----------------------------------------------------------------

  bool VulkanContext::init(const Window& window, const VulkanContextSpec& spec) {

    // ------------------------------------------------------------------
    // 1. Instance
    //
    // The instance is the connection between the application and the Vulkan
    // loader. It holds the list of active layers and extensions.
    // ------------------------------------------------------------------

    vkb::InstanceBuilder builder;
    builder.set_app_name(spec.appName)
            .set_engine_name("Fractalis Engine")
            .require_api_version(1, 3, 0);
    if (spec.enableValidation) {
      builder.request_validation_layers()
              .set_debug_callback(
                [](VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                    VkDebugUtilsMessageTypeFlagsEXT,
                    const VkDebugUtilsMessengerCallbackDataEXT* data,
                    void*) -> VkBool32 {
                      if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                        FE_ERROR("[Vulkan] %s", data->pMessage);
                      else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                        FE_WARN("[Vulkan] %s", data->pMessage);
                      else
                        FE_TRACE("[Vulkan] %s", data->pMessage);
                      return VK_FALSE;
                      });
    }
    auto instanceResult = builder.build();
    if (!instanceResult) {
      FE_ERROR("Failed to create Vulkan instance: %s", instanceResult.error().message().c_str());
      return false;
    }
    vkb::Instance vkbInstance = instanceResult.value();
    m_instance       = vkbInstance.instance;
    m_debugMessenger = vkbInstance.debug_messenger;

    FE_INFO("Vulkan instance created (API 1.3, validation %s)",
            spec.enableValidation ? "enabled" : "disabled");

    // ------------------------------------------------------------------
    // 2. Surface
    //
    // A surface is the bridge between a Vulkan instance and a native
    // window. GLFW handles the platform-specific work (Win32 on Windows).
    // The surface must be created before selecting the physical device
    // because presentation support is a per-surface property.
    // ------------------------------------------------------------------

    if (glfwCreateWindowSurface(m_instance, window.handle(), nullptr, &m_surface)
            != VK_SUCCESS) {
        FE_ERROR("Failed to create Vulkan window surface");
        return false;
    }

    FE_INFO("Window surface created");

    // ------------------------------------------------------------------
    // 3. Physical device
    //
    // Enumerate the available GPUs and pick the best one. We require:
    //   - Vulkan 1.3
    //   - dynamic rendering  (no render-pass objects needed)
    //   - synchronization2   (cleaner pipeline barriers)
    //   - bufferDeviceAddress (required by modern Vulkan workflows)
    //   - descriptorIndexing  (bindless resources, needed later)
    //
    // We prefer discrete GPUs but fall back to integrated if nothing
    // else is available.
    // ------------------------------------------------------------------

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType                 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.bufferDeviceAddress   = VK_TRUE;
    features12.descriptorIndexing    = VK_TRUE;

    vkb::PhysicalDeviceSelector selector{vkbInstance};
    auto physResult = selector
        .set_surface(m_surface)
        .set_minimum_version(1, 3)
        .set_required_features_13(features13)
        .set_required_features_12(features12)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
        .select();

    if (!physResult) {
        FE_ERROR("Failed to select a suitable GPU: %s",
                 physResult.error().message().c_str());
        return false;
    }

    vkb::PhysicalDevice vkbPhysDevice = physResult.value();
    m_physicalDevice = vkbPhysDevice.physical_device;

    FE_INFO("GPU selected: %s", vkbPhysDevice.name.c_str());

    // ------------------------------------------------------------------
    // 4. Logical device
    //
    // The logical device is the application's interface to the physical
    // device. Creating it also allocates the requested queues.
    // vk-bootstrap enables the required extensions (VK_KHR_swapchain,
    // etc.) automatically based on what PhysicalDeviceSelector found.
    // ------------------------------------------------------------------

    vkb::DeviceBuilder deviceBuilder{vkbPhysDevice};
    auto deviceResult = deviceBuilder.build();

    if (!deviceResult) {
        FE_ERROR("Failed to create logical device: %s",
                 deviceResult.error().message().c_str());
        return false;
    }

    vkb::Device vkbDevice = deviceResult.value();
    m_device = vkbDevice.device;

    // ------------------------------------------------------------------
    // 5. Queues
    //
    // Retrieve the graphics and present queue handles. On most desktop
    // GPUs these belong to the same queue family, but the spec does not
    // guarantee it: we always treat them as potentially distinct.
    // ------------------------------------------------------------------

    auto graphicsQueueResult = vkbDevice.get_queue(vkb::QueueType::graphics);
    auto presentQueueResult  = vkbDevice.get_queue(vkb::QueueType::present);

    if (!graphicsQueueResult) {
        FE_ERROR("Failed to get graphics queue: %s",
                 graphicsQueueResult.error().message().c_str());
        return false;
    }

    if (!presentQueueResult) {
        FE_ERROR("Failed to get present queue: %s",
                 presentQueueResult.error().message().c_str());
        return false;
    }

    m_graphicsQueue       = graphicsQueueResult.value();
    m_presentQueue        = presentQueueResult.value();
    m_graphicsQueueFamily =
        vkbDevice.get_queue_index(vkb::QueueType::graphics).value();
    m_presentQueueFamily  =
        vkbDevice.get_queue_index(vkb::QueueType::present).value();

    FE_INFO("Queues ready (graphics family: %u, present family: %u)",
            m_graphicsQueueFamily, m_presentQueueFamily);

    return true;
  }
} // namespace fe
