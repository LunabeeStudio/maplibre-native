#pragma once

#include <jni.h>

#include <mln/style/layers/custom_layer_render_parameters.hpp>

#include <array>
#include <cmath>
#include <memory>

namespace cumbly_common {

constexpr double kTileSize = 512.0;

inline double mercatorX(double longitude) {
    return (longitude + 180.0) / 360.0;
}

inline double mercatorY(double latitude) {
    const double radians = latitude * M_PI / 180.0;
    return (1.0 - std::log(std::tan(radians) + 1.0 / std::cos(radians)) / M_PI) / 2.0;
}

template <typename State>
std::shared_ptr<State>& handle(jlong state) {
    return *reinterpret_cast<std::shared_ptr<State>*>(state);
}

inline std::array<float, 16> modelViewProjection(const mln::style::CustomLayerRenderParameters& parameters,
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

} // namespace cumbly_common
