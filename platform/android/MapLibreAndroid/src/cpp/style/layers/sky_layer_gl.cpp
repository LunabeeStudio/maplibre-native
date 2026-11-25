#include <GLES3/gl3.h>

#include "custom_layer_gl.hpp"
#include "sky_layer_data.hpp"

#include <array>
#include <cmath>
#include <memory>

namespace {

using cumbly_sky::perspectiveCentre;
using cumbly_sky::SkyColors;

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

namespace cumbly_sky {

std::unique_ptr<mln::style::CustomLayerHost> createHost(SkyColors colors) {
    return std::make_unique<SkyLayerHost>(colors);
}

} // namespace cumbly_sky
