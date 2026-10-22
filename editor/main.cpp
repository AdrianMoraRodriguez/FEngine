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
#include "gfx/SceneRenderer.h"
#include "scene/Scene.h"
#include "scene/ComponentRegistry.h"
#include "scene/SceneSerializer.h"
#include "editor/AssetBrowser.h"
#include "editor/EditorCamera.h"
#include "editor/SceneFileController.h"
#include "editor/HierarchyPanel.h"
#include "editor/InspectorPanel.h"
#include "editor/ViewportPanel.h"
#include "editor/StatisticsPanel.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <glm/gtx/quaternion.hpp>
#include <exception>
#include <cstdlib>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

// Windows header last, with the min/max macros disabled so they do not
// break std::min / std::max in other headers.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

void beginDockspace(bool& running, fe::SceneFileController& sceneFiles) {
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
            sceneFiles.drawMenuItems();
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) running = false;
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGui::End();
}

/// Creates a named entity with a built-in mesh at the given transform.
entt::entity addMeshEntity(fe::Scene& scene,
                           const char* name,
                           fe::AssetHandle mesh,
                           const glm::vec3& position,
                           const glm::vec3& scale) {
    entt::entity e = scene.create(name);

    fe::MeshRenderer renderer;
    renderer.mesh = mesh;
    scene.registry().emplace<fe::MeshRenderer>(e, renderer);

    fe::Transform& t = scene.transform(e);
    t.position = position;
    t.scale    = scale;
    return e;
}

} // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main() {
    // If an exception ever escapes, say what it was before the program dies.
    // Without this, the only trace is a bare "abort() has been called".
    std::set_terminate([] {
        if (std::exception_ptr error = std::current_exception()) {
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& e) {
                FE_ERROR("Unhandled exception: %s", e.what());
            } catch (...) {
                FE_ERROR("Unhandled exception of unknown type");
            }
        }
        std::abort();
    });

    // Resolve shader paths relative to the executable, not the working
    // directory. Assets stay relative to the working directory because they
    // belong to the project, not to the editor binary.
    char exePathBuf[MAX_PATH];
    GetModuleFileNameA(nullptr, exePathBuf, MAX_PATH);
    const std::filesystem::path exeDir    = std::filesystem::path(exePathBuf).parent_path();
    const std::filesystem::path shaderDir = exeDir / "shaders";
    const std::filesystem::path assetsDir = std::filesystem::current_path() / "assets";

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

    fe::SceneRenderer sceneRenderer;
    if (!sceneRenderer.init(vkCtx, shaderDir.string().c_str())) return 1;
    sceneRenderer.meshes().setAssetRoot(assetsDir);
    sceneRenderer.meshes().scanAssets();

    fe::registerBuiltinComponents();

    // ---- Starting scene ----
    fe::Scene    scene;
    entt::entity selectedEntity = entt::null;
    scene.setName("Test Scene");
    {
        addMeshEntity(scene, "Ground", fe::builtin::MeshPlane,
                      {0.0f, 0.0f, 0.0f}, {10.0f, 1.0f, 10.0f});
        addMeshEntity(scene, "Cube", fe::builtin::MeshCube,
                      {0.0f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f});
        addMeshEntity(scene, "Small Cube", fe::builtin::MeshCube,
                      {1.2f, 0.3f, 0.6f}, {0.6f, 0.6f, 0.6f});

        entt::entity camera = scene.create("Camera");
        scene.registry().emplace<fe::CameraComponent>(camera);
        scene.transform(camera).position = {0.0f, 5.0f, 10.0f};

        // A sun, so the lighting can be seen changing: rotate this entity
        // and the shading of every object follows. A light shines along its
        // local -Z axis, so it starts rotated to point the same way as the
        // engine's default light (the direction used when a scene has none).
        entt::entity sun = scene.create("Sun");
        fe::LightComponent light;
        light.type      = fe::LightComponent::Type::Directional;
        light.intensity = 0.75f;   // matches the default light's brightness
        light.shadowSoftness = 1.5f;   // the sun's angular radius, in degrees
        scene.registry().emplace<fe::LightComponent>(sun, light);
        scene.transform(sun).position = {0.0f, 6.0f, 0.0f};
        scene.transform(sun).rotation = glm::rotation(
            glm::vec3(0.0f, 0.0f, -1.0f),
            glm::normalize(glm::vec3(-0.4f, -1.0f, -0.6f)));

        // A lamp: a point light shines in every direction and fades out at its range.
        entt::entity lamp = scene.create("Lamp");
        fe::LightComponent lampLight;
        lampLight.type      = static_cast<fe::LightComponent::Type>(1);   // Point
        lampLight.intensity = 8.0f;
        lampLight.range     = 8.0f;
        scene.registry().emplace<fe::LightComponent>(lamp, lampLight);
        scene.transform(lamp).position = {2.5f, 1.2f, 1.5f};

        // A spotlight aimed straight down: forward is -Z, so rotate -Z onto -Y.
        entt::entity spot = scene.create("Spotlight");
        fe::LightComponent spotLight;
        spotLight.type      = static_cast<fe::LightComponent::Type>(2);   // Spot
        spotLight.intensity = 15.0f;
        spotLight.range     = 8.0f;
        scene.registry().emplace<fe::LightComponent>(spot, spotLight);
        scene.transform(spot).position = {-2.5f, 4.0f, 0.0f};
        scene.transform(spot).rotation = glm::rotation(
            glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f));

        scene.updateTransforms();
    }

    // Frame timing: declared before the panels because StatisticsPanel
    // keeps references to these variables.
    uint64_t frameCount = 0;
    double   lastTime   = 0.0;
    double   deltaTime  = 0.0;

    fe::EditorCamera editorCamera;

    fe::HierarchyPanel  hierarchyPanel {scene, selectedEntity};
    fe::InspectorPanel  inspectorPanel {scene, selectedEntity};
    fe::ViewportPanel   viewportPanel  {offscreen, editorCamera, scene, selectedEntity, sceneRenderer};
    fe::StatisticsPanel statisticsPanel{scene, renderer, deltaTime, frameCount};
    fe::AssetBrowser    assetBrowser   {assetsDir};

    // New / Open / Save. The starting scene counts as saved, so replacing it
    // right away does not ask for confirmation.
    fe::SceneFileController sceneFiles{scene, selectedEntity, assetsDir / "scenes"};
    sceneFiles.markClean();

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

        beginDockspace(running, sceneFiles);

        for (auto* panel : panels)
            panel->onImGui();

        sceneFiles.onImGui();   // shortcuts and the Open / Save / Discard dialogs

        ImGui::Render();

        // Panels may have created, deleted or edited entities this frame, so
        // refresh the world matrices before drawing.
        scene.updateTransforms();

        // Upload any model the scene uses for the first time. It has to happen
        // here, before recording the frame, because loading submits its own GPU work.
        sceneRenderer.prepare(scene);

        // ----- GPU frame -----
        if (!renderer.beginFrame()) continue;

        VkCommandBuffer cmd = renderer.currentCommandBuffer();

        // The camera is needed twice: the sun's shadow cascades follow what it
        // sees, and the scene pass is drawn through it.
        const float aspect = static_cast<float>(offscreen.width()) /
                             static_cast<float>(offscreen.height());
        const glm::mat4 cameraView       = editorCamera.view();
        const glm::mat4 cameraProjection = editorCamera.projection(aspect);

        // 1. Shadow pass: the scene seen from each light, depth only. It also
        //    uploads this frame's lights, so it comes before the scene pass.
        sceneRenderer.renderShadows(cmd, scene, renderer.currentFrameIndex(),
                                    cameraView, cameraProjection);

        // 2. Scene pass: draw the scene into the offscreen buffer.
        fe::Renderer::beginOffscreenPass(cmd, offscreen);

        const glm::mat4 viewProj = cameraProjection * cameraView;

        // The frame index picks this frame slot's copy of the scene's lights.
        sceneRenderer.render(cmd, scene, viewProj,
                             {offscreen.width(), offscreen.height()},
                             renderer.currentFrameIndex());

        fe::Renderer::endOffscreenPass(cmd, offscreen);

        // 3. Swapchain pass: ImGui on top (the Viewport panel samples the
        //    offscreen colour image).
        renderer.beginSwapchainPass();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        renderer.endSwapchainPass();

        renderer.endFrame();
    }

    FE_INFO("Shutting down");
    vkDeviceWaitIdle(vkCtx.device());

    sceneRenderer.shutdown();
    offscreen.shutdown();
    shutdownImGui(vkCtx.device());
    renderer.shutdown();
    vkCtx.shutdown();
    window.shutdown();

    return 0;
}
