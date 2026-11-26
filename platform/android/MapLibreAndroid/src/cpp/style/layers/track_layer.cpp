#include <jni.h>

#include <mln/style/layers/custom_layer.hpp>

#include "track_layer_data.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace {

std::shared_ptr<cumbly_track::TrackData>& handle(jlong state) {
    return *reinterpret_cast<std::shared_ptr<cumbly_track::TrackData>*>(state);
}

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeCreateState(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(
        new std::shared_ptr<cumbly_track::TrackData>(std::make_shared<cumbly_track::TrackData>()));
}

JNIEXPORT jlong JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeCreateHost(JNIEnv*,
                                                                                           jclass,
                                                                                           jlong state) {
    return reinterpret_cast<jlong>(cumbly_track::createHost(handle(state)).release());
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeReleaseState(JNIEnv*,
                                                                                            jclass,
                                                                                            jlong state) {
    delete reinterpret_cast<std::shared_ptr<cumbly_track::TrackData>*>(state);
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeSetTrack(
    JNIEnv* env, jclass, jlong state, jdoubleArray coordinates, jintArray colors) {
    const jsize pointCount = env->GetArrayLength(colors);
    if (env->GetArrayLength(coordinates) < pointCount * 3) return;
    std::vector<double> lonLatAlt(static_cast<size_t>(pointCount) * 3);
    std::vector<uint32_t> argb(static_cast<size_t>(pointCount));
    static_assert(sizeof(jint) == sizeof(uint32_t));
    env->GetDoubleArrayRegion(coordinates, 0, pointCount * 3, lonLatAlt.data());
    env->GetIntArrayRegion(colors, 0, pointCount, reinterpret_cast<jint*>(argb.data()));
    cumbly_track::setTrack(*handle(state), lonLatAlt.data(), argb.data(), static_cast<size_t>(pointCount));
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeSetStyle(
    JNIEnv*, jclass, jlong state, jfloat halfWidth, jfloat altitudeScale) {
    auto& data = handle(state);
    std::lock_guard<std::mutex> lock(data->mutex);
    data->halfWidth = halfWidth;
    data->altitudeScale = altitudeScale;
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeSetLineVisible(JNIEnv*,
                                                                                              jclass,
                                                                                              jlong state,
                                                                                              jboolean visible) {
    auto& data = handle(state);
    std::lock_guard<std::mutex> lock(data->mutex);
    data->lineVisible = visible == JNI_TRUE;
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeSetCursor(
    JNIEnv*, jclass, jlong state, jdouble longitude, jdouble latitude, jdouble altitude) {
    cumbly_track::setCursor(*handle(state), longitude, latitude, altitude);
}

JNIEXPORT void JNICALL Java_org_maplibre_android_style_layers_TrackLayer_nativeClearCursor(JNIEnv*,
                                                                                           jclass,
                                                                                           jlong state) {
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
    data->cursor.fill = cumbly_track::unpackColor(static_cast<uint32_t>(fillColor));
    data->cursor.outline = cumbly_track::unpackColor(static_cast<uint32_t>(outlineColor));
}

} // extern "C"
