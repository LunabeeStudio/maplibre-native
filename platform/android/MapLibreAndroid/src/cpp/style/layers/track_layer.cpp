#include <GLES3/gl3.h>
#include <jni.h>

#include <mln/style/layers/custom_layer.hpp>
#include <mln/style/layers/custom_layer_render_parameters.hpp>
#include <mln/util/logging.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <vector>

namespace {

constexpr double kTileSize = 512.0;

struct Cursor {
    bool visible = false;
    double x = 0.0;
    double y = 0.0;
    double altitude = 0.0;
    float diameter = 12.0f;
    float outlineWidth = 2.0f;
    std::array<float, 4> fill = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> outline = {19.0f / 255.0f, 53.0f / 255.0f, 94.0f / 255.0f, 1.0f};
};

struct TrackData {
    std::mutex mutex;
    std::vector<float> vertices;
    double originX = 0.0;
    double originY = 0.0;
    float halfWidth = 1.5f;
    float altitudeScale = 1.0f;
    bool lineVisible = true;
    Cursor cursor;
    uint64_t revision = 0;
};

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

GLuint compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) {
        std::array<char, 1024> log{};
        glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
        mln::Log::Error(mln::Event::Render, std::string("TrackLayer shader: ") + log.data());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint link(const char* vertexSource, const char* fragmentSource, std::initializer_list<const char*> attributes) {
    GLuint vertexShader = compile(GL_VERTEX_SHADER, vertexSource);
    GLuint fragmentShader = compile(GL_FRAGMENT_SHADER, fragmentSource);
    if (vertexShader == 0 || fragmentShader == 0) {
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    GLuint index = 0;
    for (const char* attribute : attributes) {
        glBindAttribLocation(program, index++, attribute);
    }
    glLinkProgram(program);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    GLint status = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status == GL_FALSE) {
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

std::array<float, 16> modelViewProjection(const mln::style::CustomLayerRenderParameters& parameters,
                                          double x,
                                          double y,
                                          double altitudeScale) {
    const double worldSize = kTileSize * std::pow(2.0, parameters.zoom);
    std::array<double, 16> model{};
    model[0] = worldSize;
    model[5] = worldSize;
    model[10] = altitudeScale;
    model[12] = x * worldSize;
    model[13] = y * worldSize;
    model[15] = 1.0;
    std::array<float, 16> matrix{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += parameters.projectionMatrix[k * 4 + row] * model[column * 4 + k];
            }
            matrix[column * 4 + row] = static_cast<float>(sum);
        }
    }
    return matrix;
}

class TrackLayerHost final : public mln::style::CustomLayerHost {
public:
    explicit TrackLayerHost(std::shared_ptr<TrackData> data_)
        : data(std::move(data_)) {}

    void initialize(const mln::style::CustomLayerInitParameters&) override {
        program = link(kVertexShader, kFragmentShader, {"a_pos", "a_prev", "a_next", "a_side", "a_color"});
        if (program != 0) {
            matrixLocation = glGetUniformLocation(program, "u_matrix");
            viewportLocation = glGetUniformLocation(program, "u_viewport");
            halfWidthLocation = glGetUniformLocation(program, "u_half_width");
            glGenVertexArrays(1, &vertexArray);
            glGenBuffers(1, &buffer);
        }
        cursorProgram = link(kCursorVertexShader, kCursorFragmentShader, {"a_corner"});
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

    static constexpr int kFloatsPerVertex = 11;

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

std::shared_ptr<TrackData>& handle(jlong state) {
    return *reinterpret_cast<std::shared_ptr<TrackData>*>(state);
}

double mercatorX(double longitude) {
    return (longitude + 180.0) / 360.0;
}

double mercatorY(double latitude) {
    const double radians = latitude * M_PI / 180.0;
    return (1.0 - std::log(std::tan(radians) + 1.0 / std::cos(radians)) / M_PI) / 2.0;
}

std::array<float, 4> unpackColor(jint argb) {
    const uint32_t color = static_cast<uint32_t>(argb);
    return {static_cast<float>((color >> 16) & 0xFF) / 255.0f,
            static_cast<float>((color >> 8) & 0xFF) / 255.0f,
            static_cast<float>(color & 0xFF) / 255.0f,
            static_cast<float>((color >> 24) & 0xFF) / 255.0f};
}

void put(std::vector<float>& out, double x, double y, double z) {
    out.push_back(static_cast<float>(x));
    out.push_back(static_cast<float>(y));
    out.push_back(static_cast<float>(z));
}

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeCreateState(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(new std::shared_ptr<TrackData>(std::make_shared<TrackData>()));
}

JNIEXPORT jlong JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeCreateHost(JNIEnv*, jclass, jlong state) {
    return reinterpret_cast<jlong>(new TrackLayerHost(handle(state)));
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeReleaseState(JNIEnv*, jclass, jlong state) {
    delete reinterpret_cast<std::shared_ptr<TrackData>*>(state);
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeSetTrack(
    JNIEnv* env, jclass, jlong state, jdoubleArray coordinates, jintArray colors) {
    const jsize pointCount = env->GetArrayLength(colors);
    if (env->GetArrayLength(coordinates) < pointCount * 3) return;
    std::vector<double> lonLatAlt(static_cast<size_t>(pointCount) * 3);
    std::vector<jint> argb(static_cast<size_t>(pointCount));
    env->GetDoubleArrayRegion(coordinates, 0, pointCount * 3, lonLatAlt.data());
    env->GetIntArrayRegion(colors, 0, pointCount, argb.data());

    std::vector<double> xs(static_cast<size_t>(pointCount));
    std::vector<double> ys(static_cast<size_t>(pointCount));
    for (jsize i = 0; i < pointCount; ++i) {
        xs[i] = mercatorX(lonLatAlt[i * 3]);
        ys[i] = mercatorY(lonLatAlt[i * 3 + 1]);
    }
    const double originX = pointCount > 0 ? xs[0] : 0.0;
    const double originY = pointCount > 0 ? ys[0] : 0.0;

    std::vector<float> vertices;
    vertices.reserve(static_cast<size_t>(pointCount) * 2 * TrackLayerHost::kFloatsPerVertex);
    for (jsize i = 0; i < pointCount; ++i) {
        const jsize previous = i > 0 ? i - 1 : i;
        const jsize next = i + 1 < pointCount ? i + 1 : i;
        const uint32_t color = static_cast<uint32_t>(argb[i]);
        const std::array<uint8_t, 4> rgba = {static_cast<uint8_t>((color >> 16) & 0xFF),
                                             static_cast<uint8_t>((color >> 8) & 0xFF),
                                             static_cast<uint8_t>(color & 0xFF),
                                             static_cast<uint8_t>((color >> 24) & 0xFF)};
        float packedColor = 0.0f;
        static_assert(sizeof(packedColor) == sizeof(rgba));
        std::memcpy(&packedColor, rgba.data(), sizeof(packedColor));
        for (const float side : {-1.0f, 1.0f}) {
            put(vertices, xs[i] - originX, ys[i] - originY, lonLatAlt[i * 3 + 2]);
            put(vertices, xs[previous] - originX, ys[previous] - originY, lonLatAlt[previous * 3 + 2]);
            put(vertices, xs[next] - originX, ys[next] - originY, lonLatAlt[next * 3 + 2]);
            vertices.push_back(side);
            vertices.push_back(packedColor);
        }
    }

    auto& data = handle(state);
    std::lock_guard<std::mutex> lock(data->mutex);
    data->vertices = std::move(vertices);
    data->originX = originX;
    data->originY = originY;
    data->revision++;
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeSetStyle(
    JNIEnv*, jclass, jlong state, jfloat halfWidth, jfloat altitudeScale) {
    auto& data = handle(state);
    std::lock_guard<std::mutex> lock(data->mutex);
    data->halfWidth = halfWidth;
    data->altitudeScale = altitudeScale;
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeSetLineVisible(
    JNIEnv*, jclass, jlong state, jboolean visible) {
    auto& data = handle(state);
    std::lock_guard<std::mutex> lock(data->mutex);
    data->lineVisible = visible == JNI_TRUE;
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeSetCursor(
    JNIEnv*, jclass, jlong state, jdouble longitude, jdouble latitude, jdouble altitude) {
    const double x = mercatorX(longitude);
    const double y = mercatorY(latitude);
    auto& data = handle(state);
    std::lock_guard<std::mutex> lock(data->mutex);
    data->cursor.visible = true;
    data->cursor.x = x;
    data->cursor.y = y;
    data->cursor.altitude = altitude;
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeClearCursor(JNIEnv*, jclass, jlong state) {
    auto& data = handle(state);
    std::lock_guard<std::mutex> lock(data->mutex);
    data->cursor.visible = false;
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeSetCursorStyle(
    JNIEnv*, jclass, jlong state, jfloat diameter, jfloat outlineWidth, jint fillColor, jint outlineColor) {
    auto& data = handle(state);
    std::lock_guard<std::mutex> lock(data->mutex);
    data->cursor.diameter = diameter;
    data->cursor.outlineWidth = outlineWidth;
    data->cursor.fill = unpackColor(fillColor);
    data->cursor.outline = unpackColor(outlineColor);
}

} // extern "C"
