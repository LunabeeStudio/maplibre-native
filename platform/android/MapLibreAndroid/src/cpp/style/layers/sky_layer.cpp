#include <GLES3/gl3.h>
#include <jni.h>

#include <mln/style/layers/custom_layer.hpp>
#include <mln/style/layers/custom_layer_render_parameters.hpp>

#include "custom_layer_gl.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace {

std::array<float, 3> rgb(uint32_t argb) {
    return {static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
            static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
            static_cast<float>(argb & 0xFF) / 255.0f};
}

struct SkyColors {
    std::array<float, 3> haze;
    std::array<float, 3> light;
    std::array<float, 3> zenith;
    float lightDegrees;
    float zenithDegrees;
};

constexpr const char* kVertexShader = R"(#version 300 es
precision highp float;
out vec2 v_ndc;
void main() {
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_ndc = corner * 2.0 - 1.0;
    gl_Position = vec4(v_ndc, 1.0, 1.0);
}
)";

constexpr const char* kFragmentShader = R"(#version 300 es
precision highp float;
uniform vec2 u_center;
uniform vec2 u_tan_half_fov;
uniform vec2 u_pitch;
uniform vec3 u_haze;
uniform vec3 u_light;
uniform vec3 u_zenith;
uniform vec2 u_stops;
in vec2 v_ndc;
out vec4 fragColor;
void main() {
    vec2 offset = (v_ndc - u_center) * u_tan_half_fov;
    float lift = -u_pitch.y + offset.y * u_pitch.x;
    float elevation = degrees(asin(clamp(lift / sqrt(1.0 + dot(offset, offset)), -1.0, 1.0)));
    vec3 color = mix(u_haze, u_light, smoothstep(0.0, u_stops.x, elevation));
    color = mix(color, u_zenith, smoothstep(u_stops.x, u_stops.y, elevation));
    float dither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
    fragColor = vec4(color + dither / 255.0, 1.0);
}
)";

// Normalised device position of the map centre: the point on the camera's optical axis, which
// the edge insets move away from the middle of the screen.
std::array<float, 2> perspectiveCentre(const mln::style::CustomLayerRenderParameters& parameters) {
    const double worldSize = cumbly_gl::kTileSize * std::pow(2.0, parameters.zoom);
    const double x = cumbly_gl::mercatorX(parameters.longitude) * worldSize;
    const double y = cumbly_gl::mercatorY(parameters.latitude) * worldSize;
    const auto& m = parameters.projectionMatrix;
    const double w = m[3] * x + m[7] * y + m[15];
    if (std::abs(w) < 1e-9) return {0.0f, 0.0f};
    return {static_cast<float>((m[0] * x + m[4] * y + m[12]) / w), static_cast<float>((m[1] * x + m[5] * y + m[13]) / w)};
}

class SkyLayerHost final : public mln::style::CustomLayerHost {
public:
    explicit SkyLayerHost(SkyColors colors_)
        : colors(colors_) {}

    void initialize(const mln::style::CustomLayerInitParameters&) override {
        program = cumbly_gl::link("SkyLayer", kVertexShader, kFragmentShader);
        if (program == 0) return;
        centreLocation = glGetUniformLocation(program, "u_center");
        tanLocation = glGetUniformLocation(program, "u_tan_half_fov");
        pitchLocation = glGetUniformLocation(program, "u_pitch");
        hazeLocation = glGetUniformLocation(program, "u_haze");
        lightLocation = glGetUniformLocation(program, "u_light");
        zenithLocation = glGetUniformLocation(program, "u_zenith");
        stopsLocation = glGetUniformLocation(program, "u_stops");
        glGenVertexArrays(1, &vertexArray);
    }

    void render(const mln::style::CustomLayerRenderParameters& parameters) override {
        if (program == 0 || parameters.width <= 0.0 || parameters.height <= 0.0) return;
        const double tanHalfFov = std::tan(parameters.fieldOfView / 2.0);
        const std::array<float, 2> centre = perspectiveCentre(parameters);

        glDisable(GL_BLEND);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glUseProgram(program);
        glUniform2f(centreLocation, centre[0], centre[1]);
        glUniform2f(tanLocation, static_cast<float>(tanHalfFov * parameters.width / parameters.height), static_cast<float>(tanHalfFov));
        glUniform2f(pitchLocation, static_cast<float>(std::sin(parameters.pitch)), static_cast<float>(std::cos(parameters.pitch)));
        glUniform3fv(hazeLocation, 1, colors.haze.data());
        glUniform3fv(lightLocation, 1, colors.light.data());
        glUniform3fv(zenithLocation, 1, colors.zenith.data());
        glUniform2f(stopsLocation, colors.lightDegrees, colors.zenithDegrees);
        glBindVertexArray(vertexArray);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
    }

    void contextLost() override {
        program = 0;
        vertexArray = 0;
    }

    void deinitialize() override {
        if (program != 0) glDeleteProgram(program);
        if (vertexArray != 0) glDeleteVertexArrays(1, &vertexArray);
        program = 0;
        vertexArray = 0;
    }

private:
    SkyColors colors;
    GLuint program = 0;
    GLuint vertexArray = 0;
    GLint centreLocation = -1;
    GLint tanLocation = -1;
    GLint pitchLocation = -1;
    GLint hazeLocation = -1;
    GLint lightLocation = -1;
    GLint zenithLocation = -1;
    GLint stopsLocation = -1;
};

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_org_maplibre_android_style_layers_SkyLayer_nativeCreateHost(
    JNIEnv*, jclass, jint haze, jint light, jint zenith, jfloat lightDegrees, jfloat zenithDegrees) {
    return reinterpret_cast<jlong>(new SkyLayerHost(SkyColors{rgb(static_cast<uint32_t>(haze)),
                                                              rgb(static_cast<uint32_t>(light)),
                                                              rgb(static_cast<uint32_t>(zenith)),
                                                              lightDegrees,
                                                              std::max(zenithDegrees, lightDegrees + 0.01f)}));
}

} // extern "C"
