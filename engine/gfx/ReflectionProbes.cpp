#include "gfx/ReflectionProbes.h"

#include "gfx/VulkanContext.h"
#include "core/Log.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <fstream>
#include <string>

namespace fe {

namespace {

constexpr uint32_t kProbeLayers = kMaxReflectionProbes * ReflectionProbes::kFaces;

/// Push constants of probe_filter.frag.
struct FilterPush {
    int32_t face;
    float   roughness;
    int32_t sampleCount;
    float   sourceSize;
};

std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        FE_ERROR("ReflectionProbes: cannot open shader '%s'", path.c_str());
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
        FE_ERROR("ReflectionProbes: failed to create shader module '%s'", path.c_str());
        return VK_NULL_HANDLE;
    }
    return module;
}

} // namespace

// ---------------------------------------------------------------------------
// Faces
// ---------------------------------------------------------------------------

glm::mat4 ReflectionProbes::faceView(uint32_t face, const glm::vec3& position) {
    // Direction each face looks along, and which way is up in its picture. The
    // pictures are mirrored sideways when copied (captureFace), which is what
    // turns an ordinary camera into Vulkan's left-handed cube map faces.
    static const glm::vec3 forward[kFaces] = {
        { 1, 0, 0}, {-1, 0, 0}, {0,  1, 0}, {0, -1, 0}, {0, 0,  1}, {0, 0, -1}};
    static const glm::vec3 up[kFaces] = {
        {0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};

    face = std::min(face, kFaces - 1);
    return glm::lookAt(position, position + forward[face], up[face]);
}

glm::mat4 ReflectionProbes::faceProjection(float nearPlane, float farPlane) {
    glm::mat4 projection = glm::perspective(glm::radians(90.0f), 1.0f, nearPlane, farPlane);
    projection[1][1] *= -1.0f;   // Vulkan's Y points down, like the editor cameras
    return projection;
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

void ReflectionProbes::barrier(VkCommandBuffer cmd, VkImage image, uint32_t baseLevel, uint32_t levelCount,
                               uint32_t baseLayer, uint32_t layerCount, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{};
    b.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    b.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    b.oldLayout     = from;
    b.newLayout     = to;
    b.image         = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseLevel, levelCount, baseLayer, layerCount};

    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

bool ReflectionProbes::createImage(Image& out, uint32_t layers, VkImageUsageFlags usage, bool makeView) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = kFormat;
    imageInfo.extent        = {kResolution, kResolution, 1};
    imageInfo.mipLevels     = kLevels;
    imageInfo.arrayLayers   = layers;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = usage;
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &out.image, &out.allocation, nullptr) != VK_SUCCESS) {
        FE_ERROR("ReflectionProbes: failed to create a cube map");
        return false;
    }

    if (!makeView) return true;

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image            = out.image;
    viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format           = kFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kLevels, 0, layers};
    if (vkCreateImageView(m_ctx->device(), &viewInfo, nullptr, &out.view) != VK_SUCCESS) {
        FE_ERROR("ReflectionProbes: failed to create a cube map view");
        return false;
    }
    return true;
}

void ReflectionProbes::destroyImage(Image& image) {
    if (image.view != VK_NULL_HANDLE) vkDestroyImageView(m_ctx->device(), image.view, nullptr);
    if (image.image != VK_NULL_HANDLE) vmaDestroyImage(m_ctx->allocator(), image.image, image.allocation);
    image = Image{};
}

bool ReflectionProbes::clearProbes() {
    // Black everywhere, and readable, so a set bound before any bake is valid.
    // A command buffer of its own: this runs at start-up, not inside a frame.
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = m_ctx->graphicsQueueFamily();
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(m_ctx->device(), &poolInfo, nullptr, &pool) != VK_SUCCESS) return false;

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

        const VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, kLevels, 0, kProbeLayers};
        barrier(cmd, m_probes.image, 0, kLevels, 0, kProbeLayers,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cmd, m_probes.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        barrier(cmd, m_probes.image, 0, kLevels, 0, kProbeLayers,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit{};
        submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cmd;
        ok = vkQueueSubmit(m_ctx->graphicsQueue(), 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
        if (ok) vkQueueWaitIdle(m_ctx->graphicsQueue());
    }
    vkDestroyCommandPool(m_ctx->device(), pool, nullptr);
    return ok;
}

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------

bool ReflectionProbes::createPipeline(const char* shaderDir) {
    VkDevice device = m_ctx->device();

    VkPushConstantRange pushRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FilterPush)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &m_layout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        FE_ERROR("ReflectionProbes: failed to create the pipeline layout");
        return false;
    }

    VkShaderModule vert = makeShader(device, std::string(shaderDir) + "/fullscreen.vert.spv");
    VkShaderModule frag = makeShader(device, std::string(shaderDir) + "/probe_filter.frag.spv");
    if (!vert || !frag) {
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
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
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
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

    const VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    if (result != VK_SUCCESS) {
        FE_ERROR("ReflectionProbes: failed to create the filtering pipeline");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

bool ReflectionProbes::init(const VulkanContext& ctx, const char* shaderDir) {
    m_ctx = &ctx;
    VkDevice device = ctx.device();

    // Trilinear: the shader picks a level between two mips for in-between roughness.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter    = VK_FILTER_LINEAR;
    samplerInfo.minFilter    = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod       = static_cast<float>(kLevels);
    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        FE_ERROR("ReflectionProbes: failed to create the sampler");
        shutdown();
        return false;
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = kMaxReflectionProbes;   // the probes' cubes; the scratch set uses the first only
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings    = &binding;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_layout) != VK_SUCCESS) {
        FE_ERROR("ReflectionProbes: failed to create the descriptor set layout");
        shutdown();
        return false;
    }

    if (!createImage(m_probes, kProbeLayers,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT, false) ||
        !createImage(m_source, kFaces,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT, true)) {
        shutdown();
        return false;
    }

    // Each probe is seen as a cube map of its own: six layers of the image.
    for (uint32_t probe = 0; probe < kMaxReflectionProbes; ++probe) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image            = m_probes.image;
        viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_CUBE;
        viewInfo.format           = kFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kLevels, probe * kFaces, kFaces};
        if (vkCreateImageView(device, &viewInfo, nullptr, &m_cubeViews[probe]) != VK_SUCCESS) {
            FE_ERROR("ReflectionProbes: failed to create a cube view");
            shutdown();
            return false;
        }
    }

    // One 2D view per (probe, face, level): what the filtering pass draws into.
    m_targetViews.assign(static_cast<size_t>(kProbeLayers) * kLevels, VK_NULL_HANDLE);
    for (uint32_t layer = 0; layer < kProbeLayers; ++layer) {
        for (uint32_t level = 0; level < kLevels; ++level) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image            = m_probes.image;
            viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format           = kFormat;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, layer, 1};
            if (vkCreateImageView(device, &viewInfo, nullptr, &m_targetViews[layer * kLevels + level]) != VK_SUCCESS) {
                FE_ERROR("ReflectionProbes: failed to create a face view");
                shutdown();
                return false;
            }
        }
    }

    if (!createPipeline(shaderDir)) { shutdown(); return false; }

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxReflectionProbes + 8};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 2;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
        FE_ERROR("ReflectionProbes: failed to create the descriptor pool");
        shutdown();
        return false;
    }

    const VkDescriptorSetLayout layouts[2] = {m_layout, m_layout};
    VkDescriptorSet sets[2] = {};
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_pool;
    allocInfo.descriptorSetCount = 2;
    allocInfo.pSetLayouts        = layouts;
    if (vkAllocateDescriptorSets(device, &allocInfo, sets) != VK_SUCCESS) {
        FE_ERROR("ReflectionProbes: failed to allocate the descriptor sets");
        shutdown();
        return false;
    }
    m_probeSet  = sets[0];
    m_sourceSet = sets[1];

    VkDescriptorImageInfo probes[kMaxReflectionProbes];
    for (uint32_t i = 0; i < kMaxReflectionProbes; ++i)
        probes[i] = {m_sampler, m_cubeViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    // The scratch set only has its first sampler written; the rest are never read.
    // (The layout wants every descriptor of a set that is bound to be valid, so
    // they point at the same photograph.)
    VkDescriptorImageInfo source[kMaxReflectionProbes];
    for (uint32_t i = 0; i < kMaxReflectionProbes; ++i)
        source[i] = {m_sampler, m_source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    VkWriteDescriptorSet writes[2]{};
    for (VkWriteDescriptorSet& w : writes) {
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.descriptorCount = kMaxReflectionProbes;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    writes[0].dstSet = m_probeSet;  writes[0].pImageInfo = probes;
    writes[1].dstSet = m_sourceSet; writes[1].pImageInfo = source;
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    if (!clearProbes()) {
        FE_ERROR("ReflectionProbes: failed to clear the cube maps");
        shutdown();
        return false;
    }

    FE_INFO("ReflectionProbes: created (%u probes, %ux%u, %u levels)",
            kMaxReflectionProbes, kResolution, kResolution, kLevels);
    return true;
}

void ReflectionProbes::shutdown() {
    if (!m_ctx) return;
    VkDevice device = m_ctx->device();
    vkDeviceWaitIdle(device);

    for (VkImageView view : m_targetViews)
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
    m_targetViews.clear();

    for (VkImageView& view : m_cubeViews) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
        view = VK_NULL_HANDLE;
    }
    destroyImage(m_probes);
    destroyImage(m_source);
    if (m_pool)           vkDestroyDescriptorPool(device, m_pool, nullptr);   // frees the sets
    if (m_pipeline)       vkDestroyPipeline(device, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    if (m_layout)         vkDestroyDescriptorSetLayout(device, m_layout, nullptr);
    if (m_sampler)        vkDestroySampler(device, m_sampler, nullptr);

    m_pool = VK_NULL_HANDLE;
    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_probeSet = m_sourceSet = VK_NULL_HANDLE;
    m_ctx = nullptr;
}

// ---------------------------------------------------------------------------
// Making a probe
// ---------------------------------------------------------------------------

void ReflectionProbes::beginCapture(VkCommandBuffer cmd) {
    // Whatever the scratch cube map held is not needed any more.
    barrier(cmd, m_source.image, 0, kLevels, 0, kFaces,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

void ReflectionProbes::captureFace(VkCommandBuffer cmd, VkImage hdrImage, uint32_t face) {
    barrier(cmd, hdrImage, 0, 1, 0, 1,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    // Copied mirrored sideways: the destination's x runs from the right edge to the left.
    const int32_t size = static_cast<int32_t>(kResolution);
    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1]  = {size, size, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1};
    region.dstOffsets[0]  = {size, 0, 0};
    region.dstOffsets[1]  = {0, size, 1};
    vkCmdBlitImage(cmd, hdrImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   m_source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);

    barrier(cmd, hdrImage, 0, 1, 0, 1,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void ReflectionProbes::filter(VkCommandBuffer cmd, uint32_t probe) {
    if (probe >= kMaxReflectionProbes) return;

    // ---- The photograph's own mip chain: each level is the last at half size ----
    uint32_t size = kResolution;
    for (uint32_t level = 1; level < kLevels; ++level) {
        barrier(cmd, m_source.image, level - 1, 1, 0, kFaces,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        const int32_t from = static_cast<int32_t>(size);
        const int32_t to   = static_cast<int32_t>(std::max(1u, size / 2));
        VkImageBlit region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, kFaces};
        region.srcOffsets[1]  = {from, from, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, kFaces};
        region.dstOffsets[1]  = {to, to, 1};
        vkCmdBlitImage(cmd, m_source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       m_source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
        size = static_cast<uint32_t>(to);
    }
    barrier(cmd, m_source.image, 0, kLevels - 1, 0, kFaces,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(cmd, m_source.image, kLevels - 1, 1, 0, kFaces,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // ---- One pass per face and level, into the probe's own layers ----
    const uint32_t firstLayer = probe * kFaces;
    barrier(cmd, m_probes.image, 0, kLevels, firstLayer, kFaces,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_sourceSet, 0, nullptr);

    for (uint32_t level = 0; level < kLevels; ++level) {
        const uint32_t   extentSize = std::max(1u, kResolution >> level);
        const VkExtent2D extent{extentSize, extentSize};

        FilterPush push{};
        push.roughness   = static_cast<float>(level) / static_cast<float>(kLevels - 1);
        push.sampleCount = level == 0 ? 1 : 64;
        push.sourceSize  = static_cast<float>(kResolution);

        for (uint32_t face = 0; face < kFaces; ++face) {
            push.face = static_cast<int32_t>(face);

            VkRenderingAttachmentInfo color{};
            color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView   = m_targetViews[(firstLayer + face) * kLevels + level];
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // every texel is written
            color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo rendering{};
            rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea.extent    = extent;
            rendering.layerCount           = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments    = &color;
            vkCmdBeginRendering(cmd, &rendering);

            const VkViewport viewport{0.0f, 0.0f, float(extentSize), float(extentSize), 0.0f, 1.0f};
            const VkRect2D   scissor{{0, 0}, extent};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
        }
    }

    barrier(cmd, m_probes.image, 0, kLevels, firstLayer, kFaces,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

} // namespace fe
