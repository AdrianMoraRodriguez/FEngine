#include "gfx/PostProcess.h"

#include "gfx/OffscreenBuffer.h"
#include "gfx/VulkanContext.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

namespace fe {

/// Push constants of bloom_down.frag, bloom_up.frag and tonemap.frag (32 bytes,
/// fragment stage). Every pass uses the fields it needs.
struct PostProcess::PassPush {
    float   texelX;           ///< 1 / width of the image being read (down, up).
    float   texelY;           ///< 1 / height of the image being read.
    float   threshold;        ///< Down (first): brightness from which light glows.
    float   knee;             ///< Down (first): width of the soft transition below the threshold.
    float   bloomIntensity;   ///< Composite: strength of the glow, already divided by the number of levels.
    float   exposure;         ///< Composite: brightness multiplier (2^EV).
    int32_t toneMapper;       ///< Composite: a ToneMapper.
    int32_t flags;            ///< Down: 1 = first level. Composite: 1 = add the glow.
};
static_assert(sizeof(PostProcess::PassPush) == 32, "Must match the Push block in the post-processing shaders");

namespace {

constexpr VkFormat kBloomFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        FE_ERROR("PostProcess: cannot open shader file '%s'", path.c_str());
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
        FE_ERROR("PostProcess: failed to create shader module '%s'", path.c_str());
        return VK_NULL_HANDLE;
    }
    return module;
}

} // namespace

// ---------------------------------------------------------------------------
// Barrier
// ---------------------------------------------------------------------------

void PostProcess::transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    // Everything before waits for everything after: simple, and the cost is
    // nothing next to the passes themselves. It also covers the previous
    // frame's reads of the image.
    VkImageMemoryBarrier2 barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    barrier.oldLayout     = from;
    barrier.newLayout     = to;
    barrier.image         = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
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

bool PostProcess::createPipeline(VkPipeline& out, const char* shaderDir, const char* fragName,
                                 VkFormat colorFormat, bool additive) {
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

    // The upsample adds its result to what the image already holds.
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                   | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (additive) {
        blendAttachment.blendEnable         = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments    = &blendAttachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

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
        FE_ERROR("PostProcess: failed to create the pipeline for '%s'", fragName);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

bool PostProcess::init(const VulkanContext& ctx, const char* shaderDir) {
    m_ctx = &ctx;
    VkDevice device = ctx.device();

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter    = VK_FILTER_LINEAR;
    samplerInfo.minFilter    = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_linear) != VK_SUCCESS) {
        FE_ERROR("PostProcess: failed to create the sampler");
        shutdown();
        return false;
    }

    // One image per set. Defined exactly like the set of ImGui_ImplVulkan_AddTexture
    // (and so of OffscreenBuffer::hdrDescriptorSet()), which makes the two compatible.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings    = &binding;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_setLayout) != VK_SUCCESS) {
        FE_ERROR("PostProcess: failed to create the descriptor set layout");
        shutdown();
        return false;
    }

    // Set 0 is the image being read; set 1 is a second one (the glow, for the final pass).
    const VkDescriptorSetLayout setLayouts[2] = {m_setLayout, m_setLayout};
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.size       = sizeof(PassPush);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount         = 2;
    pipelineLayoutInfo.pSetLayouts            = setLayouts;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges    = &pushRange;
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        FE_ERROR("PostProcess: failed to create the pipeline layout");
        shutdown();
        return false;
    }

    if (!createPipeline(m_downPipeline,      shaderDir, "bloom_down.frag.spv", kBloomFormat,                false) ||
        !createPipeline(m_upPipeline,        shaderDir, "bloom_up.frag.spv",   kBloomFormat,                true)  ||
        !createPipeline(m_compositePipeline, shaderDir, "tonemap.frag.spv",    OffscreenBuffer::format(),   false)) {
        shutdown();
        return false;
    }

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxLevels};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = kMaxLevels;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
        FE_ERROR("PostProcess: failed to create the descriptor pool");
        shutdown();
        return false;
    }

    std::array<VkDescriptorSetLayout, kMaxLevels> layouts;
    layouts.fill(m_setLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_pool;
    allocInfo.descriptorSetCount = kMaxLevels;
    allocInfo.pSetLayouts        = layouts.data();
    if (vkAllocateDescriptorSets(device, &allocInfo, m_levelSets.data()) != VK_SUCCESS) {
        FE_ERROR("PostProcess: failed to allocate the descriptor sets");
        shutdown();
        return false;
    }
    return true;
}

void PostProcess::shutdown() {
    if (!m_ctx) return;
    VkDevice device = m_ctx->device();
    vkDeviceWaitIdle(device);

    destroyBloomImages();
    if (m_downPipeline      != VK_NULL_HANDLE) vkDestroyPipeline(device, m_downPipeline, nullptr);
    if (m_upPipeline        != VK_NULL_HANDLE) vkDestroyPipeline(device, m_upPipeline, nullptr);
    if (m_compositePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, m_compositePipeline, nullptr);
    if (m_pipelineLayout    != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    if (m_pool              != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, m_pool, nullptr);   // frees the sets
    if (m_setLayout         != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr);
    if (m_linear            != VK_NULL_HANDLE) vkDestroySampler(device, m_linear, nullptr);

    m_downPipeline = m_upPipeline = m_compositePipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_pool           = VK_NULL_HANDLE;
    m_setLayout      = VK_NULL_HANDLE;
    m_linear         = VK_NULL_HANDLE;
    m_levelSets.fill(VK_NULL_HANDLE);
    m_ctx = nullptr;
}

// ---------------------------------------------------------------------------
// Bloom images
// ---------------------------------------------------------------------------

void PostProcess::destroyBloomImages() {
    for (Image& level : m_levels) {
        if (level.view != VK_NULL_HANDLE)
            vkDestroyImageView(m_ctx->device(), level.view, nullptr);
        if (level.image != VK_NULL_HANDLE)
            vmaDestroyImage(m_ctx->allocator(), level.image, level.allocation);
        level = Image{};
    }
    m_levelCount  = 0;
    m_bloomWidth  = 0;
    m_bloomHeight = 0;
}

uint32_t PostProcess::ensureBloomImages(uint32_t width, uint32_t height) {
    if (m_levelCount > 0 && width == m_bloomWidth && height == m_bloomHeight) return m_levelCount;

    // The old images may still be in use by frames in flight.
    vkDeviceWaitIdle(m_ctx->device());
    destroyBloomImages();

    // Each level is half the size of the one before; stop before it gets smaller
    // than a few pixels, where it would only blur the whole picture to one colour.
    uint32_t levels = 0;
    for (uint32_t i = 0; i < kMaxLevels; ++i) {
        if ((width >> (i + 1)) < 2 || (height >> (i + 1)) < 2) break;
        ++levels;
    }
    if (levels == 0) return 0;

    for (uint32_t i = 0; i < levels; ++i) {
        Image& level = m_levels[i];
        level.extent = {width >> (i + 1), height >> (i + 1)};

        VkImageCreateInfo imageInfo{};
        imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageInfo.format        = kBloomFormat;
        imageInfo.extent        = {level.extent.width, level.extent.height, 1};
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

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                      = kBloomFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo,
                           &level.image, &level.allocation, nullptr) != VK_SUCCESS) {
            FE_ERROR("PostProcess: failed to create a bloom image (%ux%u)",
                     level.extent.width, level.extent.height);
            destroyBloomImages();
            return 0;
        }
        viewInfo.image = level.image;
        if (vkCreateImageView(m_ctx->device(), &viewInfo, nullptr, &level.view) != VK_SUCCESS) {
            FE_ERROR("PostProcess: failed to create a bloom image view");
            destroyBloomImages();
            return 0;
        }

        VkDescriptorImageInfo descriptor{};
        descriptor.sampler     = m_linear;
        descriptor.imageView   = level.view;
        descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_levelSets[i];
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &descriptor;
        vkUpdateDescriptorSets(m_ctx->device(), 1, &write, 0, nullptr);
    }

    m_levelCount  = levels;
    m_bloomWidth  = width;
    m_bloomHeight = height;
    return levels;
}

// ---------------------------------------------------------------------------
// Passes
// ---------------------------------------------------------------------------

void PostProcess::drawPass(VkCommandBuffer cmd, VkPipeline pipeline, VkImageView dstView, VkExtent2D dstExtent,
                           bool loadDst, VkDescriptorSet source, VkDescriptorSet extra, const PassPush& push) {
    VkRenderingAttachmentInfo colour{};
    colour.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colour.imageView   = dstView;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp      = loadDst ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colour.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering{};
    rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent    = dstExtent;
    rendering.layerCount           = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments    = &colour;
    vkCmdBeginRendering(cmd, &rendering);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    VkViewport viewport{0.0f, 0.0f, static_cast<float>(dstExtent.width), static_cast<float>(dstExtent.height), 0.0f, 1.0f};
    VkRect2D   scissor{{0, 0}, dstExtent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Set 1 must be valid even for passes that do not read it.
    const VkDescriptorSet sets[2] = {source, extra ? extra : source};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PassPush), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    // The caller ends the rendering (the final pass draws more into it).
}

void PostProcess::renderBloom(VkCommandBuffer cmd, VkDescriptorSet hdrSet, VkExtent2D hdrExtent,
                              const PostSettings& settings) {
    // Every image is written before it is read, so its old contents do not matter.
    for (uint32_t i = 0; i < m_levelCount; ++i)
        transition(cmd, m_levels[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // ---- Down: HDR image -> level 0 -> level 1 -> ... ----
    for (uint32_t i = 0; i < m_levelCount; ++i) {
        const VkExtent2D source = (i == 0) ? hdrExtent : m_levels[i - 1].extent;

        PassPush push{};
        push.texelX    = 1.0f / static_cast<float>(source.width);
        push.texelY    = 1.0f / static_cast<float>(source.height);
        push.threshold = std::max(settings.bloomThreshold, 0.0f);
        push.knee      = push.threshold * 0.5f;
        push.flags     = (i == 0) ? 1 : 0;

        drawPass(cmd, m_downPipeline, m_levels[i].view, m_levels[i].extent, false,
                 (i == 0) ? hdrSet : m_levelSets[i - 1], VK_NULL_HANDLE, push);
        vkCmdEndRendering(cmd);
        transition(cmd, m_levels[i].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // ---- Up: level N-1 is added into level N-2, and so on up to level 0 ----
    for (int i = static_cast<int>(m_levelCount) - 2; i >= 0; --i) {
        const VkExtent2D source = m_levels[i + 1].extent;

        PassPush push{};
        push.texelX = 1.0f / static_cast<float>(source.width);
        push.texelY = 1.0f / static_cast<float>(source.height);

        transition(cmd, m_levels[i].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        drawPass(cmd, m_upPipeline, m_levels[i].view, m_levels[i].extent, true,
                 m_levelSets[i + 1], VK_NULL_HANDLE, push);
        vkCmdEndRendering(cmd);
        transition(cmd, m_levels[i].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

void PostProcess::render(VkCommandBuffer cmd, const OffscreenBuffer& target, const PostSettings& settings,
                         const std::function<void(VkCommandBuffer)>& drawOver) {
    const VkExtent2D extent{target.width(), target.height()};

    uint32_t levels = 0;
    if (settings.bloom && settings.bloomIntensity > 0.0f)
        levels = ensureBloomImages(target.width(), target.height());
    if (levels > 0) renderBloom(cmd, target.hdrDescriptorSet(), extent, settings);

    PassPush push{};
    push.exposure   = std::exp2(settings.exposure);
    push.toneMapper = static_cast<int32_t>(settings.toneMapper);
    if (levels > 0) {
        // The glow is the sum of all levels; dividing by their number makes an
        // intensity of 1 add as much light as the glowing parts already have.
        push.bloomIntensity = settings.bloomIntensity / static_cast<float>(levels);
        push.flags          = 1;
    }

    transition(cmd, target.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    drawPass(cmd, m_compositePipeline, target.imageView(), extent, false,
             target.hdrDescriptorSet(), levels > 0 ? m_levelSets[0] : VK_NULL_HANDLE, push);
    if (drawOver) drawOver(cmd);
    vkCmdEndRendering(cmd);
    transition(cmd, target.image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

} // namespace fe
