#include <mln/style/layers/custom_layer_host.hpp>

#include "custom_layer_vk.hpp"
#include "sky_layer_data.hpp"

#include <array>
#include <cmath>
#include <memory>
#include <string>

namespace {

using cumbly_sky::perspectiveCentre;
using cumbly_sky::SkyColors;

constexpr const char* kPushBlock = R"(
layout(push_constant) uniform Push {
    vec4 rotation;
    vec2 center;
    vec2 tanHalfFov;
    vec2 pitch;
    vec2 stops;
    vec4 haze;
    vec4 light;
    vec4 zenith;
} pc;
)";

constexpr const char* kVertexShader = R"(
layout(location = 0) out vec2 v_ndc;
void main() {
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_ndc = corner * 2.0 - 1.0;
    vec2 rotated = mat2(pc.rotation.xy, pc.rotation.zw) * v_ndc;
    gl_Position = vec4(rotated.x, -rotated.y, 1.0, 1.0);
}
)";

constexpr const char* kFragmentShader = R"(
layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 fragColor;
void main() {
    vec2 offset = (v_ndc - pc.center) * pc.tanHalfFov;
    float lift = -pc.pitch.y + offset.y * pc.pitch.x;
    float elevation = degrees(asin(clamp(lift / sqrt(1.0 + dot(offset, offset)), -1.0, 1.0)));
    vec3 color = mix(pc.haze.rgb, pc.light.rgb, smoothstep(0.0, pc.stops.x, elevation));
    color = mix(color, pc.zenith.rgb, smoothstep(pc.stops.x, pc.stops.y, elevation));
    float dither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
    fragColor = vec4(color + dither / 255.0, 1.0);
}
)";

struct Push {
    std::array<float, 4> rotation;
    std::array<float, 2> center;
    std::array<float, 2> tanHalfFov;
    std::array<float, 2> pitch;
    std::array<float, 2> stops;
    std::array<float, 4> haze;
    std::array<float, 4> light;
    std::array<float, 4> zenith;
};

static_assert(sizeof(Push) == 96);

std::array<float, 4> pad(const std::array<float, 3>& rgb) {
    return {rgb[0], rgb[1], rgb[2], 0.0f};
}

class SkyLayerHost final : public mln::style::CustomLayerHost {
public:
    explicit SkyLayerHost(SkyColors colors_)
        : colors(colors_) {}

    void initialize(const mln::style::CustomLayerInitParameters& params) override {
        gpu = cumbly_vk::gpuFrom(params);
        const std::string vertex = std::string("#version 450") + kPushBlock + kVertexShader;
        const std::string fragment = std::string("#version 450") + kPushBlock + kFragmentShader;
        vertexModule = cumbly_vk::createShaderModule(gpu, "SkyLayer", EShLangVertex, vertex.c_str());
        fragmentModule = cumbly_vk::createShaderModule(gpu, "SkyLayer", EShLangFragment, fragment.c_str());
        pipelineLayout = cumbly_vk::createPushConstantLayout(gpu);
    }

    void render(const mln::style::CustomLayerRenderParameters& base) override {
        const auto& params = static_cast<const mln::style::vulkan::CustomLayerRenderParameters&>(base);
        if (!gpu || !pipelineLayout || !vertexModule || !fragmentModule || params.width <= 0.0 || params.height <= 0.0) {
            return;
        }

        graveyard.tick();
        if (!ensurePipeline(params)) return;

        const double tanHalfFov = std::tan(params.fieldOfView / 2.0);
        const Push push{cumbly_vk::preRotation(params),
                        perspectiveCentre(params),
                        {static_cast<float>(tanHalfFov * params.width / params.height), static_cast<float>(tanHalfFov)},
                        {static_cast<float>(std::sin(params.pitch)), static_cast<float>(std::cos(params.pitch))},
                        {colors.lightDegrees, colors.zenithDegrees},
                        pad(colors.haze),
                        pad(colors.light),
                        pad(colors.zenith)};

        cumbly_vk::setViewport(params);
        params.commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.get(), params.dispatcher);
        cumbly_vk::pushConstants(params, pipelineLayout.get(), push);
        params.commandBuffer.draw(3, 1, 0, 0, params.dispatcher);
    }

    void contextLost() override { release(false); }

    void deinitialize() override { release(true); }

private:
    bool ensurePipeline(const mln::style::vulkan::CustomLayerRenderParameters& params) {
        if (pipeline && builtRenderPass == params.renderPass && builtSampleCount == params.sampleCount) return true;
        if (pipeline) graveyard.bury(std::move(pipeline));

        cumbly_vk::PipelineSpec spec;
        spec.vertex = vertexModule.get();
        spec.fragment = fragmentModule.get();
        spec.layout = pipelineLayout.get();
        spec.depthTest = true;
        pipeline = cumbly_vk::createPipeline(gpu, spec, params.renderPass, params.sampleCount);
        builtRenderPass = params.renderPass;
        builtSampleCount = params.sampleCount;
        return static_cast<bool>(pipeline);
    }

    void release(bool idle) {
        if (idle) cumbly_vk::waitIdle(gpu);
        pipeline.reset();
        graveyard.clear();
        vertexModule.reset();
        fragmentModule.reset();
        pipelineLayout.reset();
        builtRenderPass = vk::RenderPass{};
        gpu = {};
    }

    SkyColors colors;
    cumbly_vk::Gpu gpu;
    cumbly_vk::Graveyard graveyard;
    cumbly_vk::Unique<vk::ShaderModule> vertexModule;
    cumbly_vk::Unique<vk::ShaderModule> fragmentModule;
    cumbly_vk::Unique<vk::PipelineLayout> pipelineLayout;
    cumbly_vk::Unique<vk::Pipeline> pipeline;
    vk::RenderPass builtRenderPass{};
    vk::SampleCountFlagBits builtSampleCount = vk::SampleCountFlagBits::e1;
};

} // namespace

namespace cumbly_sky {

std::unique_ptr<mln::style::CustomLayerHost> createHost(SkyColors colors) {
    return std::make_unique<SkyLayerHost>(colors);
}

} // namespace cumbly_sky
