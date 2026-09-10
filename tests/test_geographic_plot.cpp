// SPDX-License-Identifier: GPL-3.0-or-later
// All positions below are invented geometry inputs, not receiver observations.
#include "../src/geographic_plot.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace ovmesh;
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
bool near(double a, double b, double tolerance = 1e-7) { return std::abs(a - b) <= tolerance; }
PositionFix fix(double latitude, double longitude) {
    PositionFix result; result.latitude = latitude; result.longitude = longitude; result.valid = true; return result;
}
void contained(const GeographicPlot& plot, const PositionFix& position) {
    const auto p = plot.point(position);
    require(p && std::isfinite(p->x) && std::isfinite(p->y) && p->x >= 0 && p->x <= plot.width &&
        p->y >= 0 && p->y <= plot.height, "Valid input position must fit the projection without clipping");
}
void scale_bar(const GeographicPlot& plot) {
    require(plot.scale_bar_metres > 0 && plot.scale_bar_metres <= plot.east_west_metres / 4,
        "Scale bar must occupy at most a quarter of the width");
    const double decade = std::pow(10.0, std::floor(std::log10(plot.scale_bar_metres)));
    const double significand = plot.scale_bar_metres / decade;
    require(near(significand, 1) || near(significand, 2) || near(significand, 5), "Scale bar uses a 1/2/5 distance");
}
void physical_scaling() {
    for (double latitude : {0.0, 40.0, 80.0}) for (const auto geometry : {GeographicPoint{1000, 200}, GeographicPoint{200, 1000}, GeographicPoint{300, 300}}) {
        const double delta = .001;
        const double delta_lon = delta / std::cos(latitude * std::numbers::pi / 180);
        const std::array positions{fix(latitude - delta, 20), fix(latitude + delta, 20),
            fix(latitude, 20 - delta_lon), fix(latitude, 20 + delta_lon)};
        const std::array<const PositionFix*, 4> pointers{&positions[0], &positions[1], &positions[2], &positions[3]};
        const GeographicPlot plot(pointers, geometry.x, geometry.y);
        require(plot.drawable && plot.count == 4, "Valid coordinate cloud is drawable");
        const auto south = plot.point(positions[0]), north = plot.point(positions[1]);
        const auto west = plot.point(positions[2]), east = plot.point(positions[3]);
        require(near(east->x - west->x, south->y - north->y), "Equal local metre distances must have equal pixel lengths at every latitude");
        require(east->x > west->x && south->y > north->y, "Projection is east right and north up");
        require(near(plot.east_west_metres / geometry.x, plot.north_south_metres / geometry.y), "One physical scale for both screen axes");
        require(plot.east_west_metres >= plot.data_east_west_metres * 1.1 - 1e-8 &&
            plot.north_south_metres >= plot.data_north_south_metres * 1.1 - 1e-8, "Cloud has five percent padding on each side");
        for (const auto& position : positions) contained(plot, position);
        scale_bar(plot);
    }
}
void stationary_cloud() {
    auto center = fix(-30, 90);
    const std::array<const PositionFix*, 3> repeated{&center, &center, &center};
    const GeographicPlot plot(repeated, 1600, 200);
    const auto p = plot.point(center), mean = plot.mean_point();
    require(plot.drawable && plot.count == 3 && plot.data_east_west_metres == 0 && plot.data_north_south_metres == 0,
        "Repeated stationary positions have zero recorded spread");
    require(near(plot.north_south_metres, 100) && near(plot.east_west_metres, 800), "Short visible span stays 100m for a stationary receiver");
    require(p && mean && near(p->x, 800) && near(p->y, 100) && near(mean->x, p->x) && near(mean->y, p->y), "Single/repeated fix and mean sit at the center");
    auto nearby = fix(-30 + 1 / GeographicPlot::metres_per_degree, 90);
    const std::array<const PositionFix*, 2> jitter{&center, &nearby};
    const GeographicPlot small(jitter, 1600, 200);
    require(near(std::abs(small.point(center)->y - small.point(nearby)->y), 2), "One metre drift takes two pixels rather than filling the plot");
    require(near(small.mean_latitude, (center.latitude + nearby.latitude) / 2), "Reported position mean is retained explicitly");
    const std::array<const PositionFix*, 1> single{&center};
    const GeographicPlot one(single, 200, 800);
    require(one.count == 1 && one.drawable && near(one.east_west_metres, 100), "Single fix supports portrait geometry");
    scale_bar(plot); scale_bar(one);
}
void dateline_and_outliers() {
    const std::array positions{fix(40, 179.999), fix(40, -179.999), fix(40, -179.998)};
    const std::array<const PositionFix*, 3> pointers{&positions[0], &positions[1], &positions[2]};
    const GeographicPlot plot(pointers, 600, 300);
    require(plot.drawable && plot.data_east_west_metres > 200 && plot.data_east_west_metres < 300,
        "Dateline points use the short longitude arc");
    require(plot.mean_longitude < -179.99 && near(plot.mean_longitude, -179.99933333333334), "Mean longitude uses unwrapped coordinates");
    for (const auto& position : positions) contained(plot, position);
    require(plot.point(positions[0])->x < plot.point(positions[1])->x, "Dateline crossing stays ordered eastward");
    auto outlier = fix(-29, 91);
    auto local = fix(-30, 90);
    const std::array<const PositionFix*, 2> with_outlier{&outlier, &local};
    const GeographicPlot wide(with_outlier, 1000, 200);
    require(wide.count == 2 && wide.data_north_south_metres > 100000, "Valid spatial outlier remains part of the measured extent");
    contained(wide, outlier); contained(wide, local); scale_bar(wide);
    const std::array polar{fix(90, 180), fix(90, -180)};
    const std::array<const PositionFix*, 2> polar_pointers{&polar[0], &polar[1]};
    const GeographicPlot pole(polar_pointers, 300, 200);
    require(pole.drawable && near(pole.data_east_west_metres, 0), "Equivalent pole/dateline endpoints do not invent a global span");
    contained(pole, polar[0]); contained(pole, polar[1]);
}
void invalid_inputs() {
    const GeographicPlot empty({}, 200, 200);
    require(!empty.drawable && empty.count == 0 && !empty.mean_point(), "Empty projection has no location");
    auto good = fix(0, 0), invalid = fix(0, 0), nan = fix(std::numeric_limits<double>::quiet_NaN(), 1);
    invalid.valid = false;
    auto outside_latitude = fix(91, 0), outside_longitude = fix(0, 181);
    const std::array<const PositionFix*, 6> mixed{&good, &invalid, &nan, &outside_latitude, &outside_longitude, nullptr};
    const GeographicPlot plot(mixed, 300, 200);
    require(plot.drawable && plot.count == 1 && !plot.point(invalid) && !plot.point(nan) && !plot.point(outside_latitude) && !plot.point(outside_longitude), "Only valid finite geographic coordinates can be plotted");
    for (double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        const GeographicPlot bad_width(mixed, bad, 200), bad_height(mixed, 200, bad);
        require(!bad_width.drawable && !bad_height.drawable && !bad_width.point(good) && !bad_height.mean_point(), "Degenerate viewport does not produce coordinates");
    }
    const GeographicPlot tiny(mixed, .01, .02);
    require(tiny.drawable && near(tiny.east_west_metres, 100), "Positive tiny viewports remain finite");
    const GeographicPlot overflow(mixed, std::numeric_limits<double>::max(), std::numeric_limits<double>::min());
    require(!overflow.drawable && std::isfinite(overflow.metres_per_pixel), "Unrepresentable viewport ratios fail safely");
}
}

int main() {
    try { physical_scaling(); stationary_cloud(); dateline_and_outliers(); invalid_inputs(); std::cout << "Geographic projection checks passed\n"; return EXIT_SUCCESS; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
