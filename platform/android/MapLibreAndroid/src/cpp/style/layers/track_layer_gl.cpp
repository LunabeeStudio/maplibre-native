#include <GLES3/gl3.h>

#include <mln/style/layers/custom_layer_render_parameters.hpp>

#include "custom_layer_gl.hpp"
#include "track_layer_data.hpp"

#include <array>
#include <memory>

namespace {

using cumbly_gl::modelViewProjection;
using cumbly_track::Cursor;
using cumbly_track::kFloatsPerVertex;
using cumbly_track::TrackData;

constexpr const char* kVertexShader = R"(#version 300 es
precision highp float;
uniform mat4 u_matrix;
uniform vec2 u_viewport;
uniform float u_half_width;
in vec3 a_pos;
in vec3 a_prev;
in vec3 a_next;
in float a_side;
in vec4 a_color;
out vec4 v_color;
out float v_side;
vec2 toScreen(vec4 clip) {
    return clip.xy / clip.w * u_viewport * 0.5;
}
void main() {
    vec4 current = u_matrix * vec4(a_pos, 1.0);
    vec4 previous = u_matrix * vec4(a_prev, 1.0);
    vec4 next = u_matrix * vec4(a_next, 1.0);
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
    float miter = u_half_width / max(dot(normal, vec2(-outgoing.y, outgoing.x)), 0.4);
    vec2 offset = normal * miter * a_side;
    gl_Position = current + vec4(offset / (u_viewport * 0.5) * current.w, 0.0, 0.0);
}
)";

constexpr const char* kFragmentShader = R"(#version 300 es
precision mediump float;
in vec4 v_color;
in float v_side;
out vec4 fragColor;
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

constexpr const char* kCursorVertexShader = R"(#version 300 es
precision highp float;
uniform mat4 u_matrix;
uniform vec2 u_viewport;
uniform float u_altitude;
uniform float u_extent;
in vec2 a_corner;
out vec2 v_local;
void main() {
    vec4 center = u_matrix * vec4(0.0, 0.0, u_altitude, 1.0);
    v_local = a_corner * u_extent;
    if (center.w <= 0.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    gl_Position = center + vec4(v_local / (u_viewport * 0.5) * center.w, 0.0, 0.0);
}
)";

constexpr const char* kCursorFragmentShader = R"(#version 300 es
precision highp float;
uniform float u_fill_radius;
uniform float u_outer_radius;
uniform vec4 u_fill;
uniform vec4 u_outline;
in vec2 v_local;
out vec4 fragColor;
void main() {
    float radius = length(v_local);
    float feather = max(fwidth(radius), 0.0001);
    float inside = 1.0 - smoothstep(u_fill_radius - feather * 0.5, u_fill_radius + feather * 0.5, radius);
    float coverage = 1.0 - smoothstep(u_outer_radius - feather * 0.5, u_outer_radius + feather * 0.5, radius);
    vec4 color = mix(u_outline, u_fill, inside);
    fragColor = vec4(color.rgb * color.a, color.a) * coverage;
}
)";

class TrackLayerHost final : public mln::style::CustomLayerHost {
public:
    explicit TrackLayerHost(std::shared_ptr<TrackData> data_)
        : data(std::move(data_)) {}

    void initialize(const mln::style::CustomLayerInitParameters&) override {
        program = cumbly_gl::link("TrackLayer", kVertexShader, kFragmentShader, {"a_pos", "a_prev", "a_next", "a_side", "a_color"});
        if (program != 0) {
            matrixLocation = glGetUniformLocation(program, "u_matrix");
            viewportLocation = glGetUniformLocation(program, "u_viewport");
            halfWidthLocation = glGetUniformLocation(program, "u_half_width");
            glGenVertexArrays(1, &vertexArray);
            glGenBuffers(1, &buffer);
        }
        cursorProgram = cumbly_gl::link("TrackLayer", kCursorVertexShader, kCursorFragmentShader, {"a_corner"});
        if (cursorProgram != 0) {
            cursorMatrixLocation = glGetUniformLocation(cursorProgram, "u_matrix");
            cursorViewportLocation = glGetUniformLocation(cursorProgram, "u_viewport");
            cursorAltitudeLocation = glGetUniformLocation(cursorProgram, "u_altitude");
            cursorExtentLocation = glGetUniformLocation(cursorProgram, "u_extent");
            cursorFillRadiusLocation = glGetUniformLocation(cursorProgram, "u_fill_radius");
            cursorOuterRadiusLocation = glGetUniformLocation(cursorProgram, "u_outer_radius");
            cursorFillLocation = glGetUniformLocation(cursorProgram, "u_fill");
            cursorOutlineLocation = glGetUniformLocation(cursorProgram, "u_outline");
            glGenVertexArrays(1, &cursorArray);
            glGenBuffers(1, &cursorBuffer);
            const std::array<float, 8> corners = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
            glBindVertexArray(cursorArray);
            glBindBuffer(GL_ARRAY_BUFFER, cursorBuffer);
            glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners.data(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), reinterpret_cast<const void*>(0));
            glBindVertexArray(0);
        }
    }

    void render(const mln::style::CustomLayerRenderParameters& parameters) override {
        if (program == 0) return;
        double originX = 0.0;
        double originY = 0.0;
        float halfWidth = 1.5f;
        float altitudeScale = 1.0f;
        bool lineVisible = true;
        Cursor cursor;
        {
            std::lock_guard<std::mutex> lock(data->mutex);
            if (data->revision != uploadedRevision) {
                glBindBuffer(GL_ARRAY_BUFFER, buffer);
                glBufferData(GL_ARRAY_BUFFER,
                             static_cast<GLsizeiptr>(data->vertices.size() * sizeof(float)),
                             data->vertices.empty() ? nullptr : data->vertices.data(),
                             GL_STATIC_DRAW);
                vertexCount = static_cast<GLsizei>(data->vertices.size() / kFloatsPerVertex);
                uploadedRevision = data->revision;
                configureVertexArray();
            }
            originX = data->originX;
            originY = data->originY;
            halfWidth = data->halfWidth;
            altitudeScale = data->altitudeScale;
            lineVisible = data->lineVisible;
            cursor = data->cursor;
        }
        const bool drawsLine = lineVisible && vertexCount >= 4;
        const bool drawsCursor = cursor.visible && cursorProgram != 0;
        if (!drawsLine && !drawsCursor) return;

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        if (drawsLine) {
            const std::array<float, 16> matrix = modelViewProjection(parameters, originX, originY, altitudeScale);
            glUseProgram(program);
            glUniformMatrix4fv(matrixLocation, 1, GL_FALSE, matrix.data());
            glUniform2f(viewportLocation, static_cast<float>(parameters.width), static_cast<float>(parameters.height));
            glUniform1f(halfWidthLocation, halfWidth);
            glBindVertexArray(vertexArray);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, vertexCount);
            glBindVertexArray(0);
        }
        if (drawsCursor) {
            const float fillRadius = cursor.diameter * 0.5f;
            const float outerRadius = fillRadius + cursor.outlineWidth;
            const std::array<float, 16> matrix = modelViewProjection(parameters, cursor.x, cursor.y, altitudeScale);
            glUseProgram(cursorProgram);
            glUniformMatrix4fv(cursorMatrixLocation, 1, GL_FALSE, matrix.data());
            glUniform2f(cursorViewportLocation, static_cast<float>(parameters.width), static_cast<float>(parameters.height));
            glUniform1f(cursorAltitudeLocation, static_cast<float>(cursor.altitude));
            glUniform1f(cursorExtentLocation, outerRadius + 1.0f);
            glUniform1f(cursorFillRadiusLocation, fillRadius);
            glUniform1f(cursorOuterRadiusLocation, outerRadius);
            glUniform4fv(cursorFillLocation, 1, cursor.fill.data());
            glUniform4fv(cursorOutlineLocation, 1, cursor.outline.data());
            glBindVertexArray(cursorArray);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray(0);
        }
    }

    void contextLost() override {
        program = 0;
        vertexArray = 0;
        buffer = 0;
        cursorProgram = 0;
        cursorArray = 0;
        cursorBuffer = 0;
        uploadedRevision = 0;
    }

    void deinitialize() override {
        if (program != 0) glDeleteProgram(program);
        if (buffer != 0) glDeleteBuffers(1, &buffer);
        if (vertexArray != 0) glDeleteVertexArrays(1, &vertexArray);
        if (cursorProgram != 0) glDeleteProgram(cursorProgram);
        if (cursorBuffer != 0) glDeleteBuffers(1, &cursorBuffer);
        if (cursorArray != 0) glDeleteVertexArrays(1, &cursorArray);
        program = 0;
        buffer = 0;
        vertexArray = 0;
        cursorProgram = 0;
        cursorBuffer = 0;
        cursorArray = 0;
    }

private:
    void configureVertexArray() {
        glBindVertexArray(vertexArray);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        const GLsizei stride = kFloatsPerVertex * sizeof(float);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(3 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(6 * sizeof(float)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(9 * sizeof(float)));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<const void*>(10 * sizeof(float)));
        glBindVertexArray(0);
    }

    std::shared_ptr<TrackData> data;
    GLuint program = 0;
    GLuint vertexArray = 0;
    GLuint buffer = 0;
    GLint matrixLocation = -1;
    GLint viewportLocation = -1;
    GLint halfWidthLocation = -1;
    GLsizei vertexCount = 0;
    GLuint cursorProgram = 0;
    GLuint cursorArray = 0;
    GLuint cursorBuffer = 0;
    GLint cursorMatrixLocation = -1;
    GLint cursorViewportLocation = -1;
    GLint cursorAltitudeLocation = -1;
    GLint cursorExtentLocation = -1;
    GLint cursorFillRadiusLocation = -1;
    GLint cursorOuterRadiusLocation = -1;
    GLint cursorFillLocation = -1;
    GLint cursorOutlineLocation = -1;
    uint64_t uploadedRevision = 0;
};

} // namespace

namespace cumbly_track {

std::unique_ptr<mln::style::CustomLayerHost> createHost(std::shared_ptr<TrackData> data) {
    return std::make_unique<TrackLayerHost>(std::move(data));
}

} // namespace cumbly_track
