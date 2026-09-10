// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ovmesh/survey.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

namespace ovmesh {

struct GeographicPoint { double x = 0, y = 0; };

inline bool geographic_position_valid(const PositionFix& fix) {
    return fix.valid && std::isfinite(fix.latitude) && std::isfinite(fix.longitude) &&
        std::abs(fix.latitude) <= 90 && std::abs(fix.longitude) <= 180;
}

// A local, spherical equirectangular display projection, not a distance survey
// or a GPS accuracy estimate. Both axes use the same metres-per-pixel scale.
// Every valid fix influences the bounds; this helper never removes outliers,
// smooths measurements, or rewrites the supplied receiver positions.
class GeographicPlot {
public:
    static constexpr double metres_per_degree = 6371008.8 * std::numbers::pi / 180;
    static constexpr double minimum_short_span_metres = 100;

    size_t count = 0;
    bool drawable = false;
    double width = 0, height = 0;
    double reference_latitude = 0, reference_longitude = 0;
    double mean_latitude = 0, mean_longitude = 0;
    double data_east_west_metres = 0, data_north_south_metres = 0;
    double east_west_metres = 0, north_south_metres = 0;
    double metres_per_pixel = 1, scale_bar_metres = 0;

    GeographicPlot(std::span<const PositionFix* const> fixes, double pixels_wide, double pixels_high) {
        std::vector<double> longitudes;
        longitudes.reserve(fixes.size());
        double south = 90, north = -90;
        for (const auto* fix : fixes) {
            if (!fix || !geographic_position_valid(*fix)) continue;
            ++count;
            south = std::min(south, fix->latitude);
            north = std::max(north, fix->latitude);
            mean_latitude += (fix->latitude - mean_latitude) / static_cast<double>(count);
            longitudes.push_back(wrap_positive(fix->longitude));
        }
        if (!count) return;

        // Cut the circle at its largest empty longitude gap. A survey crossing
        // +/-180 degrees then occupies a short arc instead of nearly the globe.
        std::sort(longitudes.begin(), longitudes.end());
        double largest_gap = -1, arc_start = longitudes.front();
        for (size_t i = 0; i < longitudes.size(); ++i) {
            const double next = i + 1 < longitudes.size() ? longitudes[i + 1] : longitudes.front() + 360;
            const double gap = next - longitudes[i];
            if (gap > largest_gap) {
                largest_gap = gap;
                arc_start = wrap_positive(next);
            }
        }
        const double arc_span = std::max(0.0, 360 - largest_gap);
        reference_latitude = (south + north) / 2;
        reference_longitude = wrap_signed(arc_start + arc_span / 2);
        longitude_factor_ = std::cos(reference_latitude * std::numbers::pi / 180);
        data_east_west_metres = arc_span * longitude_factor_ * metres_per_degree;
        data_north_south_metres = (north - south) * metres_per_degree;
        double mean_longitude_offset = 0;
        size_t mean_count = 0;
        for (double longitude : longitudes) {
            ++mean_count;
            const double offset = wrap_signed(longitude - reference_longitude);
            mean_longitude_offset += (offset - mean_longitude_offset) / static_cast<double>(mean_count);
        }
        mean_longitude = wrap_signed(reference_longitude + mean_longitude_offset);

        if (!std::isfinite(pixels_wide) || !std::isfinite(pixels_high) || pixels_wide <= 0 || pixels_high <= 0) return;
        width = pixels_wide;
        height = pixels_high;
        // Five percent padding on each side; even a single fixed point gets a
        // visible short dimension of at least 100 m rather than a false route.
        const double scale = std::max({minimum_short_span_metres / std::min(width, height),
            data_east_west_metres * 1.1 / width, data_north_south_metres * 1.1 / height});
        const double visible_width = width * scale, visible_height = height * scale;
        if (!std::isfinite(scale) || scale <= 0 || !std::isfinite(visible_width) || !std::isfinite(visible_height)) return;
        metres_per_pixel = scale;
        east_west_metres = visible_width;
        north_south_metres = visible_height;
        scale_bar_metres = nice_scale(visible_width / 4);
        drawable = true;
    }

    std::optional<GeographicPoint> point(const PositionFix& fix) const {
        if (!drawable || !geographic_position_valid(fix)) return {};
        return project(fix.latitude, fix.longitude);
    }

    // Mean of the reported positions, not an estimate of the true site or an
    // inferred stationary location. No receiver measurement is replaced by it.
    std::optional<GeographicPoint> mean_point() const {
        if (!drawable) return {};
        return project(mean_latitude, mean_longitude);
    }

private:
    double longitude_factor_ = 1;

    static double wrap_positive(double value) {
        const double result = std::fmod(value, 360);
        return result < 0 ? result + 360 : result;
    }
    static double wrap_signed(double value) { return wrap_positive(value + 180) - 180; }

    static double nice_scale(double maximum) {
        const double decade = std::pow(10.0, std::floor(std::log10(maximum)));
        const double significand = maximum / decade;
        return (significand >= 5 ? 5 : significand >= 2 ? 2 : 1) * decade;
    }

    GeographicPoint project(double latitude, double longitude) const {
        const double east = wrap_signed(longitude - reference_longitude) * longitude_factor_ * metres_per_degree;
        const double north = (latitude - reference_latitude) * metres_per_degree;
        return {width / 2 + east / metres_per_pixel, height / 2 - north / metres_per_pixel};
    }
};

} // namespace ovmesh
