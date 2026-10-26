#include "gfx/OutlinePass.h"

#include "gfx/OffscreenBuffer.h"
#include "gfx/VulkanContext.h"
#include "gfx/Vertex.h"
#include "core/Log.h"

#include <fstream>
#include <string>
#include <vector>

namespace fe {
namespace {

constexpr VkFormat kMaskFormat = VK_FORMAT_R8_UNORM;

/// Push constants of the outline shader. Mirrors `Params` in outline.frag.
struct OutlinePushConstants {
    glm::vec4 color;    ///< rgb: colour. a: strength.
    glm::vec2 texel;    ///< 1 / size of the mask in pixels.
    float     radius;   ///< Thickness in pixels.
    float     padding;
};
static_assert(sizeof(OutlinePushConstants) == 32, "Must match Params in outline.frag");

std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        FE_ERROR("OutlinePass: cannot open shader file '%s'", path.c_str());
        return {};
    }
    const size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> buf(size);
    file.seekg(0);
    file.read(buf.data(), static_cast<std::streamsize>(size));
    return buf;
}

} // namespace

OutlinePass::~OutlinePass() { shutdown(); }

VkShaderModule OutlinePass::loadShader(const char* path) const {
    const auto code = readFile(path);
    if (code.empty()) return VK_NULL_HANDLE;

    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_ctx->device(), &info, nullptr, &module) != VK_SUCCESS) {
        FE_ERROR("OutlinePass: failed to create shader module '%s'", path);
        return VK_NULL_HANDLE;
    }
    return module;
}

bool OutlinePass::init(const VulkanContext& ctx, VkFormat colorFormat, VkFormat depthFormat,
                       const char* shaderDir) {
    m_ctx = &ctx;

    // ---- The sampler that reads the mask ----
    // Linear, so that a pixel on the object's border reads as partly covered,
    // and clamped, so that looking past the edge of the screen finds nothing new.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter    = VK_FILTER_LINEAR;
    samplerInfo.minFilter    = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(ctx.device(), &samplerInfo, nullptr, &m_maskSampler) != VK_SUCCESS) {
        FE_ERROR("OutlinePass: failed to create the sampler");
        shutdown();
        return false;
    }

    // ---- Descriptor set: the mask, for the outline shader ----
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings    = &binding;
    if (vkCreateDescriptorSetLayout(ctx.device(), &layoutInfo, nullptr, &m_setLayout) != VK_SUCCESS) {
        FE_ERROR("OutlinePass: failed to create the descriptor set layout");
        shutdown();
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    if (vkCreateDescriptorPool(ctx.device(), &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
        FE_ERROR("OutlinePass: failed to create the descriptor pool");
        shutdown();
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_setLayout;
    if (vkAllocateDescriptorSets(ctx.device(), &allocInfo, &m_set) != VK_SUCCESS) {
        FE_ERROR("OutlinePass: failed to allocate the descriptor set");
        shutdown();
        return false;
    }

    if (!createMaskPipeline(shaderDir) ||
        !createOutlinePipeline(colorFormat, depthFormat, shaderDir)) {
        shutdown();
        return false;
    }

    FE_INFO("OutlinePass: created successfully");
    return true;
}

void OutlinePass::shutdown() {
    if (!m_ctx) return;
    VkDevice device = m_ctx->device();

    destroyMask();

    if (m_outlinePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, m_outlinePipeline, nullptr);
    if (m_outlineLayout   != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, m_outlineLayout, nullptr);
    if (m_maskPipeline    != VK_NULL_HANDLE) vkDestroyPipeline(device, m_maskPipeline, nullptr);
    if (m_maskLayout      != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, m_maskLayout, nullptr);
    if (m_pool            != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, m_pool, nullptr);
    if (m_setLayout       != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr);
    if (m_maskSampler     != VK_NULL_HANDLE) vkDestroySampler(device, m_maskSampler, nullptr);

    m_outlinePipeline = VK_NULL_HANDLE;
    m_outlineLayout   = VK_NULL_HANDLE;
    m_maskPipeline    = VK_NULL_HANDLE;
    m_maskLayout      = VK_NULL_HANDLE;
    m_pool            = VK_NULL_HANDLE;
    m_set             = VK_NULL_HANDLE;
    m_setLayout       = VK_NULL_HANDLE;
    m_maskSampler     = VK_NULL_HANDLE;
    m_ctx             = nullptr;
}

// ---------------------------------------------------------------------------
// The mask image
// ---------------------------------------------------------------------------

bool OutlinePass::createMask(VkExtent2D extent) {
    destroyMask();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = kMaskFormat;
    imageInfo.extent        = {extent.width, extent.height, 1};
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo,
                       &m_maskImage, &m_maskAllocation, nullptr) != VK_SUCCESS) {
        FE_ERROR("OutlinePass: failed to create the mask image");
        m_maskImage = VK_NULL_HANDLE;
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                       = m_maskImage;
    viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                      = kMaskFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(m_ctx->device(), &viewInfo, nullptr, &m_maskView) != VK_SUCCESS) {
        FE_ERROR("OutlinePass: failed to create the mask view");
        destroyMask();
        return false;
    }

    // Point the set at the new image. The old one was destroyed after the
    // device went idle, so nothing in flight still uses the set.
    VkDescriptorImageInfo descriptor{};
    descriptor.sampler     = m_maskSampler;
    descriptor.imageView   = m_maskView;
    descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_set;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &descriptor;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &write, 0, nullptr);

    m_maskExtent = extent;
    return true;
}

void OutlinePass::destroyMask() {
    if (!m_ctx) return;
    if (m_maskImage == VK_NULL_HANDLE && m_maskView == VK_NULL_HANDLE) return;

    // Frames still in flight may be reading the mask.
    vkDeviceWaitIdle(m_ctx->device());

    if (m_maskView != VK_NULL_HANDLE)
        vkDestroyImageView(m_ctx->device(), m_maskView, nullptr);
    if (m_maskImage != VK_NULL_HANDLE)
        vmaDestroyImage(m_ctx->allocator(), m_maskImage, m_maskAllocation);

    m_maskView       = VK_NULL_HANDLE;
    m_maskImage      = VK_NULL_HANDLE;
    m_maskAllocation = VK_NULL_HANDLE;
    m_maskExtent     = {0, 0};
}

// ---------------------------------------------------------------------------
// Pipelines
// ---------------------------------------------------------------------------

bool OutlinePass::createMaskPipeline(const char* shaderDir) {
    const std::string dir = shaderDir;
    // The vertex stage is the shadow pass's: it only moves the position.
    VkShaderModule vert = loadShader((dir + "/shadow.vert.spv").c_str());
    VkShaderModule frag = loadShader((dir + "/outline_mask.frag.spv").c_str());
    if (!vert || !frag) {
        if (vert) vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
        if (frag) vkDestroyShaderModule(m_ctx->device(), frag, nullptr);
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

    // Same vertex buffers as every other pass; only the position is read.
    const auto binding = Vertex::bindingDescription();
    const auto attrs   = Vertex::attributeDescriptions();

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &binding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions    = &attrs[0];

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    // No culling: the mask only needs to know what is covered, whichever way
    // the triangles face.
    VkPipelineRasterizationStateCreateInfo rasteriser{};
    rasteriser.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasteriser.polygonMode = VK_POLYGON_MODE_FILL;
    rasteriser.cullMode    = VK_CULL_MODE_NONE;
    rasteriser.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasteriser.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;   // no depth

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;

    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments    = &blendAttachment;

    const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.size       = sizeof(OutlineMaskPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;

    bool ok = vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_maskLayout) == VK_SUCCESS;
    if (ok) {
        const VkFormat maskFormat = kMaskFormat;
        VkPipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        renderingInfo.colorAttachmentCount    = 1;
        renderingInfo.pColorAttachmentFormats = &maskFormat;

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
        pipelineInfo.layout              = m_maskLayout;

        ok = vkCreateGraphicsPipelines(m_ctx->device(), VK_NULL_HANDLE, 1, &pipelineInfo,
                                       nullptr, &m_maskPipeline) == VK_SUCCESS;
    }

    vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
    vkDestroyShaderModule(m_ctx->device(), frag, nullptr);

    if (!ok) FE_ERROR("OutlinePass: failed to create the mask pipeline");
    return ok;
}

bool OutlinePass::createOutlinePipeline(VkFormat colorFormat, VkFormat depthFormat,
                                        const char* shaderDir) {
    const std::string dir = shaderDir;
    VkShaderModule vert = loadShader((dir + "/outline.vert.spv").c_str());
    VkShaderModule frag = loadShader((dir + "/outline.frag.spv").c_str());
    if (!vert || !frag) {
        if (vert) vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
        if (frag) vkDestroyShaderModule(m_ctx->device(), frag, nullptr);
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

    // No vertex buffers: the vertex shader builds the triangle from the vertex index.
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
    rasteriser.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasteriser.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = OffscreenBuffer::kSampleCount;   // drawn inside the (multisampled) scene pass

    // The scene pass has a depth attachment, so the pipeline must declare its
    // format, but the outline neither tests nor writes depth: it is drawn over
    // everything.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    // Ordinary alpha blending: the outline colour over what the scene drew.
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable         = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments    = &blendAttachment;

    const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.size       = sizeof(OutlinePushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &m_setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;

    bool ok = vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_outlineLayout) == VK_SUCCESS;
    if (ok) {
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
        pipelineInfo.layout              = m_outlineLayout;

        ok = vkCreateGraphicsPipelines(m_ctx->device(), VK_NULL_HANDLE, 1, &pipelineInfo,
                                       nullptr, &m_outlinePipeline) == VK_SUCCESS;
    }

    vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
    vkDestroyShaderModule(m_ctx->device(), frag, nullptr);

    if (!ok) FE_ERROR("OutlinePass: failed to create the outline pipeline");
    return ok;
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

bool OutlinePass::beginMask(VkCommandBuffer cmd, VkExtent2D extent) {
    if (!m_ctx || extent.width == 0 || extent.height == 0) return false;

    if (m_maskImage == VK_NULL_HANDLE ||
        extent.width != m_maskExtent.width || extent.height != m_maskExtent.height) {
        if (!createMask(extent)) return false;
    }

    // The previous contents are not needed, but the previous frame's read of
    // the mask must finish before this frame overwrites it.
    VkImageMemoryBarrier2 barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.srcAccessMask = 0;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.image         = m_maskImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);

    VkRenderingAttachmentInfo color{};
    color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView   = m_maskView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    VkRenderingInfo rendering{};
    rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent    = extent;
    rendering.layerCount           = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments    = &color;
    vkCmdBeginRendering(cmd, &rendering);

    VkViewport viewport{};
    viewport.width    = static_cast<float>(extent.width);
    viewport.height   = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_maskPipeline);
    return true;
}

void OutlinePass::pushMask(VkCommandBuffer cmd, const OutlineMaskPushConstants& pc) const {
    vkCmdPushConstants(cmd, m_maskLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(OutlineMaskPushConstants), &pc);
}

void OutlinePass::endMask(VkCommandBuffer cmd) {
    vkCmdEndRendering(cmd);

    VkImageMemoryBarrier2 barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.image         = m_maskImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void OutlinePass::draw(VkCommandBuffer cmd, VkExtent2D extent) const {
    VkViewport viewport{};
    viewport.width    = static_cast<float>(extent.width);
    viewport.height   = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_outlinePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_outlineLayout,
                            0, 1, &m_set, 0, nullptr);

    OutlinePushConstants pc{};
    pc.color  = glm::vec4(color, 1.0f);
    pc.texel  = glm::vec2(1.0f / static_cast<float>(m_maskExtent.width),
                          1.0f / static_cast<float>(m_maskExtent.height));
    pc.radius = thickness;
    vkCmdPushConstants(cmd, m_outlineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(OutlinePushConstants), &pc);

    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace fe
