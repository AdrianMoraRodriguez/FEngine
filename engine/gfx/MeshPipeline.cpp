#include "gfx/MeshPipeline.h"

#include "gfx/OffscreenBuffer.h"
#include "gfx/VulkanContext.h"
#include "gfx/Vertex.h"
#include "core/Log.h"

#include <fstream>
#include <string>
#include <vector>

namespace fe {
namespace {

std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        FE_ERROR("MeshPipeline: cannot open shader file '%s'", path.c_str());
        return {};
    }
    const size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> buf(size);
    file.seekg(0);
    file.read(buf.data(), static_cast<std::streamsize>(size));
    return buf;
}

} // namespace

MeshPipeline::~MeshPipeline() { shutdown(); }

void MeshPipeline::shutdown() {
    if (!m_ctx) return;
    VkDevice dev = m_ctx->device();

    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
    m_ctx = nullptr;
}

VkShaderModule MeshPipeline::loadShader(const char* path) const {
    auto code = readFile(path);
    if (code.empty()) return VK_NULL_HANDLE;

    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_ctx->device(), &info, nullptr, &mod) != VK_SUCCESS) {
        FE_ERROR("MeshPipeline: failed to create shader module '%s'", path);
        return VK_NULL_HANDLE;
    }
    return mod;
}

bool MeshPipeline::init(const VulkanContext& ctx,
                        VkFormat colorFormat,
                        VkFormat depthFormat,
                        const char* shaderDir,
                        VkDescriptorSetLayout sceneLayout,
                        VkDescriptorSetLayout materialLayout,
                        VkDescriptorSetLayout environmentLayout,
                        VkDescriptorSetLayout occlusionLayout,
                        bool translucent) {
    m_ctx = &ctx;

    // ---- Shader modules ----
    const std::string dir = shaderDir;
    VkShaderModule vert = loadShader((dir + "/mesh.vert.spv").c_str());
    VkShaderModule frag = loadShader((dir + "/mesh.frag.spv").c_str());

    if (!vert || !frag) {
        if (vert) vkDestroyShaderModule(ctx.device(), vert, nullptr);
        if (frag) vkDestroyShaderModule(ctx.device(), frag, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";

    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    // ---- Vertex input ----
    auto binding = Vertex::bindingDescription();
    auto attrs   = Vertex::attributeDescriptions();

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vertexInput.pVertexAttributeDescriptions    = attrs.data();

    // ---- Input assembly ----
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // ---- Viewport and scissor (dynamic) ----
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    // ---- Rasteriser ----
    // Front faces are counter-clockwise as seen on screen. This only holds
    // because the projection matrix flips Y (see EditorCamera::projection).
    VkPipelineRasterizationStateCreateInfo rasteriser{};
    rasteriser.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasteriser.polygonMode = VK_POLYGON_MODE_FILL;
    rasteriser.cullMode    = VK_CULL_MODE_BACK_BIT;   // dynamic: materials can be double-sided
    rasteriser.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasteriser.lineWidth   = 1.0f;

    // ---- Multisampling (off) ----
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = OffscreenBuffer::kSampleCount;   // the scene pass is multisampled

    // ---- Depth test ----
    // Depth range is [0, 1] (GLM_FORCE_DEPTH_ZERO_TO_ONE); nearer fragments
    // have smaller values, so LESS keeps the closest surface.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = translucent ? VK_FALSE : VK_TRUE;   // translucent surfaces do not hide what is behind them
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

    // ---- Colour blending (opaque) ----
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (!translucent) {
        // Cut-out edges are smoothed by alpha to coverage, which reads the
        // alpha the shader writes. That alpha must not end up in the picture
        // (the viewport would show the window through the edges), so only the
        // colour is written: the alpha stays at the clear value, 1.
        multisampling.alphaToCoverageEnable = VK_TRUE;
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                       | VK_COLOR_COMPONENT_B_BIT;
    }
    if (translucent) {
        // The shader outputs colour already multiplied by alpha (so that the
        // highlights, which glass keeps, can be left out of the product):
        // result = source + destination * (1 - alpha).
        blendAttachment.blendEnable         = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments    = &blendAttachment;

    // ---- Dynamic state ----
    VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_CULL_MODE,
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 3;
    dynamicState.pDynamicStates    = dynamicStates;

    // ---- Pipeline layout: set 0 (scene data), set 1 (material), set 2 (environment), set 3 (occlusion) + push constants ----
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(MeshPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    const VkDescriptorSetLayout setLayouts[] = {sceneLayout, materialLayout, environmentLayout, occlusionLayout};
    layoutInfo.setLayoutCount         = 4;
    layoutInfo.pSetLayouts            = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;

    if (vkCreatePipelineLayout(ctx.device(), &layoutInfo, nullptr,
                               &m_layout) != VK_SUCCESS) {
        FE_ERROR("MeshPipeline: failed to create pipeline layout");
        vkDestroyShaderModule(ctx.device(), vert, nullptr);
        vkDestroyShaderModule(ctx.device(), frag, nullptr);
        return false;
    }

    // ---- Dynamic rendering attachment formats ----
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount    = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;
    renderingInfo.depthAttachmentFormat   = depthFormat;

    // ---- Create the pipeline ----
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext               = &renderingInfo;
    pipelineInfo.stageCount          = 2;
    pipelineInfo.pStages             = stages;
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasteriser;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &blending;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = m_layout;

    const VkResult result = vkCreateGraphicsPipelines(
        ctx.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);

    // Shader modules are no longer needed once the pipeline exists.
    vkDestroyShaderModule(ctx.device(), vert, nullptr);
    vkDestroyShaderModule(ctx.device(), frag, nullptr);

    if (result != VK_SUCCESS) {
        FE_ERROR("MeshPipeline: failed to create graphics pipeline");
        return false;
    }

    FE_INFO("MeshPipeline: created successfully");
    return true;
}

void MeshPipeline::bind(VkCommandBuffer cmd, VkExtent2D extent) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    VkViewport viewport{};
    viewport.width    = static_cast<float>(extent.width);
    viewport.height   = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdSetCullMode(cmd, VK_CULL_MODE_BACK_BIT);
}

void MeshPipeline::bindDescriptorSet(VkCommandBuffer cmd, VkDescriptorSet sceneSet) const {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout,
                            0,               // first set number: set 0
                            1, &sceneSet,
                            0, nullptr);     // no dynamic offsets
}

void MeshPipeline::bindMaterial(VkCommandBuffer cmd, VkDescriptorSet materialSet) const {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout,
                            1,               // first set number: set 1
                            1, &materialSet,
                            0, nullptr);
}

void MeshPipeline::setDoubleSided(VkCommandBuffer cmd, bool doubleSided) const {
    vkCmdSetCullMode(cmd, doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
}

void MeshPipeline::bindEnvironment(VkCommandBuffer cmd, VkDescriptorSet environmentSet) const {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout,
                            2,               // first set number: set 2
                            1, &environmentSet,
                            0, nullptr);
}

void MeshPipeline::bindOcclusion(VkCommandBuffer cmd, VkDescriptorSet occlusionSet) const {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout,
                            3,               // first set number: set 3
                            1, &occlusionSet,
                            0, nullptr);
}

void MeshPipeline::push(VkCommandBuffer cmd, const MeshPushConstants& pc) const {
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(MeshPushConstants), &pc);
}

} // namespace fe
