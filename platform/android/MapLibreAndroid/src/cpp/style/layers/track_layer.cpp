#include <jni.h>

#include <mln/style/layers/custom_layer.hpp>

#include "custom_layer_common.hpp"
#include "track_layer_data.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace {

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
    return reinterpret_cast<jlong>(new std::shared_ptr<cumbly_track::TrackData>(std::make_shared<cumbly_track::TrackData>()));
}

JNIEXPORT jlong JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeCreateHost(JNIEnv*, jclass, jlong state) {
    return reinterpret_cast<jlong>(cumbly_track::createHost(cumbly_gl::handle<cumbly_track::TrackData>(state)).release());
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeReleaseState(JNIEnv*, jclass, jlong state) {
    delete reinterpret_cast<std::shared_ptr<cumbly_track::TrackData>*>(state);
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
        xs[i] = cumbly_gl::mercatorX(lonLatAlt[i * 3]);
        ys[i] = cumbly_gl::mercatorY(lonLatAlt[i * 3 + 1]);
    }
    const double originX = pointCount > 0 ? xs[0] : 0.0;
    const double originY = pointCount > 0 ? ys[0] : 0.0;

    std::vector<float> vertices;
    vertices.reserve(static_cast<size_t>(pointCount) * 2 * cumbly_track::kFloatsPerVertex);
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

    auto& data = cumbly_gl::handle<cumbly_track::TrackData>(state);
    std::lock_guard<std::mutex> lock(data->mutex);
    data->vertices = std::move(vertices);
    data->originX = originX;
    data->originY = originY;
    data->revision++;
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeSetStyle(
    JNIEnv*, jclass, jlong state, jfloat halfWidth, jfloat altitudeScale) {
    auto& data = cumbly_gl::handle<cumbly_track::TrackData>(state);
    std::lock_guard<std::mutex> lock(data->mutex);
    data->halfWidth = halfWidth;
    data->altitudeScale = altitudeScale;
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeSetLineVisible(
    JNIEnv*, jclass, jlong state, jboolean visible) {
    auto& data = cumbly_gl::handle<cumbly_track::TrackData>(state);
    std::lock_guard<std::mutex> lock(data->mutex);
    data->lineVisible = visible == JNI_TRUE;
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeSetCursor(
    JNIEnv*, jclass, jlong state, jdouble longitude, jdouble latitude, jdouble altitude) {
    const double x = cumbly_gl::mercatorX(longitude);
    const double y = cumbly_gl::mercatorY(latitude);
    auto& data = cumbly_gl::handle<cumbly_track::TrackData>(state);
    std::lock_guard<std::mutex> lock(data->mutex);
    data->cursor.visible = true;
    data->cursor.x = x;
    data->cursor.y = y;
    data->cursor.altitude = altitude;
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeClearCursor(JNIEnv*, jclass, jlong state) {
    auto& data = cumbly_gl::handle<cumbly_track::TrackData>(state);
    std::lock_guard<std::mutex> lock(data->mutex);
    data->cursor.visible = false;
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeSetCursorStyle(
    JNIEnv*, jclass, jlong state, jfloat diameter, jfloat outlineWidth, jint fillColor, jint outlineColor) {
    auto& data = cumbly_gl::handle<cumbly_track::TrackData>(state);
    std::lock_guard<std::mutex> lock(data->mutex);
    data->cursor.diameter = diameter;
    data->cursor.outlineWidth = outlineWidth;
    data->cursor.fill = unpackColor(fillColor);
    data->cursor.outline = unpackColor(outlineColor);
}

} // extern "C"
