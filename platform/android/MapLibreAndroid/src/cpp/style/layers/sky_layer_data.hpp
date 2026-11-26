#pragma once

#include <mln/style/layers/custom_layer_host.hpp>
#include <mln/style/layers/custom_layer_render_parameters.hpp>

#include "custom_layer_common.hpp"

#include <array>
#include <cmath>
#include <memory>

namespace cumbly_sky {

struct SkyColors {
    std::array<float, 3> haze;
    std::array<float, 3> light;
    std::array<float, 3> zenith;
    float lightDegrees;
    float zenithDegrees;
};

// Normalised device position of the map centre: the point on the camera's optical axis, which
// the edge insets move away from the middle of the screen.
inline std::array<float, 2> perspectiveCentre(const mln::style::CustomLayerRenderParameters& parameters) {
    const double worldSize = cumbly_common::kTileSize * std::pow(2.0, parameters.zoom);
    const double x = cumbly_common::mercatorX(parameters.longitude) * worldSize;
    const double y = cumbly_common::mercatorY(parameters.latitude) * worldSize;
    const auto& m = parameters.projectionMatrix;
    const double w = m[3] * x + m[7] * y + m[15];
    if (std::abs(w) < 1e-9) return {0.0f, 0.0f};
    return {static_cast<float>((m[0] * x + m[4] * y + m[12]) / w),
            static_cast<float>((m[1] * x + m[5] * y + m[13]) / w)};
}

std::unique_ptr<mln::style::CustomLayerHost> createHost(SkyColors colors);

} // namespace cumbly_sky
