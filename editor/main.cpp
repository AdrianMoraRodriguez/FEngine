/**
 * @file main.cpp
 * @brief Entry point of the FEngine editor.
 */

#include "core/Log.h"
#include "core/Version.h"
#include "platform/Window.h"
#include "gfx/VulkanContext.h"
#include "gfx/Renderer.h"
#include "gfx/OffscreenBuffer.h"
#include "scene/Scene.h"
#include "scene/ComponentRegistry.h"
#include "scene/SceneSerializer.h"
#include "editor/AssetBrowser.h"
#include "editor/HierarchyPanel.h"
#include "editor/InspectorPanel.h"
#include "editor/ViewportPanel.h"
#include "editor/StatisticsPanel.h"
#include "gfx/MeshPipeline.h"
#include "gfx/MeshBuffer.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <glm/gtc/matrix_transform.hpp>
#include <windows.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace {

// ---------------------------------------------------------------------------
// ImGui initialisation
// ---------------------------------------------------------------------------

VkDescriptorPool g_imguiPool = VK_NULL_HANDLE;

bool initImGui(const fe::VulkanContext& ctx,
               const fe::Window& window,
               const fe::Renderer& renderer) {

    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER,                1  },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          1  },
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = 16;
    poolInfo.poolSizeCount = 3;
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
    initInfo.ApiVersion          = VK_API_VERSION_1_3;
    initInfo.Instance            = ctx.instance();
    initInfo.PhysicalDevice      = ctx.physicalDevice();
    initInfo.Device              = ctx.device();
    initInfo.QueueFamily         = ctx.graphicsQueueFamily();
    initInfo.Queue               = ctx.graphicsQueue();
    initInfo.DescriptorPool      = g_imguiPool;
    initInfo.MinImageCount       = 2;
    initInfo.ImageCount          = 2;
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
            ImGui::MenuItem("Hierarchy",     nullptr, nullptr);
            ImGui::MenuItem("Inspector",     nullptr, nullptr);
            ImGui::MenuItem("Viewport",      nullptr, nullptr);
            ImGui::MenuItem("Statistics",    nullptr, nullptr);
            ImGui::MenuItem("Asset Browser", nullptr, nullptr);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGui::End();
}

} // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main() {
    // Resolve paths relative to the executable, not the working directory.
    char exePathBuf[MAX_PATH];
    GetModuleFileNameA(nullptr, exePathBuf, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePathBuf).parent_path();
    std::filesystem::path shaderDir  = exeDir / "shaders";
    std::filesystem::path assetsDir  = std::filesystem::current_path() / "assets";

    FE_INFO("%s", fe::engineVersionString());

    fe::Window window;
    if (!window.init(fe::WindowSpec{})) return 1;

    fe::VulkanContext vkCtx;
    if (!vkCtx.init(window)) return 1;

    fe::Renderer renderer;
    if (!renderer.init(vkCtx, window)) return 1;

    if (!initImGui(vkCtx, window, renderer)) return 1;

    fe::OffscreenBuffer offscreen;
    if (!offscreen.init(vkCtx, 1280, 720)) return 1;

    // Test mesh — a coloured cube.
    const std::vector<fe::Vertex> cubeVertices = {
        // Front face
        {{-0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}},
        {{ 0.5f, -0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}},
        {{ 0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}},
        {{-0.5f,  0.5f,  0.5f}, {1.0f, 1.0f, 0.0f}},
        // Back face
        {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 1.0f}},
        {{ 0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 1.0f}},
        {{ 0.5f,  0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}},
        {{-0.5f,  0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
    };

    const std::vector<uint32_t> cubeIndices = {
        0,1,2, 2,3,0,  // front
        4,6,5, 6,4,7,  // back
        4,5,1, 1,0,4,  // bottom
        3,2,6, 6,7,3,  // top
        1,5,6, 6,2,1,  // right
        4,0,3, 3,7,4,  // left
    };

    fe::MeshBuffer cubeMesh;
    if (!cubeMesh.upload(vkCtx, cubeVertices, cubeIndices)) return 1;

    fe::MeshPipeline meshPipeline;
    if (!meshPipeline.init(vkCtx, fe::OffscreenBuffer::format(),
                       shaderDir.string().c_str())) return 1;


    fe::registerBuiltinComponents();

    // Test scene.
    fe::Scene        scene;
    entt::entity     selectedEntity = entt::null;
    scene.setName("Test Scene");
    {
        entt::entity root   = scene.create("Root");
        entt::entity child  = scene.create("Child", root);
        entt::entity camera = scene.create("Camera", root);
        (void)child;
        scene.registry().emplace<fe::CameraComponent>(camera);
        scene.transform(camera).position = {0.0f, 5.0f, 10.0f};
        scene.updateTransforms();

        fe::SceneSerializer::saveToFile(scene, "test_scene.fescene");
        fe::Scene loaded;
        fe::SceneSerializer::loadFromFile(loaded, "test_scene.fescene");
        FE_INFO("Round-trip OK: '%s' with %zu entities",
                loaded.name().c_str(),
                loaded.registry().storage<entt::entity>().size());
    }

    // Frame timing — declared before panels so StatisticsPanel can
    // hold const references to them.
    uint64_t frameCount = 0;
    double   lastTime   = 0.0;
    double   deltaTime  = 0.0;

    // Panels — each owns references to its dependencies.
    // Construction order does not matter; all dependencies are already alive.
    fe::HierarchyPanel  hierarchyPanel {scene, selectedEntity};
    fe::InspectorPanel  inspectorPanel {scene, selectedEntity};
    fe::ViewportPanel   viewportPanel  {offscreen};
    fe::StatisticsPanel statisticsPanel{scene, renderer, deltaTime, frameCount};
    fe::AssetBrowser assetBrowser{assetsDir};

    // Uniform panel array — adding a new panel is one line here.
    fe::Panel* panels[] = {
        &hierarchyPanel,
        &inspectorPanel,
        &viewportPanel,
        &statisticsPanel,
        &assetBrowser,
    };

    bool running = true;

    while (running && !window.shouldClose()) {
        const double now = glfwGetTime();
        deltaTime = now - lastTime;
        lastTime  = now;
        ++frameCount;

        window.pollEvents();

        // ----- ImGui frame -----
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        beginDockspace(running);

        for (auto* panel : panels)
            panel->onImGui();

        ImGui::Render();   // all panels must be submitted before this

        // ----- GPU frame -----
        if (!renderer.beginFrame()) continue;

        // 1. Offscreen pass — scene geometry (placeholder clear for now).
        fe::Renderer::beginOffscreenPass(
            renderer.currentCommandBuffer(),
            offscreen.image(),
            offscreen.imageView(),
            {offscreen.width(), offscreen.height()});

        // Set viewport and scissor dynamically.
        VkViewport vp{};
        vp.width    = static_cast<float>(offscreen.width());
        vp.height   = static_cast<float>(offscreen.height());
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(renderer.currentCommandBuffer(), 0, 1, &vp);

        VkRect2D scissor{};
        scissor.extent = {offscreen.width(), offscreen.height()};
        vkCmdSetScissor(renderer.currentCommandBuffer(), 0, 1, &scissor);

        // Rotate the cube over time.
        float angle = static_cast<float>(glfwGetTime());
        glm::mat4 model = glm::rotate(glm::mat4(1.0f), angle,
                                       glm::vec3(0.5f, 1.0f, 0.0f));
        glm::mat4 view  = glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f),
                                       glm::vec3(0.0f, 0.0f, 0.0f),
                                       glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 proj  = glm::perspective(
            glm::radians(60.0f),
            static_cast<float>(offscreen.width()) /
            static_cast<float>(offscreen.height()),
            0.1f, 100.0f);

        fe::MeshPushConstants pc;
        pc.mvp = proj * view * model;

        meshPipeline.bind(renderer.currentCommandBuffer(), pc);
        cubeMesh.draw(renderer.currentCommandBuffer());

        fe::Renderer::endOffscreenPass(
            renderer.currentCommandBuffer(),
            offscreen.image());

        // 2. Swapchain pass — ImGui on top of the offscreen image.
        renderer.beginSwapchainPass();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
                                        renderer.currentCommandBuffer());
        renderer.endSwapchainPass();

        renderer.endFrame();
    }

    FE_INFO("Shutting down");
    cubeMesh.destroy();
    meshPipeline.shutdown();
    vkDeviceWaitIdle(vkCtx.device());
    offscreen.shutdown();
    shutdownImGui(vkCtx.device());
    renderer.shutdown();
    vkCtx.shutdown();
    window.shutdown();

    return 0;
}
