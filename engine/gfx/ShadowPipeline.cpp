#include "gfx/ShadowPipeline.h"

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
        FE_ERROR("ShadowPipeline: cannot open shader file '%s'", path.c_str());
        return {};
    }
    const size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> buf(size);
    file.seekg(0);
    file.read(buf.data(), static_cast<std::streamsize>(size));
    return buf;
}

} // namespace

ShadowPipeline::~ShadowPipeline() { shutdown(); }

void ShadowPipeline::shutdown() {
    if (!m_ctx) return;

    if (m_pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(m_ctx->device(), m_pipeline, nullptr);
    if (m_maskedPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(m_ctx->device(), m_maskedPipeline, nullptr);
    if (m_layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(m_ctx->device(), m_layout, nullptr);
    if (m_maskedLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(m_ctx->device(), m_maskedLayout, nullptr);

    m_pipeline = m_maskedPipeline = VK_NULL_HANDLE;
    m_layout = m_maskedLayout = VK_NULL_HANDLE;
    m_ctx      = nullptr;
}

bool ShadowPipeline::createPipeline(VkFormat depthFormat, const char* shaderDir,
                                    const char* vertName, const char* fragName,
                                    VkPipelineLayout layout, bool withUv, VkPipeline& out) {
    const VulkanContext& ctx = *m_ctx;

    // ---- Shaders ----
    const auto vertCode = readFile(std::string(shaderDir) + "/" + vertName);
    if (vertCode.empty()) return false;
    std::vector<char> fragCode;
    if (fragName) {
        fragCode = readFile(std::string(shaderDir) + "/" + fragName);
        if (fragCode.empty()) return false;
    }

    const auto makeModule = [&](const std::vector<char>& code) {
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = code.size();
        moduleInfo.pCode    = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(ctx.device(), &moduleInfo, nullptr, &module) != VK_SUCCESS)
            FE_ERROR("ShadowPipeline: failed to create the shader module");
        return module;
    };

    VkShaderModule vert = makeModule(vertCode);
    VkShaderModule frag = fragName ? makeModule(fragCode) : VK_NULL_HANDLE;
    const auto cleanup = [&] {
        if (vert) vkDestroyShaderModule(ctx.device(), vert, nullptr);
        if (frag) vkDestroyShaderModule(ctx.device(), frag, nullptr);
    };
    if (!vert || (fragName && !frag)) { cleanup(); return false; }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    uint32_t stageCount = 1;
    if (frag) {
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName  = "main";
        stageCount = 2;
    }

    // ---- Vertex input: the same buffers as the main pass, but only the position
    //      (and the texture coordinates, for the cut-out variant) are read ----
    const auto binding = Vertex::bindingDescription();
    const auto attrs   = Vertex::attributeDescriptions();   // [0] position, [3] uv
    VkVertexInputAttributeDescription used[2] = {attrs[0], attrs[3]};
    used[1].location = 1;                                    // shadow_mask.vert's location

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &binding;
    vertexInput.vertexAttributeDescriptionCount = withUv ? 2 : 1;
    vertexInput.pVertexAttributeDescriptions    = used;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    // ---- Rasteriser: no culling, plus a depth bias ----
    // The bias pushes the stored depths slightly away from the light, more on
    // surfaces that are steep as seen from it. Without it a surface compares
    // against its own (slightly different, because of the texel grid) depth
    // and shadows itself in stripes.
    VkPipelineRasterizationStateCreateInfo rasteriser{};
    rasteriser.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasteriser.polygonMode             = VK_POLYGON_MODE_FILL;
    rasteriser.cullMode                = VK_CULL_MODE_NONE;
    rasteriser.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasteriser.depthBiasEnable         = VK_TRUE;
    rasteriser.depthBiasConstantFactor = 1.0f;
    rasteriser.depthBiasSlopeFactor    = 1.75f;
    rasteriser.lineWidth               = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

    // No colour attachments, so no blend attachments either.
    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

    const VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    // ---- Dynamic rendering: depth only ----
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType                 = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.depthAttachmentFormat = depthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext               = &renderingInfo;
    pipelineInfo.stageCount          = stageCount;
    pipelineInfo.pStages             = stages;
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasteriser;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &blending;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = layout;

    const VkResult result = vkCreateGraphicsPipelines(
        ctx.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &out);
    cleanup();

    if (result != VK_SUCCESS) {
        FE_ERROR("ShadowPipeline: failed to create the graphics pipeline");
        return false;
    }
    return true;
}

bool ShadowPipeline::init(const VulkanContext& ctx, VkFormat depthFormat,
                          const char* shaderDir, VkDescriptorSetLayout materialLayout) {
    m_ctx = &ctx;

    // ---- Layouts: one push constant block; the cut-out variant adds the material set ----
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.size       = sizeof(ShadowPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;
    if (vkCreatePipelineLayout(ctx.device(), &layoutInfo, nullptr, &m_layout) != VK_SUCCESS) {
        FE_ERROR("ShadowPipeline: failed to create the pipeline layout");
        shutdown();
        return false;
    }

    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts    = &materialLayout;
    if (vkCreatePipelineLayout(ctx.device(), &layoutInfo, nullptr, &m_maskedLayout) != VK_SUCCESS) {
        FE_ERROR("ShadowPipeline: failed to create the cut-out pipeline layout");
        shutdown();
        return false;
    }

    if (!createPipeline(depthFormat, shaderDir, "shadow.vert.spv", nullptr,
                        m_layout, false, m_pipeline) ||
        !createPipeline(depthFormat, shaderDir, "shadow_mask.vert.spv", "shadow_mask.frag.spv",
                        m_maskedLayout, true, m_maskedPipeline)) {
        shutdown();
        return false;
    }

    FE_INFO("ShadowPipeline: created successfully");
    return true;
}

void ShadowPipeline::bind(VkCommandBuffer cmd) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    m_maskedBound = false;
}

void ShadowPipeline::bindMasked(VkCommandBuffer cmd, VkDescriptorSet materialSet) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_maskedPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_maskedLayout,
                            0, 1, &materialSet, 0, nullptr);
    m_maskedBound = true;
}

void ShadowPipeline::push(VkCommandBuffer cmd, const ShadowPushConstants& pc) const {
    vkCmdPushConstants(cmd, m_maskedBound ? m_maskedLayout : m_layout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(ShadowPushConstants), &pc);
}

} // namespace fe
