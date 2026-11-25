#pragma once

#include <mln/style/layers/custom_layer_host.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace cumbly_track {

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

constexpr int kFloatsPerVertex = 11;

std::unique_ptr<mln::style::CustomLayerHost> createHost(std::shared_ptr<TrackData> data);

} // namespace cumbly_track
