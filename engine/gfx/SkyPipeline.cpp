#include "gfx/SkyPipeline.h"

#include "gfx/OffscreenBuffer.h"
#include "gfx/VulkanContext.h"
#include "core/Log.h"

#include <fstream>
#include <string>
#include <vector>

namespace fe {

VkShaderModule SkyPipeline::loadShader(const char* path) const {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        FE_ERROR("SkyPipeline: cannot open shader file '%s'", path);
        return VK_NULL_HANDLE;
    }
    const size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> code(size);
    file.seekg(0);
    file.read(code.data(), static_cast<std::streamsize>(size));

    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_ctx->device(), &info, nullptr, &module) != VK_SUCCESS) {
        FE_ERROR("SkyPipeline: failed to create shader module '%s'", path);
        return VK_NULL_HANDLE;
    }
    return module;
}

bool SkyPipeline::init(const VulkanContext& ctx, VkFormat colorFormat, VkFormat depthFormat,
                       const char* shaderDir, VkDescriptorSetLayout environmentLayout) {
    shutdown();
    m_ctx = &ctx;

    const std::string dir = shaderDir;
    VkShaderModule vert = loadShader((dir + "/sky.vert.spv").c_str());
    VkShaderModule frag = loadShader((dir + "/sky.frag.spv").c_str());
    if (!vert || !frag) {
        if (vert) vkDestroyShaderModule(ctx.device(), vert, nullptr);
        if (frag) vkDestroyShaderModule(ctx.device(), frag, nullptr);
        m_ctx = nullptr;
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

    // No vertex buffer: the triangle's corners come from the vertex index.
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasteriser{};
    rasteriser.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasteriser.polygonMode = VK_POLYGON_MODE_FILL;
    rasteriser.cullMode    = VK_CULL_MODE_NONE;
    rasteriser.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = OffscreenBuffer::kSampleCount;   // the scene pass is multisampled

    // The sky sits exactly on the far plane (depth 1) and must pass where the
    // buffer still holds its cleared value (1), hence "less or equal". It
    // writes no depth.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments    = &blendAttachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.size       = sizeof(SkyPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &environmentLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;
    if (vkCreatePipelineLayout(ctx.device(), &layoutInfo, nullptr, &m_layout) != VK_SUCCESS) {
        FE_ERROR("SkyPipeline: failed to create pipeline layout");
        vkDestroyShaderModule(ctx.device(), vert, nullptr);
        vkDestroyShaderModule(ctx.device(), frag, nullptr);
        shutdown();
        return false;
    }

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount    = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;
    renderingInfo.depthAttachmentFormat   = depthFormat;

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

    const VkResult result = vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1,
                                                      &pipelineInfo, nullptr, &m_pipeline);
    vkDestroyShaderModule(ctx.device(), vert, nullptr);
    vkDestroyShaderModule(ctx.device(), frag, nullptr);
    if (result != VK_SUCCESS) {
        FE_ERROR("SkyPipeline: failed to create graphics pipeline");
        shutdown();
        return false;
    }
    return true;
}

void SkyPipeline::shutdown() {
    if (!m_ctx) return;
    if (m_pipeline) vkDestroyPipeline(m_ctx->device(), m_pipeline, nullptr);
    if (m_layout)   vkDestroyPipelineLayout(m_ctx->device(), m_layout, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    m_layout   = VK_NULL_HANDLE;
    m_ctx      = nullptr;
}

void SkyPipeline::draw(VkCommandBuffer cmd, VkExtent2D extent, VkDescriptorSet environment,
                       const glm::mat4& viewProj, float brightness) const {
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

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &environment, 0, nullptr);

    const SkyPushConstants pc{glm::inverse(viewProj), glm::vec4(brightness, 0.0f, 0.0f, 0.0f)};
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace fe
