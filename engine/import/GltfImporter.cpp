#include "import/GltfImporter.h"

#include "core/Log.h"

// cgltf is a single-file library: exactly one translation unit defines the
// implementation, every other include only sees the declarations.
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <glm/gtc/type_ptr.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_map>

namespace fe {
namespace {

/// Colour used for primitives that have no material at all.
const glm::vec3 kDefaultColor{0.8f};

/// State shared by all the primitives of one file while importing.
struct ImportContext {
    std::filesystem::path                     directory;   ///< Folder of the model file; image paths are relative to it.
    std::unordered_map<const cgltf_image*, int> imageIndex;   ///< Image -> index in MeshData::images (-1: unusable).
    std::unordered_map<const cgltf_material*, int> materialIndex;   ///< Material -> index in MeshData::materials.
};

/// Registers the image of @p view in @p out once and returns its index.
/// @return The index in MeshData::images, or -1 if the material has no usable texture.
int imageFor(const cgltf_texture_view& view, ImportContext& context, MeshData& out) {
    const cgltf_image* image = view.texture ? view.texture->image : nullptr;
    if (!image) return -1;

    if (const auto known = context.imageIndex.find(image); known != context.imageIndex.end())
        return known->second;

    ImageSource source;
    source.label = image->name ? image->name : (image->uri ? image->uri : "embedded image");

    if (image->buffer_view) {
        // Inside the file (typical for .glb): the bytes of a PNG or JPEG.
        const uint8_t* data = cgltf_buffer_view_data(image->buffer_view);
        if (data && image->buffer_view->size > 0)
            source.encoded = std::make_shared<const std::vector<uint8_t>>(
                data, data + image->buffer_view->size);
    } else if (image->uri) {
        std::string uri = image->uri;

        if (uri.rfind("data:", 0) == 0) {
            // The image is written in the .gltf itself as base64 text.
            const size_t comma = uri.find(',');
            if (comma != std::string::npos) {
                const char*  base64  = uri.c_str() + comma + 1;
                const size_t length  = std::strlen(base64);
                size_t       padding = 0;
                while (padding < length && padding < 2 && base64[length - 1 - padding] == '=') ++padding;

                void* raw = nullptr;
                const cgltf_size size = length / 4 * 3 - padding;
                cgltf_options options{};
                if (size > 0 && cgltf_load_buffer_base64(&options, size, base64, &raw) == cgltf_result_success) {
                    const uint8_t* bytes = static_cast<const uint8_t*>(raw);
                    source.encoded = std::make_shared<const std::vector<uint8_t>>(bytes, bytes + size);
                    std::free(raw);
                }
            }
        } else {
            // A file next to the model. URIs escape characters as %20 and so on.
            cgltf_decode_uri(uri.data());
            uri.resize(std::strlen(uri.c_str()));
            source.file = context.directory / std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(uri.data()), uri.size()));
        }
    }

    int index = -1;
    if (source.encoded || !source.file.empty()) {
        index = static_cast<int>(out.images.size());
        out.images.push_back(std::move(source));
    }
    context.imageIndex[image] = index;
    return index;
}

/// Registers the material once and returns its index in MeshData::materials.
/// @return The index, or -1 for a primitive without a material.
int materialFor(const cgltf_material* material, ImportContext& context, MeshData& out) {
    if (!material) return -1;

    if (const auto known = context.materialIndex.find(material); known != context.materialIndex.end())
        return known->second;

    ModelMaterial result;

    if (material->has_pbr_metallic_roughness) {
        const auto& pbr = material->pbr_metallic_roughness;
        result.albedo     = imageFor(pbr.base_color_texture, context, out);
        result.metalRough = imageFor(pbr.metallic_roughness_texture, context, out);
        result.metallic   = pbr.metallic_factor;
        result.roughness  = pbr.roughness_factor;
    }

    switch (material->alpha_mode) {
        case cgltf_alpha_mode_mask:  result.alphaMode = AlphaMode::Mask;  break;
        case cgltf_alpha_mode_blend: result.alphaMode = AlphaMode::Blend; break;
        default:                     result.alphaMode = AlphaMode::Opaque; break;
    }
    result.alphaCutoff = material->alpha_cutoff;
    result.doubleSided = material->double_sided != 0;
    if (material->has_pbr_metallic_roughness)
        result.opacity = material->pbr_metallic_roughness.base_color_factor[3];

    result.normal      = imageFor(material->normal_texture, context, out);
    result.normalScale = material->normal_texture.scale;
    result.emissive    = imageFor(material->emissive_texture, context, out);
    result.emissiveFactor = glm::vec3(material->emissive_factor[0], material->emissive_factor[1],
                                      material->emissive_factor[2]);
    if (material->has_emissive_strength)
        result.emissiveFactor *= material->emissive_strength.emissive_strength;

    const int index = static_cast<int>(out.materials.size());
    out.materials.push_back(result);
    context.materialIndex[material] = index;
    return index;
}

/// Reads an accessor into a flat array of floats, converting from whatever
/// the file stores (normalised integers, for example).
std::vector<float> readFloats(const cgltf_accessor* accessor) {
    const cgltf_size floatCount = accessor->count * cgltf_num_components(accessor->type);

    std::vector<float> out(floatCount);
    if (floatCount > 0)
        cgltf_accessor_unpack_floats(accessor, out.data(), floatCount);
    return out;
}

/// Appends one primitive, transformed by @p world, to @p out.
/// @return `false` if the primitive was skipped.
bool appendPrimitive(const cgltf_primitive& primitive,
                     const glm::mat4& world, ImportContext& context, MeshData& out) {
    if (primitive.type != cgltf_primitive_type_triangles) {
        FE_WARN("GltfImporter: skipped a primitive that is not a plain triangle list");
        return false;
    }

    // Find the accessors for the attributes we use. Only the first set of
    // each (index 0) is read.
    const cgltf_accessor* positionAccessor = nullptr;
    const cgltf_accessor* normalAccessor   = nullptr;
    const cgltf_accessor* colorAccessor    = nullptr;
    const cgltf_accessor* uvAccessor       = nullptr;

    for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
        const cgltf_attribute& attribute = primitive.attributes[i];
        if (attribute.index != 0) continue;

        if (attribute.type == cgltf_attribute_type_position) positionAccessor = attribute.data;
        if (attribute.type == cgltf_attribute_type_normal)   normalAccessor   = attribute.data;
        if (attribute.type == cgltf_attribute_type_color)    colorAccessor    = attribute.data;
        if (attribute.type == cgltf_attribute_type_texcoord) uvAccessor       = attribute.data;
    }

    if (!positionAccessor || positionAccessor->count == 0) return false;

    const size_t vertexCount = positionAccessor->count;

    // ---- Positions ----
    const std::vector<float> positionData = readFloats(positionAccessor);
    std::vector<glm::vec3> positions(vertexCount);
    for (size_t v = 0; v < vertexCount; ++v)
        positions[v] = glm::vec3(positionData[3 * v], positionData[3 * v + 1], positionData[3 * v + 2]);

    // ---- Indices ----
    std::vector<uint32_t> indices;
    if (primitive.indices) {
        indices.resize(primitive.indices->count);
        for (size_t i = 0; i < indices.size(); ++i)
            indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(primitive.indices, i));
    } else {
        // Non-indexed geometry: every three vertices form a triangle.
        indices.resize(vertexCount);
        std::iota(indices.begin(), indices.end(), 0u);
    }

    indices.resize(indices.size() - indices.size() % 3);   // drop an incomplete triangle

    for (uint32_t index : indices) {
        if (index >= vertexCount) {
            FE_WARN("GltfImporter: skipped a primitive with an out-of-range index");
            return false;
        }
    }

    // ---- Texture coordinates ----
    // glTF already uses the image-file convention (V down from the top), and
    // cgltf converts normalised integer storage to floats for us.
    std::vector<float> uvData;
    if (uvAccessor && uvAccessor->count == vertexCount)
        uvData = readFloats(uvAccessor);

    // ---- Normals ----
    std::vector<glm::vec3> normals(vertexCount, glm::vec3(0.0f));

    if (normalAccessor && normalAccessor->count == vertexCount) {
        const std::vector<float> normalData = readFloats(normalAccessor);
        for (size_t v = 0; v < vertexCount; ++v)
            normals[v] = glm::vec3(normalData[3 * v], normalData[3 * v + 1], normalData[3 * v + 2]);
    } else {
        // No normals in the file: accumulate each triangle's normal into its
        // vertices. The unnormalised cross product is proportional to the
        // triangle's area, so larger triangles weigh more, which is the
        // usual choice for smooth shading.
        for (size_t t = 0; t < indices.size(); t += 3) {
            const glm::vec3& p0 = positions[indices[t]];
            const glm::vec3& p1 = positions[indices[t + 1]];
            const glm::vec3& p2 = positions[indices[t + 2]];

            const glm::vec3 faceNormal = glm::cross(p1 - p0, p2 - p0);
            normals[indices[t]]     += faceNormal;
            normals[indices[t + 1]] += faceNormal;
            normals[indices[t + 2]] += faceNormal;
        }
    }

    // ---- Colour ----
    // The material's colour factor is baked into the vertex colours; the
    // texture is multiplied on top of it by the shader.
    glm::vec3 baseColor = kDefaultColor;
    if (primitive.material && primitive.material->has_pbr_metallic_roughness) {
        const auto& pbr = primitive.material->pbr_metallic_roughness;
        baseColor = glm::vec3(pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2]);
    }
    const int material = materialFor(primitive.material, context, out);

    std::vector<float> vertexColors;
    cgltf_size colorComponents = 0;
    if (colorAccessor && colorAccessor->count == vertexCount) {
        vertexColors    = readFloats(colorAccessor);
        colorComponents = cgltf_num_components(colorAccessor->type);   // 3 (RGB) or 4 (RGBA)
    }

    // ---- Build the vertices in the model's space ----
    // Normals need the inverse transpose of the matrix, or they stop being
    // perpendicular to the surface when a node is scaled non-uniformly.
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));

    const uint32_t baseVertex = static_cast<uint32_t>(out.vertices.size());

    for (size_t v = 0; v < vertexCount; ++v) {
        Vertex vertex;
        vertex.position = glm::vec3(world * glm::vec4(positions[v], 1.0f));

        const glm::vec3 n = normalMatrix * normals[v];
        vertex.normal = (glm::dot(n, n) > 0.0f) ? glm::normalize(n)
                                                : glm::vec3(0.0f, 1.0f, 0.0f);

        vertex.color = baseColor;
        if (colorComponents >= 3)
            vertex.color *= glm::vec3(vertexColors[colorComponents * v],
                                      vertexColors[colorComponents * v + 1],
                                      vertexColors[colorComponents * v + 2]);

        if (uvData.size() >= 2 * vertexCount)
            vertex.uv = glm::vec2(uvData[2 * v], uvData[2 * v + 1]);

        out.vertices.push_back(vertex);
    }

    // A node with a negative determinant mirrors the geometry, which turns
    // every triangle inside out. The glTF spec says to compensate by
    // reversing the winding.
    const bool mirrored = glm::determinant(glm::mat3(world)) < 0.0f;

    const uint32_t firstIndex = static_cast<uint32_t>(out.indices.size());

    for (size_t t = 0; t < indices.size(); t += 3) {
        out.indices.push_back(baseVertex + indices[t]);
        out.indices.push_back(baseVertex + indices[mirrored ? t + 2 : t + 1]);
        out.indices.push_back(baseVertex + indices[mirrored ? t + 1 : t + 2]);
    }
    out.addPart(firstIndex, static_cast<uint32_t>(out.indices.size()) - firstIndex, material);

    return true;
}

/// Visits a node and its children, appending the meshes found along the way.
void visitNode(const cgltf_node& node, ImportContext& context, MeshData& out) {
    if (node.mesh) {
        cgltf_float matrix[16];
        cgltf_node_transform_world(&node, matrix);   // column-major, like GLM
        const glm::mat4 world = glm::make_mat4(matrix);

        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p)
            appendPrimitive(node.mesh->primitives[p], world, context, out);
    }

    for (cgltf_size c = 0; c < node.children_count; ++c)
        visitNode(*node.children[c], context, out);
}

/// Releases a cgltf_data when it goes out of scope, on every return path.
struct GltfDataGuard {
    cgltf_data* data;
    ~GltfDataGuard() { cgltf_free(data); }
};

} // namespace

std::optional<MeshData> importGltf(const std::filesystem::path& file) {
    const std::string path = file.string();

    cgltf_options options{};
    cgltf_data*   data = nullptr;

    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        FE_ERROR("GltfImporter: cannot read '%s' as a glTF file", path.c_str());
        return std::nullopt;
    }
    GltfDataGuard guard{data};

    // Compressed geometry needs a decoder we do not have; better to say so
    // than to produce garbage.
    for (cgltf_size i = 0; i < data->extensions_required_count; ++i) {
        const char* extension = data->extensions_required[i];
        if (std::strcmp(extension, "KHR_draco_mesh_compression") == 0 ||
            std::strcmp(extension, "EXT_meshopt_compression") == 0) {
            FE_ERROR("GltfImporter: '%s' uses %s, which is not supported",
                     path.c_str(), extension);
            return std::nullopt;
        }
    }

    // Loads the binary buffers: the GLB chunk, or the external .bin files
    // next to the .gltf.
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        FE_ERROR("GltfImporter: cannot load the data buffers of '%s'", path.c_str());
        return std::nullopt;
    }

    // Checks that every accessor stays inside its buffer, so the reads below
    // cannot go out of bounds on a damaged file.
    if (cgltf_validate(data) != cgltf_result_success) {
        FE_ERROR("GltfImporter: '%s' is not a valid glTF file", path.c_str());
        return std::nullopt;
    }

    MeshData      result;
    ImportContext context;
    context.directory = file.parent_path();

    const cgltf_scene* scene = data->scene;
    if (!scene && data->scenes_count > 0)
        scene = &data->scenes[0];

    if (scene) {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i)
            visitNode(*scene->nodes[i], context, result);
    } else {
        // No scene declared: walk from every node without a parent.
        for (cgltf_size i = 0; i < data->nodes_count; ++i)
            if (!data->nodes[i].parent)
                visitNode(data->nodes[i], context, result);
    }

    if (result.vertices.empty() || result.indices.empty()) {
        FE_ERROR("GltfImporter: '%s' contains no triangle meshes", path.c_str());
        return std::nullopt;
    }

    return result;
}

} // namespace fe
