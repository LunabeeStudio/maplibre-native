#include <jni.h>

#include <mln/style/layers/custom_layer.hpp>

#include "sky_layer_data.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace {

std::array<float, 3> rgb(uint32_t argb) {
    return {static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
            static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
            static_cast<float>(argb & 0xFF) / 255.0f};
}

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_org_maplibre_android_style_layers_SkyLayer_nativeCreateHost(
    JNIEnv*, jclass, jint haze, jint light, jint zenith, jfloat lightDegrees, jfloat zenithDegrees) {
    return reinterpret_cast<jlong>(cumbly_sky::createHost(cumbly_sky::SkyColors{rgb(static_cast<uint32_t>(haze)),
                                                                                rgb(static_cast<uint32_t>(light)),
                                                                                rgb(static_cast<uint32_t>(zenith)),
                                                                                lightDegrees,
                                                                                std::max(zenithDegrees, lightDegrees + 0.01f)})
                                       .release());
}

} // extern "C"
