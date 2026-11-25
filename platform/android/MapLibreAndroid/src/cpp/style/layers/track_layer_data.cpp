#include "track_layer_data.hpp"

#include "custom_layer_common.hpp"

#include <cstring>
#include <utility>

namespace cumbly_track {

namespace {

void put(std::vector<float>& out, double x, double y, double z) {
    out.push_back(static_cast<float>(x));
    out.push_back(static_cast<float>(y));
    out.push_back(static_cast<float>(z));
}

} // namespace

std::array<float, 4> unpackColor(uint32_t argb) {
    return {static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
            static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
            static_cast<float>(argb & 0xFF) / 255.0f,
            static_cast<float>((argb >> 24) & 0xFF) / 255.0f};
}

void setTrack(TrackData& data, const double* lonLatAltitudes, const uint32_t* argb, size_t count) {
    std::vector<double> xs(count);
    std::vector<double> ys(count);
    for (size_t i = 0; i < count; ++i) {
        xs[i] = cumbly_common::mercatorX(lonLatAltitudes[i * 3]);
        ys[i] = cumbly_common::mercatorY(lonLatAltitudes[i * 3 + 1]);
    }
    const double originX = count > 0 ? xs[0] : 0.0;
    const double originY = count > 0 ? ys[0] : 0.0;

    std::vector<float> vertices;
    vertices.reserve(count * 2 * kFloatsPerVertex);
    for (size_t i = 0; i < count; ++i) {
        const size_t previous = i > 0 ? i - 1 : i;
        const size_t next = i + 1 < count ? i + 1 : i;
        const uint32_t color = argb[i];
        const std::array<uint8_t, 4> rgba = {static_cast<uint8_t>((color >> 16) & 0xFF),
                                             static_cast<uint8_t>((color >> 8) & 0xFF),
                                             static_cast<uint8_t>(color & 0xFF),
                                             static_cast<uint8_t>((color >> 24) & 0xFF)};
        float packedColor = 0.0f;
        static_assert(sizeof(packedColor) == sizeof(rgba));
        std::memcpy(&packedColor, rgba.data(), sizeof(packedColor));
        for (const float side : {-1.0f, 1.0f}) {
            put(vertices, xs[i] - originX, ys[i] - originY, lonLatAltitudes[i * 3 + 2]);
            put(vertices,
                xs[previous] - originX,
                ys[previous] - originY,
                lonLatAltitudes[previous * 3 + 2]);
            put(vertices, xs[next] - originX, ys[next] - originY, lonLatAltitudes[next * 3 + 2]);
            vertices.push_back(side);
            vertices.push_back(packedColor);
        }
    }

    std::lock_guard<std::mutex> lock(data.mutex);
    data.vertices = std::move(vertices);
    data.originX = originX;
    data.originY = originY;
    data.revision++;
}

void setCursor(TrackData& data, double longitude, double latitude, double altitude) {
    const double x = cumbly_common::mercatorX(longitude);
    const double y = cumbly_common::mercatorY(latitude);
    std::lock_guard<std::mutex> lock(data.mutex);
    data.cursor.visible = true;
    data.cursor.x = x;
    data.cursor.y = y;
    data.cursor.altitude = altitude;
}

} // namespace cumbly_track
