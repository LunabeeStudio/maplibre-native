#pragma once

#include <mln/style/layers/vulkan/custom_layer_init_parameters.hpp>
#include <mln/style/layers/vulkan/custom_layer_render_parameters.hpp>
#include <mln/util/logging.hpp>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cumbly_vk {

using Dispatcher = vk::detail::DispatchLoaderDynamic;

template <typename T>
using Unique = vk::UniqueHandle<T, Dispatcher>;

struct Gpu {
    vk::Device device;
    vk::PhysicalDevice physicalDevice;
    const Dispatcher* dispatcher = nullptr;

    explicit operator bool() const { return dispatcher != nullptr && device; }
};

inline Gpu gpuFrom(const mln::style::CustomLayerInitParameters& base) {
    const auto& params = static_cast<const mln::style::vulkan::CustomLayerInitParameters&>(base);
    return {params.device, params.physicalDevice, &params.dispatcher};
}

inline std::vector<uint32_t> compileGlsl(const char* label, EShLanguage stage, const char* source) {
    glslang::TShader shader(stage);
    const char* strings[] = {source};
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    shader.setEntryPoint("main");

    constexpr auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, ENoProfile, false, true, messages)) {
        mln::Log::Error(mln::Event::Shader, std::string(label) + " - " + shader.getInfoLog());
        return {};
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        mln::Log::Error(mln::Event::Shader, std::string(label) + " - " + program.getInfoLog());
        return {};
    }

    std::vector<uint32_t> spirv;
    glslang::SpvOptions options;
    options.stripDebugInfo = true;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &options);
    return spirv;
}

inline Unique<vk::ShaderModule> createShaderModule(const Gpu& gpu, const char* label, EShLanguage stage, const char* source) {
    const auto spirv = compileGlsl(label, stage, source);
    if (spirv.empty()) return {};
    try {
        return gpu.device.createShaderModuleUnique(
            vk::ShaderModuleCreateInfo().setCode(spirv), nullptr, *gpu.dispatcher);
    } catch (const std::exception& error) {
        mln::Log::Error(mln::Event::Render, std::string(label) + " shader module: " + error.what());
        return {};
    }
}

struct HostBuffer {
    Unique<vk::Buffer> buffer;
    Unique<vk::DeviceMemory> memory;
    vk::DeviceSize size = 0;

    explicit operator bool() const { return static_cast<bool>(buffer); }
    vk::Buffer handle() const { return buffer.get(); }
};

inline HostBuffer createHostBuffer(const Gpu& gpu, const void* data, vk::DeviceSize size, vk::BufferUsageFlags usage) {
    HostBuffer result;
    if (size == 0) return result;
    try {
        const auto& dispatcher = *gpu.dispatcher;
        result.buffer = gpu.device.createBufferUnique(
            vk::BufferCreateInfo().setSize(size).setUsage(usage).setSharingMode(vk::SharingMode::eExclusive),
            nullptr,
            dispatcher);
        const auto requirements = gpu.device.getBufferMemoryRequirements(result.buffer.get(), dispatcher);
        const auto properties = gpu.physicalDevice.getMemoryProperties(dispatcher);
        const auto wanted = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        uint32_t typeIndex = std::numeric_limits<uint32_t>::max();
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
                typeIndex = i;
                break;
            }
        }
        if (typeIndex == std::numeric_limits<uint32_t>::max()) {
            mln::Log::Error(mln::Event::Render, "No host visible coherent memory type for a custom layer buffer");
            return {};
        }
        result.memory = gpu.device.allocateMemoryUnique(
            vk::MemoryAllocateInfo().setAllocationSize(requirements.size).setMemoryTypeIndex(typeIndex),
            nullptr,
            dispatcher);
        gpu.device.bindBufferMemory(result.buffer.get(), result.memory.get(), 0, dispatcher);
        void* mapped = gpu.device.mapMemory(result.memory.get(), 0, VK_WHOLE_SIZE, {}, dispatcher);
        std::memcpy(mapped, data, static_cast<size_t>(size));
        gpu.device.unmapMemory(result.memory.get(), dispatcher);
        result.size = size;
    } catch (const std::exception& error) {
        mln::Log::Error(mln::Event::Render, std::string("Custom layer buffer: ") + error.what());
        return {};
    }
    return result;
}

class Graveyard {
public:
    template <typename T>
    void bury(T&& object) {
        entries.push_back({0, std::make_shared<std::remove_reference_t<T>>(std::move(object))});
    }

    void tick() {
        for (auto& entry : entries) ++entry.age;
        entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& e) { return e.age > kFramesInFlight; }),
                      entries.end());
    }

    void clear() { entries.clear(); }

private:
    static constexpr int kFramesInFlight = 3;
    struct Entry {
        int age;
        std::shared_ptr<void> object;
    };
    std::vector<Entry> entries;
};

inline void waitIdle(const Gpu& gpu) {
    if (!gpu) return;
    try {
        gpu.device.waitIdle(*gpu.dispatcher);
    } catch (const std::exception&) {
    }
}

struct PipelineSpec {
    vk::ShaderModule vertex;
    vk::ShaderModule fragment;
    vk::PipelineLayout layout;
    vk::PrimitiveTopology topology = vk::PrimitiveTopology::eTriangleList;
    std::vector<vk::VertexInputBindingDescription> bindings;
    std::vector<vk::VertexInputAttributeDescription> attributes;
    bool depthTest = false;
};

inline Unique<vk::Pipeline> createPipeline(const Gpu& gpu,
                                         const PipelineSpec& spec,
                                         vk::RenderPass renderPass,
                                         vk::SampleCountFlagBits sampleCount) {
    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {
        vk::PipelineShaderStageCreateInfo().setStage(vk::ShaderStageFlagBits::eVertex).setModule(spec.vertex).setPName("main"),
        vk::PipelineShaderStageCreateInfo().setStage(vk::ShaderStageFlagBits::eFragment).setModule(spec.fragment).setPName("main")};

    const auto vertexInput = vk::PipelineVertexInputStateCreateInfo()
                                 .setVertexBindingDescriptions(spec.bindings)
                                 .setVertexAttributeDescriptions(spec.attributes);
    const auto inputAssembly = vk::PipelineInputAssemblyStateCreateInfo().setTopology(spec.topology);
    const auto viewportState = vk::PipelineViewportStateCreateInfo().setViewportCount(1).setScissorCount(1);
    const auto rasterizer = vk::PipelineRasterizationStateCreateInfo()
                                .setPolygonMode(vk::PolygonMode::eFill)
                                .setCullMode(vk::CullModeFlagBits::eNone)
                                .setFrontFace(vk::FrontFace::eCounterClockwise)
                                .setLineWidth(1.0f);
    const auto multisample = vk::PipelineMultisampleStateCreateInfo().setRasterizationSamples(sampleCount);
    const auto depthStencil = vk::PipelineDepthStencilStateCreateInfo()
                                  .setDepthTestEnable(spec.depthTest)
                                  .setDepthWriteEnable(false)
                                  .setDepthCompareOp(vk::CompareOp::eLessOrEqual)
                                  .setStencilTestEnable(false);
    const auto blendAttachment = vk::PipelineColorBlendAttachmentState()
                                     .setBlendEnable(true)
                                     .setSrcColorBlendFactor(vk::BlendFactor::eOne)
                                     .setDstColorBlendFactor(vk::BlendFactor::eOneMinusSrcAlpha)
                                     .setColorBlendOp(vk::BlendOp::eAdd)
                                     .setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
                                     .setDstAlphaBlendFactor(vk::BlendFactor::eOneMinusSrcAlpha)
                                     .setAlphaBlendOp(vk::BlendOp::eAdd)
                                     .setColorWriteMask(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                                        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
    const auto colorBlend = vk::PipelineColorBlendStateCreateInfo().setAttachments(blendAttachment);
    const std::array<vk::DynamicState, 2> dynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const auto dynamicState = vk::PipelineDynamicStateCreateInfo().setDynamicStates(dynamicStates);

    const auto createInfo = vk::GraphicsPipelineCreateInfo()
                                .setStages(stages)
                                .setPVertexInputState(&vertexInput)
                                .setPInputAssemblyState(&inputAssembly)
                                .setPViewportState(&viewportState)
                                .setPRasterizationState(&rasterizer)
                                .setPMultisampleState(&multisample)
                                .setPDepthStencilState(&depthStencil)
                                .setPColorBlendState(&colorBlend)
                                .setPDynamicState(&dynamicState)
                                .setLayout(spec.layout)
                                .setRenderPass(renderPass)
                                .setSubpass(0);
    try {
        return std::move(gpu.device.createGraphicsPipelineUnique(nullptr, createInfo, nullptr, *gpu.dispatcher).value);
    } catch (const std::exception& error) {
        mln::Log::Error(mln::Event::Render, std::string("Custom layer pipeline: ") + error.what());
        return {};
    }
}

constexpr uint32_t kPushConstantBytes = 128;

inline Unique<vk::PipelineLayout> createPushConstantLayout(const Gpu& gpu) {
    const auto range = vk::PushConstantRange()
                           .setStageFlags(vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment)
                           .setOffset(0)
                           .setSize(kPushConstantBytes);
    try {
        return gpu.device.createPipelineLayoutUnique(
            vk::PipelineLayoutCreateInfo().setPushConstantRanges(range), nullptr, *gpu.dispatcher);
    } catch (const std::exception& error) {
        mln::Log::Error(mln::Event::Render, std::string("Custom layer pipeline layout: ") + error.what());
        return {};
    }
}

template <typename Push>
void pushConstants(const mln::style::vulkan::CustomLayerRenderParameters& params, vk::PipelineLayout layout, const Push& push) {
    static_assert(sizeof(Push) <= kPushConstantBytes);
    params.commandBuffer.pushConstants(layout,
                                       vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                       0,
                                       static_cast<uint32_t>(sizeof(Push)),
                                       &push,
                                       params.dispatcher);
}

inline void setViewport(const mln::style::vulkan::CustomLayerRenderParameters& params) {
    const auto width = static_cast<float>(params.width);
    const auto height = static_cast<float>(params.height);
    const vk::Viewport viewport(0.0f, 0.0f, width, height, 0.0f, 1.0f);
    const vk::Rect2D scissor({0, 0}, {static_cast<uint32_t>(params.width), static_cast<uint32_t>(params.height)});
    params.commandBuffer.setViewport(0, viewport, params.dispatcher);
    params.commandBuffer.setScissor(0, scissor, params.dispatcher);
}

inline std::array<float, 4> preRotation(const mln::style::vulkan::CustomLayerRenderParameters& params) {
    const float angle = params.screenPreRotationRadiansClockwise;
    const float s = std::sin(angle);
    const float c = std::cos(angle);
    return {c, -s, s, c};
}

constexpr const char* kFinishClipGlsl = R"(
vec4 finishClip(vec4 clip, vec4 rotation) {
    clip.z = (clip.z + clip.w) * 0.5;
    clip.xy = mat2(rotation.xy, rotation.zw) * clip.xy;
    clip.y = -clip.y;
    return clip;
}
)";

} // namespace cumbly_vk
