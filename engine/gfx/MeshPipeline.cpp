#include "gfx/MeshPipeline.h"
#include "gfx/VulkanContext.h"
#include "gfx/Vertex.h"
#include "core/Log.h"

#include <fstream>
#include <vector>
#include <string>

namespace fe {
namespace {

std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        FE_ERROR("MeshPipeline: cannot open shader file '%s'", path.c_str());
        return {};
    }
    size_t size = static_cast<size_t>(file.tellg());
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
                        const char* shaderDir) {
    m_ctx = &ctx;

    // ---- Shader modules ----
    const std::string dir = shaderDir;
    VkShaderModule vert = loadShader((dir + "/mesh.vert.spv").c_str());
    VkShaderModule frag = loadShader((dir + "/mesh.frag.spv").c_str());

    if (!vert || !frag) {
        vkDestroyShaderModule(ctx.device(), vert, nullptr);
        vkDestroyShaderModule(ctx.device(), frag, nullptr);
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
    VkPipelineRasterizationStateCreateInfo rasteriser{};
    rasteriser.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasteriser.polygonMode = VK_POLYGON_MODE_FILL;
    rasteriser.cullMode    = VK_CULL_MODE_BACK_BIT;
    rasteriser.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasteriser.lineWidth   = 1.0f;

    // ---- Multisampling (off) ----
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // ---- Colour blending (opaque) ----
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments    = &blendAttachment;

    // ---- Dynamic state (viewport + scissor set at draw time) ----
    VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    // ---- Pipeline layout (push constants only) ----
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(MeshPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;

    if (vkCreatePipelineLayout(ctx.device(), &layoutInfo, nullptr,
                               &m_layout) != VK_SUCCESS) {
        FE_ERROR("MeshPipeline: failed to create pipeline layout");
        return false;
    }

    // ---- Dynamic rendering attachment info ----
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount    = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;

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
    pipelineInfo.pColorBlendState    = &blending;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = m_layout;

    if (vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1,
                                  &pipelineInfo, nullptr,
                                  &m_pipeline) != VK_SUCCESS) {
        FE_ERROR("MeshPipeline: failed to create graphics pipeline");
        return false;
    }

    vkDestroyShaderModule(ctx.device(), vert, nullptr);
    vkDestroyShaderModule(ctx.device(), frag, nullptr);

    FE_INFO("MeshPipeline: created successfully");
    return true;
}

void MeshPipeline::bind(VkCommandBuffer cmd,
                        const MeshPushConstants& pc) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(MeshPushConstants), &pc);
}

} // namespace fe
