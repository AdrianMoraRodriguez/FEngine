/**
 * @file main.cpp
 * @brief Entry point of the FEngine editor.
 */

#include "core/AssetPaths.h"
#include "core/Log.h"
#include "core/Version.h"
#include "platform/Window.h"
#include "gfx/VulkanContext.h"
#include "gfx/Renderer.h"
#include "gfx/OffscreenBuffer.h"
#include "gfx/SceneRenderer.h"
#include "core/ProjectSettings.h"
#include "editor/ProjectSettingsPanel.h"
#include "import/AssetDatabase.h"
#include "gfx/ThumbnailRenderer.h"
#include "scene/Scene.h"
#include "scene/ComponentRegistry.h"
#include "scene/SceneSerializer.h"
#include "editor/AssetBrowser.h"
#include "editor/ImageThumbnails.h"
#include "import/ImageLoader.h"
#include "editor/ImageViewerPanel.h"
#include "editor/ModelViewerPanel.h"
#include "editor/MeshDistanceFieldPanel.h"
#include "editor/FileDrop.h"
#include "editor/EditorTheme.h"
#include "editor/EditorCamera.h"
#include "editor/SceneFileController.h"
#include "editor/HierarchyPanel.h"
#include "editor/InspectorPanel.h"
#include "editor/UndoHistory.h"
#include "editor/ViewportPanel.h"
#include "editor/StatisticsPanel.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <glm/gtx/quaternion.hpp>
#include <exception>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

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
        { VK_DESCRIPTOR_TYPE_SAMPLER,                1   },
        // One per texture ImGui shows: its fonts, the viewport and the asset
        // thumbnails (see ThumbnailRenderer::kMaxThumbnails).
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          1   },
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = 512;
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

    // Look of the editor: Inter + Lucide icons, FEngine colours and metrics.
    // Both live in EditorTheme; apply() replaces every colour of the style,
    // so there is no need for ImGui::StyleColorsDark() first.
    fe::theme::loadFonts();
    fe::theme::apply();

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

void beginDockspace(fe::SceneFileController& sceneFiles, fe::UndoHistory& undo,
                    fe::MeshDistanceFieldPanel& distanceFieldPanel,
                    fe::ProjectSettingsPanel& projectSettingsPanel,
                    fe::SceneRenderer& sceneRenderer) {
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
            if (ImGui::MenuItem("Exit", "Alt+F4")) sceneFiles.requestQuit();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            undo.drawMenuItems();
            ImGui::Separator();
            ImGui::MenuItem("Project Settings", nullptr, &projectSettingsPanel.visible);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Mesh Distance Field", nullptr, &distanceFieldPanel.visible);
            ImGui::MenuItem("Scene Distance Field", nullptr, &sceneRenderer.showDistanceField);
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
    fe::FileDrop::install(window.handle());

    // Window icon (title bar, task bar, Alt+Tab). The PNGs are copied next to
    // the executable by the build; if they are missing the default icon stays.
    {
        std::vector<fe::ImageData> images;
        for (const int size : {16, 32, 48, 64, 128, 256}) {
            const std::filesystem::path file =
                exeDir / "branding" / ("fractalis-icon-" + std::to_string(size) + ".png");
            if (std::optional<fe::ImageData> image = fe::loadImage(file))
                images.push_back(std::move(*image));
        }
        std::vector<GLFWimage> icons;
        for (fe::ImageData& image : images)
            icons.push_back({static_cast<int>(image.width), static_cast<int>(image.height),
                             image.pixels.data()});
        if (!icons.empty())
            glfwSetWindowIcon(window.handle(), static_cast<int>(icons.size()), icons.data());
    }

    fe::VulkanContext vkCtx;
    if (!vkCtx.init(window)) return 1;

    fe::Renderer renderer;
    if (!renderer.init(vkCtx, window)) return 1;

    if (!initImGui(vkCtx, window, renderer)) return 1;

    fe::OffscreenBuffer offscreen;
    if (!offscreen.init(vkCtx, 1280, 720)) return 1;

    // The project's quality settings: read before the renderer, which sizes the
    // meshes' distance field atlas from them. They live next to the assets.
    const std::filesystem::path projectSettingsFile = std::filesystem::current_path() / "project_settings.json";
    fe::ProjectSettings projectSettings;
    projectSettings.load(projectSettingsFile);

    fe::SceneRenderer sceneRenderer;
    if (!sceneRenderer.init(vkCtx, shaderDir.string().c_str(), &projectSettings)) return 1;
    fe::assets::setRoot(assetsDir);
    fe::AssetDatabase::instance().scan();   // reads / creates the .meta files

    fe::registerBuiltinComponents();
    fe::ComponentRegistry::instance().add<fe::ReflectionProbeComponent>("ReflectionProbe");

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
    fe::ImageViewerPanel imageViewer   {vkCtx};
    fe::ModelViewerPanel modelViewer   {vkCtx, sceneRenderer};
    fe::MeshDistanceFieldPanel distanceFieldPanel{scene, selectedEntity, sceneRenderer};
    fe::ProjectSettingsPanel   projectSettingsPanel{projectSettings, projectSettingsFile};

    // Small pictures of the models shown in the Asset Browser, drawn on demand.
    fe::ThumbnailRenderer thumbnails;
    thumbnails.init(vkCtx, sceneRenderer);
    assetBrowser.setThumbnails(&thumbnails);
    assetBrowser.setScene(&scene);   // the Delete dialog warns about assets the scene uses

    // Pictures of the image files, for their tiles.
    fe::ImageThumbnails imageThumbnails;
    imageThumbnails.init(vkCtx);
    assetBrowser.setImageThumbnails(&imageThumbnails);

    // New / Open / Save. The starting scene counts as saved, so replacing it
    // right away does not ask for confirmation.
    fe::SceneFileController sceneFiles{scene, selectedEntity, assetsDir / "scenes"};
    sceneFiles.markClean();

    // Undo and redo. It starts from the scene as it is now, and starts over
    // whenever New or Open replaces the scene.
    fe::UndoHistory undoHistory{scene, selectedEntity};
    sceneFiles.setSceneReplacedCallback([&undoHistory] { undoHistory.reset(); });

    fe::Panel* panels[] = {
        &hierarchyPanel,
        &inspectorPanel,
        &viewportPanel,
        &statisticsPanel,
        &assetBrowser,
        &imageViewer,
        &modelViewer,
        &distanceFieldPanel,
        &projectSettingsPanel,
    };

    // Mouse capture while the camera navigates (right, middle or Alt+left drag).
    // The OS cursor is hidden and locked, so it stays where the drag began and
    // can never reach (and highlight) other panels; GLFW still reports how far
    // the mouse moves, and that movement is what turns the camera. When the
    // button is released GLFW puts the cursor back where it was.
    bool   mouseCaptured = false;
    double captureAnchorX = 0.0, captureAnchorY = 0.0;   // where the cursor was when the capture began
    double captureLastX   = 0.0, captureLastY   = 0.0;   // the locked cursor's last reported position

    // The editor closes once the scene files controller says nothing would be
    // lost (see requestQuit()).
    while (!sceneFiles.quitConfirmed()) {
        const double now = glfwGetTime();
        deltaTime = now - lastTime;
        lastTime  = now;
        ++frameCount;

        window.pollEvents();

        // Files dropped from the OS file explorer go to the Asset Browser.
        {
            std::vector<std::filesystem::path> dropped = fe::FileDrop::take();
            if (!dropped.empty()) {
                // The window may not have the focus (the drag came from
                // another application), in which case ImGui has not heard
                // where the cursor is. Tell it, so the Asset Browser can
                // tell which folder the files were dropped on.
                double x = 0.0, y = 0.0;
                glfwGetCursorPos(window.handle(), &x, &y);
                ImGui::GetIO().AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
                assetBrowser.onFilesDropped(std::move(dropped));
            }
        }

        // Start or end the mouse capture. The camera decides during the ImGui
        // frame whether a drag is in progress, so this follows it one frame later.
        float captureDeltaX = 0.0f, captureDeltaY = 0.0f;
        {
            const bool wantCapture = editorCamera.isNavigating();
            if (wantCapture && !mouseCaptured) {
                glfwGetCursorPos(window.handle(), &captureAnchorX, &captureAnchorY);
                glfwSetInputMode(window.handle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                // In this mode GLFW reports a virtual, unbounded position that
                // starts where the cursor was.
                glfwGetCursorPos(window.handle(), &captureLastX, &captureLastY);
                mouseCaptured = true;
            } else if (!wantCapture && mouseCaptured) {
                glfwSetInputMode(window.handle(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                mouseCaptured = false;
            }

            if (mouseCaptured) {
                double x = 0.0, y = 0.0;
                glfwGetCursorPos(window.handle(), &x, &y);
                captureDeltaX = static_cast<float>(x - captureLastX);
                captureDeltaY = static_cast<float>(y - captureLastY);
                captureLastX = x;
                captureLastY = y;
                // For ImGui the pointer has not moved: panels see it parked at
                // the spot where the drag began.
                ImGui::GetIO().AddMousePosEvent(static_cast<float>(captureAnchorX),
                                                static_cast<float>(captureAnchorY));
            }
        }

        // The window's close button, or Alt+F4: do not let the window close
        // by itself. Ask the controller, which may show the Unsaved Changes
        // dialog first, and keep running until it confirms.
        if (window.shouldClose()) {
            glfwSetWindowShouldClose(window.handle(), GLFW_FALSE);
            sceneFiles.requestQuit();
        }

        // ----- ImGui frame -----
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // While captured, the camera is driven by the movement GLFW reported.
        if (mouseCaptured)
            ImGui::GetIO().MouseDelta = ImVec2(captureDeltaX, captureDeltaY);

        beginDockspace(sceneFiles, undoHistory, distanceFieldPanel, projectSettingsPanel, sceneRenderer);

        for (auto* panel : panels)
            panel->onImGui();

        // A scene double-clicked in the Asset Browser opens like File > Open.
        {
            const std::filesystem::path scenePath = assetBrowser.takeSceneToOpen();
            if (!scenePath.empty()) sceneFiles.openScene(scenePath);
        }

        // An image double-clicked in the Asset Browser opens in a viewer tab.
        {
            const std::filesystem::path imagePath = assetBrowser.takeImageToOpen();
            if (!imagePath.empty()) imageViewer.open(imagePath);
        }

        // Same for a model: it opens in a viewer tab with its own camera.
        {
            const std::filesystem::path modelPath = assetBrowser.takeModelToOpen();
            if (!modelPath.empty()) modelViewer.open(modelPath);
        }

        sceneFiles.onImGui();   // shortcuts and the Open / Save / Discard dialogs

        // Last, after every panel has had its say: records what the user just
        // did and handles Ctrl+Z / Ctrl+Y.
        undoHistory.update();

        ImGui::Render();

        // Panels may have created, deleted or edited entities this frame, so
        // refresh the world matrices before drawing.
        scene.updateTransforms();

        // Upload any model the scene uses for the first time. It has to happen
        // here, before recording the frame, because loading submits its own GPU work.
        sceneRenderer.prepare(scene);

        // Same reason for the thumbnails: finished models are uploaded here.
        thumbnails.update();
        imageThumbnails.update();
        modelViewer.update();      // finishes loading the models open in viewers

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

        // 1b. Reflection probes: photographs the scene for the ones that need it
        //     (it opens passes of its own) and tells the shaders which are ready.
        sceneRenderer.renderProbes(cmd, scene, renderer.currentFrameIndex());

        const glm::mat4 viewProj = cameraProjection * cameraView;
        const VkExtent2D extent{offscreen.width(), offscreen.height()};

        // 2. Ambient occlusion: the scene's depth from the camera, and from it
        //    how closed each pixel is. The scene pass reads the result.
        sceneRenderer.renderAmbientOcclusion(cmd, scene, viewProj, cameraProjection, extent);

        // 2b. The shape of the selected entity, for its outline. Like the
        //    shadow pass, it opens passes of its own, so it comes first.
        sceneRenderer.renderSelectionMask(cmd, scene, selectedEntity, viewProj, extent);

        // 3. Scene pass: draw the scene into the offscreen buffer.
        fe::Renderer::beginOffscreenPass(cmd, offscreen);

        // The frame index picks this frame slot's copy of the scene's lights.
        sceneRenderer.render(cmd, scene, viewProj, extent, renderer.currentFrameIndex());

        fe::Renderer::endOffscreenPass(cmd, offscreen);

        // 3a. Post-processing: the HDR scene becomes the finished picture
        //     (bloom, exposure, tone mapping), with the outline of the
        //     selected entity drawn over it.
        sceneRenderer.renderPostProcess(cmd, offscreen, scene);

        // 3b. Thumbnails of the models the Asset Browser asked for. Each one
        //     is its own small pass; the pictures appear from the next frame.
        thumbnails.render(cmd);

        // 3c. The models open in viewer tabs, each with its own camera.
        modelViewer.render(cmd);

        // 4. Swapchain pass: ImGui on top (the Viewport panel samples the
        //    offscreen colour image).
        renderer.beginSwapchainPass();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        renderer.endSwapchainPass();

        renderer.endFrame();
    }

    FE_INFO("Shutting down");

    // Say which step is slow, if one is: a shutdown that seems to hang is
    // otherwise impossible to tell apart from one that is just waiting.
    double lapStart = glfwGetTime();
    auto lap = [&lapStart](const char* what) {
        const double now = glfwGetTime();
        if (now - lapStart > 0.25)
            FE_WARN("Shutdown: %s took %.2f s", what, now - lapStart);
        lapStart = now;
    };

    vkDeviceWaitIdle(vkCtx.device());
    lap("waiting for the GPU");

    modelViewer.shutdown();  // its pictures are ImGui textures too
    imageThumbnails.shutdown();
    imageViewer.shutdown();  // same: its pictures are ImGui textures
    thumbnails.shutdown();   // its pictures are ImGui textures: before ImGui goes away
    lap("thumbnails");
    sceneRenderer.shutdown();
    lap("scene renderer (models and imports)");
    offscreen.shutdown();
    shutdownImGui(vkCtx.device());
    lap("ImGui");
    renderer.shutdown();
    vkCtx.shutdown();
    lap("Vulkan");
    window.shutdown();

    // An import that would not stop is still running on its own thread.
    // Returning from main would run the static destructors underneath it, so
    // leave right away: everything that needed an orderly shutdown has had it.
    if (sceneRenderer.meshes().importsStillRunning()) {
        std::fflush(nullptr);
        std::quick_exit(0);
    }

    return 0;
}
