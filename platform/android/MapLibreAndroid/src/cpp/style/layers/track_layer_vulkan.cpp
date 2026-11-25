#include <mln/style/layers/custom_layer_host.hpp>

#include "custom_layer_common.hpp"
#include "custom_layer_vk.hpp"
#include "track_layer_data.hpp"

#include <array>
#include <memory>
#include <string>

namespace {

using cumbly_track::Cursor;
using cumbly_track::kFloatsPerVertex;
using cumbly_track::TrackData;

constexpr const char* kVertexShader = R"(#version 450
layout(push_constant) uniform Push {
    mat4 matrix;
    vec4 rotation;
    vec2 viewport;
    float halfWidth;
} pc;
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_prev;
layout(location = 2) in vec3 a_next;
layout(location = 3) in float a_side;
layout(location = 4) in vec4 a_color;
layout(location = 0) out vec4 v_color;
layout(location = 1) out float v_side;
vec2 toScreen(vec4 clip) {
    return clip.xy / clip.w * pc.viewport * 0.5;
}
)";

constexpr const char* kLineMain = R"(
void main() {
    vec4 current = pc.matrix * vec4(a_pos, 1.0);
    vec4 previous = pc.matrix * vec4(a_prev, 1.0);
    vec4 next = pc.matrix * vec4(a_next, 1.0);
    v_color = a_color;
    v_side = a_side;
    if (current.w <= 0.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    vec2 here = toScreen(current);
    vec2 incoming = previous.w > 0.0 ? here - toScreen(previous) : vec2(0.0);
    vec2 outgoing = next.w > 0.0 ? toScreen(next) - here : vec2(0.0);
    if (length(incoming) < 0.0001) incoming = outgoing;
    if (length(outgoing) < 0.0001) outgoing = incoming;
    if (length(incoming) < 0.0001) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    incoming = normalize(incoming);
    outgoing = normalize(outgoing);
    vec2 tangent = incoming + outgoing;
    tangent = length(tangent) < 0.0001 ? outgoing : normalize(tangent);
    vec2 normal = vec2(-tangent.y, tangent.x);
    float miter = pc.halfWidth / max(dot(normal, vec2(-outgoing.y, outgoing.x)), 0.4);
    vec2 offset = normal * miter * a_side;
    gl_Position = finishClip(current + vec4(offset / (pc.viewport * 0.5) * current.w, 0.0, 0.0), pc.rotation);
}
)";

constexpr const char* kFragmentShader = R"(#version 450
layout(location = 0) in vec4 v_color;
layout(location = 1) in float v_side;
layout(location = 0) out vec4 fragColor;
void main() {
    float edge = abs(v_side);
    float feather = max(fwidth(v_side) * 1.5, 0.0001);
    float coverage = 1.0 - smoothstep(1.0 - feather, 1.0, edge);
    float facing = sqrt(max(1.0 - edge * edge, 0.0));
    float light = 0.55 + 0.45 * facing + 0.18 * pow(max(facing - 0.6 * v_side, 0.0), 6.0);
    vec3 shaded = min(v_color.rgb * light, vec3(1.0));
    fragColor = vec4(shaded, 1.0) * v_color.a * coverage;
}
)";

constexpr const char* kCursorVertexShader = R"(#version 450
layout(push_constant) uniform Push {
    vec4 center;
    vec4 rotation;
    vec2 viewport;
    float extent;
    float fillRadius;
    float outerRadius;
    vec4 fill;
    vec4 outline;
} pc;
layout(location = 0) in vec2 a_corner;
layout(location = 0) out vec2 v_local;
)";

constexpr const char* kCursorMain = R"(
void main() {
    v_local = a_corner * pc.extent;
    gl_Position = finishClip(pc.center + vec4(v_local / (pc.viewport * 0.5) * pc.center.w, 0.0, 0.0), pc.rotation);
}
)";

constexpr const char* kCursorFragmentShader = R"(#version 450
layout(push_constant) uniform Push {
    vec4 center;
    vec4 rotation;
    vec2 viewport;
    float extent;
    float fillRadius;
    float outerRadius;
    vec4 fill;
    vec4 outline;
} pc;
layout(location = 0) in vec2 v_local;
layout(location = 0) out vec4 fragColor;
void main() {
    float radius = length(v_local);
    float feather = max(fwidth(radius), 0.0001);
    float inside = 1.0 - smoothstep(pc.fillRadius - feather * 0.5, pc.fillRadius + feather * 0.5, radius);
    float coverage = 1.0 - smoothstep(pc.outerRadius - feather * 0.5, pc.outerRadius + feather * 0.5, radius);
    vec4 color = mix(pc.outline, pc.fill, inside);
    fragColor = vec4(color.rgb * color.a, color.a) * coverage;
}
)";

struct LinePush {
    std::array<float, 16> matrix;
    std::array<float, 4> rotation;
    std::array<float, 2> viewport;
    float halfWidth;
};

struct CursorPush {
    std::array<float, 4> center;
    std::array<float, 4> rotation;
    std::array<float, 2> viewport;
    float extent;
    float fillRadius;
    float outerRadius;
    float padding[3];
    std::array<float, 4> fill;
    std::array<float, 4> outline;
};

static_assert(sizeof(LinePush) == 92);
static_assert(offsetof(CursorPush, fill) == 64);
static_assert(sizeof(CursorPush) == 96);

class TrackLayerHost final : public mln::style::CustomLayerHost {
public:
    explicit TrackLayerHost(std::shared_ptr<TrackData> data_)
        : data(std::move(data_)) {}

    void initialize(const mln::style::CustomLayerInitParameters& params) override {
        gpu = cumbly_vk::gpuFrom(params);
        const std::string lineVertex = std::string(kVertexShader) + cumbly_vk::kFinishClipGlsl + kLineMain;
        const std::string cursorVertex = std::string(kCursorVertexShader) + cumbly_vk::kFinishClipGlsl + kCursorMain;
        lineVertexModule = cumbly_vk::createShaderModule(gpu, "TrackLayer line", EShLangVertex, lineVertex.c_str());
        lineFragmentModule = cumbly_vk::createShaderModule(gpu, "TrackLayer line", EShLangFragment, kFragmentShader);
        cursorVertexModule = cumbly_vk::createShaderModule(
            gpu, "TrackLayer cursor", EShLangVertex, cursorVertex.c_str());
        cursorFragmentModule = cumbly_vk::createShaderModule(
            gpu, "TrackLayer cursor", EShLangFragment, kCursorFragmentShader);
        pipelineLayout = cumbly_vk::createPushConstantLayout(gpu);

        const std::array<float, 8> corners = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
        cursorBuffer = cumbly_vk::createHostBuffer(
            gpu, corners.data(), sizeof(corners), vk::BufferUsageFlagBits::eVertexBuffer);
    }

    void render(const mln::style::CustomLayerRenderParameters& base) override {
        if (!gpu || !pipelineLayout || !lineVertexModule || !lineFragmentModule) return;
        const auto& params = static_cast<const mln::style::vulkan::CustomLayerRenderParameters&>(base);

        graveyard.tick();

        double originX = 0.0;
        double originY = 0.0;
        float halfWidth = 1.5f;
        float altitudeScale = 1.0f;
        bool lineVisible = true;
        Cursor cursor;
        {
            std::lock_guard<std::mutex> lock(data->mutex);
            if (data->revision != uploadedRevision) {
                if (lineBuffer) graveyard.bury(std::move(lineBuffer));
                lineBuffer = cumbly_vk::createHostBuffer(gpu,
                                                         data->vertices.data(),
                                                         data->vertices.size() * sizeof(float),
                                                         vk::BufferUsageFlagBits::eVertexBuffer);
                vertexCount = lineBuffer ? static_cast<uint32_t>(data->vertices.size() / kFloatsPerVertex) : 0;
                uploadedRevision = data->revision;
            }
            originX = data->originX;
            originY = data->originY;
            halfWidth = data->halfWidth;
            altitudeScale = data->altitudeScale;
            lineVisible = data->lineVisible;
            cursor = data->cursor;
        }

        const bool drawsLine = lineVisible && vertexCount >= 4;
        const bool drawsCursor = cursor.visible && cursorVertexModule && cursorFragmentModule && cursorBuffer;
        if (!drawsLine && !drawsCursor) return;

        if (!ensurePipelines(params)) return;

        cumbly_vk::setViewport(params);
        const auto rotation = cumbly_vk::preRotation(params);
        const std::array<float, 2> viewport = {static_cast<float>(params.width), static_cast<float>(params.height)};
        const auto& cmd = params.commandBuffer;

        if (drawsLine) {
            LinePush push{cumbly_common::modelViewProjection(params, originX, originY, altitudeScale),
                          rotation,
                          viewport,
                          halfWidth};
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, linePipeline.get(), params.dispatcher);
            cumbly_vk::pushConstants(params, pipelineLayout.get(), push);
            const vk::Buffer buffer = lineBuffer.handle();
            const vk::DeviceSize offset = 0;
            cmd.bindVertexBuffers(0, buffer, offset, params.dispatcher);
            cmd.draw(vertexCount, 1, 0, 0, params.dispatcher);
        }

        if (drawsCursor) {
            const auto m = cumbly_common::modelViewProjection(params, cursor.x, cursor.y, altitudeScale);
            const float altitude = static_cast<float>(cursor.altitude);
            const std::array<float, 4> center = {
                m[12] + altitude * m[8], m[13] + altitude * m[9], m[14] + altitude * m[10], m[15] + altitude * m[11]};
            if (center[3] > 0.0f) {
                const float fillRadius = cursor.diameter * 0.5f;
                const float outerRadius = fillRadius + cursor.outlineWidth;
                CursorPush push{center,
                                rotation,
                                viewport,
                                outerRadius + 1.0f,
                                fillRadius,
                                outerRadius,
                                {},
                                cursor.fill,
                                cursor.outline};
                cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, cursorPipeline.get(), params.dispatcher);
                cumbly_vk::pushConstants(params, pipelineLayout.get(), push);
                const vk::Buffer buffer = cursorBuffer.handle();
                const vk::DeviceSize offset = 0;
                cmd.bindVertexBuffers(0, buffer, offset, params.dispatcher);
                cmd.draw(4, 1, 0, 0, params.dispatcher);
            }
        }
    }

    void contextLost() override { release(false); }

    void deinitialize() override { release(true); }

private:
    bool ensurePipelines(const mln::style::vulkan::CustomLayerRenderParameters& params) {
        const bool depthTest = params.terrainExaggeration > 0.0;
        if (linePipeline && cursorPipeline && builtRenderPass == params.renderPass &&
            builtSampleCount == params.sampleCount && builtDepthTest == depthTest) {
            return true;
        }
        if (linePipeline) graveyard.bury(std::move(linePipeline));
        if (cursorPipeline) graveyard.bury(std::move(cursorPipeline));

        cumbly_vk::PipelineSpec line;
        line.vertex = lineVertexModule.get();
        line.fragment = lineFragmentModule.get();
        line.layout = pipelineLayout.get();
        line.topology = vk::PrimitiveTopology::eTriangleStrip;
        line.bindings = {
            vk::VertexInputBindingDescription(0, kFloatsPerVertex * sizeof(float), vk::VertexInputRate::eVertex)};
        line.attributes = {
            vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, 0),
            vk::VertexInputAttributeDescription(1, 0, vk::Format::eR32G32B32Sfloat, 3 * sizeof(float)),
            vk::VertexInputAttributeDescription(2, 0, vk::Format::eR32G32B32Sfloat, 6 * sizeof(float)),
            vk::VertexInputAttributeDescription(3, 0, vk::Format::eR32Sfloat, 9 * sizeof(float)),
            vk::VertexInputAttributeDescription(4, 0, vk::Format::eR8G8B8A8Unorm, 10 * sizeof(float)),
        };
        line.depthTest = depthTest;
        linePipeline = cumbly_vk::createPipeline(gpu, line, params.renderPass, params.sampleCount);

        if (cursorVertexModule && cursorFragmentModule) {
            cumbly_vk::PipelineSpec cursor;
            cursor.vertex = cursorVertexModule.get();
            cursor.fragment = cursorFragmentModule.get();
            cursor.layout = pipelineLayout.get();
            cursor.topology = vk::PrimitiveTopology::eTriangleStrip;
            cursor.bindings = {vk::VertexInputBindingDescription(0, 2 * sizeof(float), vk::VertexInputRate::eVertex)};
            cursor.attributes = {vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32Sfloat, 0)};
            cursor.depthTest = depthTest;
            cursorPipeline = cumbly_vk::createPipeline(gpu, cursor, params.renderPass, params.sampleCount);
        }

        builtRenderPass = params.renderPass;
        builtSampleCount = params.sampleCount;
        builtDepthTest = depthTest;
        return static_cast<bool>(linePipeline);
    }

    void release(bool idle) {
        if (idle) cumbly_vk::waitIdle(gpu);
        linePipeline.reset();
        cursorPipeline.reset();
        lineBuffer = {};
        cursorBuffer = {};
        graveyard.clear();
        lineVertexModule.reset();
        lineFragmentModule.reset();
        cursorVertexModule.reset();
        cursorFragmentModule.reset();
        pipelineLayout.reset();
        builtRenderPass = vk::RenderPass{};
        vertexCount = 0;
        uploadedRevision = 0;
        gpu = {};
    }

    std::shared_ptr<TrackData> data;
    cumbly_vk::Gpu gpu;
    cumbly_vk::Graveyard graveyard;
    cumbly_vk::Unique<vk::ShaderModule> lineVertexModule;
    cumbly_vk::Unique<vk::ShaderModule> lineFragmentModule;
    cumbly_vk::Unique<vk::ShaderModule> cursorVertexModule;
    cumbly_vk::Unique<vk::ShaderModule> cursorFragmentModule;
    cumbly_vk::Unique<vk::PipelineLayout> pipelineLayout;
    cumbly_vk::Unique<vk::Pipeline> linePipeline;
    cumbly_vk::Unique<vk::Pipeline> cursorPipeline;
    vk::RenderPass builtRenderPass{};
    vk::SampleCountFlagBits builtSampleCount = vk::SampleCountFlagBits::e1;
    bool builtDepthTest = false;
    cumbly_vk::HostBuffer lineBuffer;
    cumbly_vk::HostBuffer cursorBuffer;
    uint32_t vertexCount = 0;
    uint64_t uploadedRevision = 0;
};

} // namespace

namespace cumbly_track {

std::unique_ptr<mln::style::CustomLayerHost> createHost(std::shared_ptr<TrackData> data) {
    return std::make_unique<TrackLayerHost>(std::move(data));
}

} // namespace cumbly_track
