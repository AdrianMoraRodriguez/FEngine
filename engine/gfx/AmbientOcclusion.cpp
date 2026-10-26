#include "gfx/AmbientOcclusion.h"

#include "gfx/VulkanContext.h"
#include "core/Log.h"

#include <fstream>
#include <string>
#include <vector>

namespace fe {

/// Push constants of ssao.frag and ssao_blur.frag (80 bytes, fragment stage).
struct PassPush {
    glm::mat4 projection;
    glm::vec4 params;     ///< SSAO: x radius, y intensity. Blur: xy step between taps in uv units.
};
static_assert(sizeof(PassPush) == 80, "Must match the Push block in ssao.frag / ssao_blur.frag");

namespace {

std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        FE_ERROR("AmbientOcclusion: cannot open shader file '%s'", path.c_str());
        return {};
    }
    const size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> buf(size);
    file.seekg(0);
    file.read(buf.data(), static_cast<std::streamsize>(size));
    return buf;
}

VkShaderModule makeShader(VkDevice device, const std::string& path) {
    const auto code = readFile(path);
    if (code.empty()) return VK_NULL_HANDLE;

    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        FE_ERROR("AmbientOcclusion: failed to create shader module '%s'", path.c_str());
        return VK_NULL_HANDLE;
    }
    return module;
}

} // namespace

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

bool AmbientOcclusion::createImage(Image& out, VkFormat format, VkExtent2D extent,
                                   VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = format;
    imageInfo.extent        = {extent.width, extent.height, 1};
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = usage;
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo,
                       &out.image, &out.allocation, nullptr) != VK_SUCCESS) {
        FE_ERROR("AmbientOcclusion: failed to create an image (%ux%u)", extent.width, extent.height);
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                       = out.image;
    viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                      = format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_ctx->device(), &viewInfo, nullptr, &out.view) != VK_SUCCESS) {
        FE_ERROR("AmbientOcclusion: failed to create an image view");
        return false;
    }
    return true;
}

void AmbientOcclusion::destroyImage(Image& image) {
    if (image.view != VK_NULL_HANDLE)
        vkDestroyImageView(m_ctx->device(), image.view, nullptr);
    if (image.image != VK_NULL_HANDLE)
        vmaDestroyImage(m_ctx->allocator(), image.image, image.allocation);
    image = Image{};
}

void AmbientOcclusion::destroySizedImages() {
    destroyImage(m_depth);
    destroyImage(m_a);
    destroyImage(m_b);
}

bool AmbientOcclusion::createWhiteImage() {
    if (!createImage(m_white, kFormat, {1, 1},
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT))
        return false;

    // Fill it with 1 and leave it readable by shaders, once, with a command
    // buffer of its own: this runs at start-up, not in the middle of a frame.
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = m_ctx->graphicsQueueFamily();
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(m_ctx->device(), &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        FE_ERROR("AmbientOcclusion: failed to create a command pool");
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = pool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool ok = vkAllocateCommandBuffers(m_ctx->device(), &allocInfo, &cmd) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        transition(cmd, m_white.image, VK_IMAGE_ASPECT_COLOR_BIT,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkClearColorValue one{};
        one.float32[0] = one.float32[1] = one.float32[2] = one.float32[3] = 1.0f;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, m_white.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &range);

        transition(cmd, m_white.image, VK_IMAGE_ASPECT_COLOR_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit{};
        submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cmd;
        ok = vkQueueSubmit(m_ctx->graphicsQueue(), 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
        if (ok) vkQueueWaitIdle(m_ctx->graphicsQueue());
    }
    vkDestroyCommandPool(m_ctx->device(), pool, nullptr);   // frees cmd too

    if (!ok) FE_ERROR("AmbientOcclusion: failed to fill the white image");
    return ok;
}

void AmbientOcclusion::transition(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                                  VkImageLayout from, VkImageLayout to) {
    // Everything before waits for... everything after: simple, and the cost is
    // nothing next to the passes themselves. It also covers the previous
    // frame's reads of the image (the old contents are never needed).
    VkImageMemoryBarrier2 barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    barrier.oldLayout     = from;
    barrier.newLayout     = to;
    barrier.image         = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

// ---------------------------------------------------------------------------
// Pipelines
// ---------------------------------------------------------------------------

bool AmbientOcclusion::createPipeline(VkPipeline& out, const char* shaderDir, const char* fragName) {
    VkShaderModule vert = makeShader(m_ctx->device(), std::string(shaderDir) + "/fullscreen.vert.spv");
    VkShaderModule frag = makeShader(m_ctx->device(), std::string(shaderDir) + "/" + fragName);
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

    VkPipelineVertexInputStateCreateInfo vertexInput{};   // the triangle is made from the vertex index
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
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};   // no depth attachment: nothing to test
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                   | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments    = &blendAttachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    const VkFormat colorFormat = kFormat;
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount    = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;

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
    pipelineInfo.layout              = m_pipelineLayout;

    const VkResult result = vkCreateGraphicsPipelines(m_ctx->device(), VK_NULL_HANDLE, 1,
                                                      &pipelineInfo, nullptr, &out);
    vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
    vkDestroyShaderModule(m_ctx->device(), frag, nullptr);

    if (result != VK_SUCCESS) {
        FE_ERROR("AmbientOcclusion: failed to create the pipeline for '%s'", fragName);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

bool AmbientOcclusion::init(const VulkanContext& ctx, const char* shaderDir) {
    m_ctx = &ctx;
    VkDevice device = ctx.device();

    // ---- Samplers ----
    // The depth is read exactly (nearest): depths must not be blended. The
    // occlusion is read with linear filtering, so that it is smooth if the
    // image ever has another size than the target.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod       = 0.0f;

    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_nearest) != VK_SUCCESS) {
        FE_ERROR("AmbientOcclusion: failed to create the depth sampler");
        shutdown();
        return false;
    }
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_linear) != VK_SUCCESS) {
        FE_ERROR("AmbientOcclusion: failed to create the sampler");
        shutdown();
        return false;
    }

    // ---- Descriptor set layouts ----
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_passLayout) != VK_SUCCESS) {
        FE_ERROR("AmbientOcclusion: failed to create the pass layout");
        shutdown();
        return false;
    }
    layoutInfo.bindingCount = 1;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_resultLayout) != VK_SUCCESS) {
        FE_ERROR("AmbientOcclusion: failed to create the result layout");
        shutdown();
        return false;
    }

    // ---- Pipelines (both passes share the layout) ----
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.size       = sizeof(PassPush);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount         = 1;
    pipelineLayoutInfo.pSetLayouts            = &m_passLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges    = &pushRange;
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        FE_ERROR("AmbientOcclusion: failed to create the pipeline layout");
        shutdown();
        return false;
    }
    if (!createPipeline(m_ssaoPipeline, shaderDir, "ssao.frag.spv") ||
        !createPipeline(m_blurPipeline, shaderDir, "ssao_blur.frag.spv")) {
        shutdown();
        return false;
    }

    // ---- Descriptor sets: 3 pass sets + 2 result sets = 8 samplers ----
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 5;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
        FE_ERROR("AmbientOcclusion: failed to create the descriptor pool");
        shutdown();
        return false;
    }

    const VkDescriptorSetLayout layouts[5] = {m_passLayout, m_passLayout, m_passLayout,
                                              m_resultLayout, m_resultLayout};
    VkDescriptorSet sets[5] = {};
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_pool;
    allocInfo.descriptorSetCount = 5;
    allocInfo.pSetLayouts        = layouts;
    if (vkAllocateDescriptorSets(device, &allocInfo, sets) != VK_SUCCESS) {
        FE_ERROR("AmbientOcclusion: failed to allocate the descriptor sets");
        shutdown();
        return false;
    }
    m_ssaoSet = sets[0]; m_blurHSet = sets[1]; m_blurVSet = sets[2];
    m_resultSet = sets[3]; m_whiteSet = sets[4];

    if (!createWhiteImage()) { shutdown(); return false; }

    // The white set never changes. The others are written by resize().
    VkDescriptorImageInfo white{m_linear, m_white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_whiteSet;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &white;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    FE_INFO("AmbientOcclusion: created");
    return true;
}

void AmbientOcclusion::shutdown() {
    if (!m_ctx) return;
    VkDevice device = m_ctx->device();

    destroySizedImages();
    destroyImage(m_white);
    if (m_pool)           vkDestroyDescriptorPool(device, m_pool, nullptr);
    if (m_ssaoPipeline)   vkDestroyPipeline(device, m_ssaoPipeline, nullptr);
    if (m_blurPipeline)   vkDestroyPipeline(device, m_blurPipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    if (m_passLayout)     vkDestroyDescriptorSetLayout(device, m_passLayout, nullptr);
    if (m_resultLayout)   vkDestroyDescriptorSetLayout(device, m_resultLayout, nullptr);
    if (m_nearest)        vkDestroySampler(device, m_nearest, nullptr);
    if (m_linear)         vkDestroySampler(device, m_linear, nullptr);

    m_pool = VK_NULL_HANDLE;
    m_ssaoPipeline = m_blurPipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_passLayout = m_resultLayout = VK_NULL_HANDLE;
    m_nearest = m_linear = VK_NULL_HANDLE;
    m_ssaoSet = m_blurHSet = m_blurVSet = m_resultSet = m_whiteSet = VK_NULL_HANDLE;
    m_extent = {0, 0};
    m_ctx = nullptr;
}

bool AmbientOcclusion::resize(VkExtent2D extent) {
    if (!m_ctx || extent.width == 0 || extent.height == 0) return false;
    if (extent.width == m_extent.width && extent.height == m_extent.height && m_depth.image)
        return true;

    // The old images may still be read by frames in flight.
    vkDeviceWaitIdle(m_ctx->device());
    destroySizedImages();
    m_extent = {0, 0};

    const bool ok =
        createImage(m_depth, kDepthFormat, extent,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT) &&
        createImage(m_a, kFormat, extent,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT) &&
        createImage(m_b, kFormat, extent,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
    if (!ok) { destroySizedImages(); return false; }

    m_extent = extent;
    writeSets();
    return true;
}

void AmbientOcclusion::writeSets() {
    const VkDescriptorImageInfo white {m_linear,  m_white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo depth {m_nearest, m_depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo a     {m_linear,  m_a.view,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo b     {m_linear,  m_b.view,     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    VkWriteDescriptorSet writes[6]{};
    const struct { VkDescriptorSet set; uint32_t binding; const VkDescriptorImageInfo* info; } targets[6] = {
        {m_ssaoSet,   0, &white}, {m_ssaoSet,  1, &depth},
        {m_blurHSet,  0, &a},     {m_blurHSet, 1, &depth},
        {m_blurVSet,  0, &b},     {m_blurVSet, 1, &depth},
    };
    for (int i = 0; i < 6; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = targets[i].set;
        writes[i].dstBinding      = targets[i].binding;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo      = targets[i].info;
    }
    vkUpdateDescriptorSets(m_ctx->device(), 6, writes, 0, nullptr);

    VkWriteDescriptorSet result{};
    result.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    result.dstSet          = m_resultSet;
    result.descriptorCount = 1;
    result.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    result.pImageInfo      = &a;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &result, 0, nullptr);
}

// ---------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------

void AmbientOcclusion::beginDepth(VkCommandBuffer cmd) {
    transition(cmd, m_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo depth{};
    depth.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView   = m_depth.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{};
    rendering.sType             = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = m_extent;
    rendering.layerCount        = 1;
    rendering.pDepthAttachment  = &depth;
    vkCmdBeginRendering(cmd, &rendering);

    VkViewport viewport{0.0f, 0.0f, float(m_extent.width), float(m_extent.height), 0.0f, 1.0f};
    VkRect2D   scissor{{0, 0}, m_extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void AmbientOcclusion::endDepth(VkCommandBuffer cmd) {
    vkCmdEndRendering(cmd);
    transition(cmd, m_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT,
               VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
}

void AmbientOcclusion::fullscreenPass(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet set,
                                      const Image& target, const PassPush& push) {
    transition(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo color{};
    color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView   = target.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // every pixel is written
    color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering{};
    rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent    = m_extent;
    rendering.layerCount           = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments    = &color;
    vkCmdBeginRendering(cmd, &rendering);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkViewport viewport{0.0f, 0.0f, float(m_extent.width), float(m_extent.height), 0.0f, 1.0f};
    VkRect2D   scissor{{0, 0}, m_extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PassPush), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);
    transition(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void AmbientOcclusion::compute(VkCommandBuffer cmd, const glm::mat4& projection,
                               float radius, float intensity) {
    const glm::vec2 texel(1.0f / float(m_extent.width), 1.0f / float(m_extent.height));

    // 1. The raw occlusion, from the depth, into A.
    fullscreenPass(cmd, m_ssaoPipeline, m_ssaoSet, m_a,
                   PassPush{projection, glm::vec4(radius, intensity, 0.0f, 0.0f)});
    // 2. Blur across, A -> B.  3. Blur down, B -> A. The result ends in A.
    fullscreenPass(cmd, m_blurPipeline, m_blurHSet, m_b,
                   PassPush{projection, glm::vec4(texel.x, 0.0f, 0.0f, 0.0f)});
    fullscreenPass(cmd, m_blurPipeline, m_blurVSet, m_a,
                   PassPush{projection, glm::vec4(0.0f, texel.y, 0.0f, 0.0f)});
}

} // namespace fe
