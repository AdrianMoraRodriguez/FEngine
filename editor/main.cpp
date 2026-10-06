/**
 * @file main.cpp
 * @brief Entry point of the FEngine editor.
 */

#include "core/Log.h"
#include "core/Version.h"
#include "platform/Window.h"
#include "gfx/VulkanContext.h"
#include "gfx/Renderer.h"
#include "scene/Scene.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <functional>

namespace {

// ---------------------------------------------------------------------------
// ImGui initialisation
// ---------------------------------------------------------------------------

VkDescriptorPool g_imguiPool = VK_NULL_HANDLE;

bool initImGui(const fe::VulkanContext& ctx,
               const fe::Window& window,
               const fe::Renderer& renderer) {

    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 },
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = 16;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(ctx.device(), &poolInfo, nullptr, &g_imguiPool)
            != VK_SUCCESS) {
        FE_ERROR("Failed to create ImGui descriptor pool");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = "feditor_layout.ini";

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 4.0f;
    style.FrameRounding     = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.GrabRounding      = 3.0f;

    ImGui_ImplGlfw_InitForVulkan(window.handle(), true);

    VkPipelineRenderingCreateInfoKHR pipelineRenderingInfo{};
    pipelineRenderingInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    pipelineRenderingInfo.colorAttachmentCount    = 1;
    VkFormat fmt = renderer.swapchainFormat();
    pipelineRenderingInfo.pColorAttachmentFormats = &fmt;

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion         = VK_API_VERSION_1_3;
    initInfo.Instance           = ctx.instance();
    initInfo.PhysicalDevice     = ctx.physicalDevice();
    initInfo.Device             = ctx.device();
    initInfo.QueueFamily        = ctx.graphicsQueueFamily();
    initInfo.Queue              = ctx.graphicsQueue();
    initInfo.DescriptorPool     = g_imguiPool;
    initInfo.MinImageCount      = 2;
    initInfo.ImageCount         = 2;
    initInfo.UseDynamicRendering = true;

    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
#ifdef IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = pipelineRenderingInfo;
#endif

    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        FE_ERROR("ImGui_ImplVulkan_Init failed");
        return false;
    }

    FE_INFO("ImGui initialised (docking enabled)");
    return true;
}

void shutdownImGui(VkDevice device) {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    if (g_imguiPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, g_imguiPool, nullptr);
        g_imguiPool = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Dockspace
// ---------------------------------------------------------------------------

void beginDockspace(bool& running) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking             |
        ImGuiWindowFlags_NoTitleBar            |
        ImGuiWindowFlags_NoCollapse            |
        ImGuiWindowFlags_NoResize              |
        ImGuiWindowFlags_NoMove                |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus            |
        ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##DockSpaceHost", nullptr, flags);
    ImGui::PopStyleVar(3);

    ImGui::DockSpace(ImGui::GetID("FEngineDockSpace"), ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Exit", "Alt+F4")) running = false;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Hierarchy",  nullptr, nullptr);
            ImGui::MenuItem("Inspector",  nullptr, nullptr);
            ImGui::MenuItem("Viewport",   nullptr, nullptr);
            ImGui::MenuItem("Statistics", nullptr, nullptr);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Hierarchy panel
// ---------------------------------------------------------------------------

void drawHierarchyNode(fe::Scene& scene, entt::entity e) {
    const auto& name = scene.registry().get<fe::NameComponent>(e).name;
    const auto& rel  = scene.registry().get<fe::Relationship>(e);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (rel.firstChild == entt::null)
        flags |= ImGuiTreeNodeFlags_Leaf;

    bool open = ImGui::TreeNodeEx(
        reinterpret_cast<void*>(static_cast<intptr_t>(
            static_cast<std::uint32_t>(e))),
        flags, "%s", name.c_str());

    if (open) {
        for (entt::entity c = rel.firstChild; c != entt::null;
             c = scene.registry().get<fe::Relationship>(c).nextSibling)
            drawHierarchyNode(scene, c);
        ImGui::TreePop();
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main() {
    FE_INFO("%s", fe::engineVersionString());

    fe::Window window;
    if (!window.init(fe::WindowSpec{})) return 1;

    fe::VulkanContext vkCtx;
    if (!vkCtx.init(window)) return 1;

    fe::Renderer renderer;
    if (!renderer.init(vkCtx, window)) return 1;

    if (!initImGui(vkCtx, window, renderer)) return 1;

    // Build a test scene to verify the ECS compiles and runs correctly.
    fe::Scene scene;
    scene.setName("Test Scene");
    {
        entt::entity root   = scene.create("Root");
        entt::entity child  = scene.create("Child", root);
        entt::entity camera = scene.create("Camera", root);
        (void)child;
        scene.registry().emplace<fe::CameraComponent>(camera);
        scene.updateTransforms();
        FE_INFO("Scene '%s' ready (%zu entities)",
                scene.name().c_str(),
                scene.registry().storage<entt::entity>().size());
    }

    bool     running    = true;
    uint64_t frameCount = 0;
    double   lastTime   = 0.0;
    double   deltaTime  = 0.0;

    while (running && !window.shouldClose()) {
        const double now = glfwGetTime();
        deltaTime = now - lastTime;
        lastTime  = now;
        ++frameCount;

        window.pollEvents();

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        beginDockspace(running);

        // ---- Hierarchy panel ----
        ImGui::Begin("Hierarchy");
        ImGui::Text("Scene: %s", scene.name().c_str());
        ImGui::Separator();
        for (entt::entity e = scene.firstRoot(); e != entt::null;
             e = scene.registry().get<fe::Relationship>(e).nextSibling)
            drawHierarchyNode(scene, e);
        ImGui::End();

        // ---- Inspector panel ----
        ImGui::Begin("Inspector");
        ImGui::TextUnformatted("(select an entity)");
        ImGui::End();

        // ---- Viewport panel ----
        ImGui::Begin("Viewport");
        ImGui::TextUnformatted("Scene will be rendered here.");
        ImGui::End();

        // ---- Statistics panel ----
        ImGui::Begin("Statistics");
        ImGui::Text("Frame time : %.3f ms", deltaTime * 1000.0);
        ImGui::Text("FPS        : %.1f",    deltaTime > 0.0 ? 1.0 / deltaTime : 0.0);
        ImGui::Text("Frame #    : %llu",    frameCount);
        ImGui::Text("Entities   : %zu",     scene.registry().storage<entt::entity>().size());
        ImGui::Text("Viewport   : %u x %u",
                    renderer.swapchainExtent().width,
                    renderer.swapchainExtent().height);
        ImGui::End();

        ImGui::Render();

        if (!renderer.beginFrame()) continue;

        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
                                        renderer.currentCommandBuffer());

        renderer.endFrame();
    }

    FE_INFO("Shutting down");

    vkDeviceWaitIdle(vkCtx.device());

    shutdownImGui(vkCtx.device());
    renderer.shutdown();
    vkCtx.shutdown();
    window.shutdown();

    return 0;
}
