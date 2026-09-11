// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/report.hpp"
#include "storage.hpp"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace ovmesh {
namespace {
constexpr size_t row_limit = 1000000;
// A std::map node plus the fixed accumulator is < 512 bytes on supported ABIs.
// This bound keeps aggregation below 100 MiB plus one validated source tile.
constexpr size_t aggregate_limit = 200000;
constexpr double earth_radius_m = 6371008.8;
constexpr double radians = std::numbers::pi / 180;

std::string number(double value) {
    if (!std::isfinite(value)) throw std::runtime_error("Non-finite report value");
    std::ostringstream out; out.imbue(std::locale::classic());
    out << std::setprecision(15) << value; return out.str();
}
std::string integer(uint64_t value) { return std::to_string(value); }
std::string text(const std::string& value) { return csv_text(value); }
template<class T> std::string optional_number(const std::optional<T>& value) {
    return value ? number(double(*value)) : std::string();
}
double power(double dbfs) { return std::pow(10., dbfs / 10.); }
std::string db(double p) { return p > 0 ? number(10 * std::log10(p)) : std::string(); }
std::string percent(double numerator, double denominator) {
    return denominator > 0 ? number(100 * std::clamp(numerator / denominator, 0., 1.)) : std::string();
}
double rounded(double value, unsigned decimals) {
    const double scale = std::pow(10., double(decimals)); return std::round(value * scale) / scale;
}
std::string optional_coordinate(const std::optional<double>& value,unsigned decimals) {
    return value?number(rounded(*value,decimals)):std::string();
}
struct Csv {
    const std::function<void(std::string_view)>& emit;
    size_t rows = 0, columns = 0;
    void header(const std::vector<std::string>& cells) {
        columns = cells.size(); write(cells);
    }
    void write(const std::vector<std::string>& cells) {
        if (cells.size() != columns) throw std::runtime_error("Internal report column mismatch");
        if (rows++ > row_limit) throw std::runtime_error("Report exceeds one million rows; narrow the selection or use larger time/geographic groups");
        std::string line;
        for (size_t i=0;i<cells.size();++i) { if(i)line+=',';line+=cells[i]; }
        line += '\n'; emit(line);
    }
};
void append(std::vector<std::string>& to, const std::vector<std::string>& from) {
    to.insert(to.end(), from.begin(), from.end());
}
bool has_position(const std::optional<PositionFix>& position) { return position && position->valid; }
bool in_region(const std::optional<PositionFix>& position, const SurveyQuery& query) {
    return !query.geographic_filter || (has_position(position) &&
        position->latitude >= query.south && position->latitude <= query.north &&
        position->longitude >= query.west && position->longitude <= query.east);
}
std::string quality_name(uint32_t value) {
    std::string result;
    for (const auto& [flag, name] : {
        std::pair{SurveyUncalibrated, "uncalibrated"}, {SurveyUpstreamLossUnknown, "upstream_loss_unknown"},
        {SurveyClipped, "clipped"}, {SurveyPositionMissing, "position_missing"},
        {SurveyBoundary, "measurement_boundary"}, {SurveyTruncated, "truncated"},
        {SurveyMerged, "merged"}, {SurveyBackgroundUncertain, "background_uncertain"},
        {SurveyDcSuspect, "possible_receiver_center_artifact"}, {SurveyPowerAggregated, "power_time_aggregated"}}) {
        if (value & flag) { if (!result.empty()) result += ';'; result += name; }
    }
    return text(result);
}

struct Context {
    const ReportOptions& options;
    Snapshot summary;
    int schema = 0;
    double lower = 0, upper = 0, end = 0;
    explicit Context(const ReportOptions& o, Snapshot s, int version) : options(o), summary(std::move(s)), schema(version) {
        const auto& q = o.query;
        for (double bound : {q.lower_hz,q.upper_hz,q.elapsed_start,q.elapsed_end})
            if (!std::isfinite(bound) || bound < 0 || bound > 1e10) throw std::runtime_error("Invalid report selection bound");
        lower = q.lower_hz > 0 ? q.lower_hz : double(summary.config.center_hz) - summary.config.survey_span_hz / 2.;
        upper = q.upper_hz > 0 ? q.upper_hz : double(summary.config.center_hz) + summary.config.survey_span_hz / 2.;
        end = q.elapsed_end > 0 ? q.elapsed_end : 1e10;
        if (lower >= upper || end <= q.elapsed_start) throw std::runtime_error("Empty or reversed report selection");
        if (q.geographic_filter && (!std::isfinite(q.south) || !std::isfinite(q.north) ||
            !std::isfinite(q.west) || !std::isfinite(q.east) || q.south < -90 || q.north > 90 ||
            q.west < -180 || q.east > 180 || q.south > q.north || q.west > q.east))
            throw std::runtime_error("Invalid geographic rectangle; split a selection crossing the date line");
        if (o.privacy.coordinate_decimals > 7) throw std::runtime_error("Coordinate decimal places must be 0 through 7");
        if (o.kind == ReportKind::TimeSummary && (!std::isfinite(q.time_bucket_seconds) ||
            q.time_bucket_seconds < .001 || q.time_bucket_seconds > 1e10))
            throw std::runtime_error("Report time groups must be at least one millisecond");
        if (o.kind == ReportKind::GeographicSummary && (!std::isfinite(o.geographic_cell_m) ||
            o.geographic_cell_m < 10 || o.geographic_cell_m > 10000))
            throw std::runtime_error("Geographic cell size must be between 10 and 10000 metres");
        if ((o.kind == ReportKind::GeographicSummary || o.kind == ReportKind::ReceiverTrack) && !o.privacy.include_receiver_positions)
            throw std::runtime_error("This report requires receiver GPS export to be enabled; geographic grouping is not anonymization");
        if (o.kind == ReportKind::AuthorizedContent && !o.privacy.include_content)
            throw std::runtime_error("Authorized-content export must be explicitly enabled");
        if (o.kind == ReportKind::Waveforms && schema < 5)
            throw std::runtime_error("Waveform observations are unavailable in this legacy recording format");
    }
    std::vector<std::string> metadata_header() const {
        std::vector<std::string> result{"session_id","recording_schema_version","source","sample_rate_hz","receiver_center_hz","offset_hz",
            "lna_gain_db","vga_gain_db","rf_amplifier","threshold_dbfs","fft_bin_width_hz","hann_enbw_hz",
            "recording_incomplete","geographic_filter_applied","session_dropped_samples","requested_lower_hz","requested_upper_hz",
            "rtl_tuner_gain_db","rtl_auto_gain"};
        if (options.privacy.include_provenance) append(result,{"antenna_description","receiver_description","survey_notes"});
        return result;
    }
    std::vector<std::string> metadata() const {
        const auto& c = summary.config;
        const bool rtl=!c.synthetic&&c.hardware_receiver==HardwareReceiver::RtlSdr;
        const bool rak=!c.synthetic&&c.hardware_receiver==HardwareReceiver::Rak5146;
        std::vector<std::string> result{text(summary.session_id),std::to_string(schema),text(c.synthetic ? "synthetic" : receiver_source_name(c)),
            rak?"":integer(c.sample_rate),integer(c.center_hz),schema>=2?number(double(c.tuning_offset_hz)):"",(rtl||rak)?"":integer(c.lna_gain),
            (rtl||rak)?"":integer(c.vga_gain),(rtl||rak)?"":integer(c.amplifier),rak?"":number(c.activity_threshold_dbfs),(schema>=4&&!rak)?number(summary.spectrum_bin_width_hz):"",
            (schema>=4&&!rak)?number(summary.spectrum_enbw_hz):"",integer(summary.incomplete),integer(options.query.geographic_filter),integer(summary.dropped_samples),number(lower),number(upper),
            rtl&&!c.rtl_auto_gain?number(c.rtl_gain_tenths_db/10.0):"",rtl?integer(c.rtl_auto_gain):""};
        if (options.privacy.include_provenance) append(result,{text(c.antenna_description),text(c.receiver_description),text(c.survey_notes)});
        return result;
    }
    bool selected(double frequency, double width, double elapsed, const std::optional<PositionFix>& fix) const {
        return frequency + width / 2 > lower && frequency - width / 2 < upper &&
            elapsed >= options.query.elapsed_start && elapsed < end && in_region(fix, options.query);
    }
};

struct Bounds {
    bool any = false;
    double south = 0, north = 0, west = 0, east = 0;
    void add(const std::optional<PositionFix>& fix) {
        if (!has_position(fix)) return;
        if (!any) { any = true; south = north = fix->latitude; west = east = fix->longitude; }
        else { south = std::min(south,fix->latitude); north = std::max(north,fix->latitude);
            west = std::min(west,fix->longitude); east = std::max(east,fix->longitude); }
    }
    void merge(const Bounds& b) {
        if (!b.any) return;
        if (!any) { *this = b; return; }
        south = std::min(south,b.south); north = std::max(north,b.north);
        west = std::min(west,b.west); east = std::max(east,b.east);
    }
    std::vector<std::string> fields(unsigned decimals) const {
        if (!any) return {"","","",""};
        return {number(rounded(south,decimals)),number(rounded(north,decimals)),
            number(rounded(west,decimals)),number(rounded(east,decimals))};
    }
};
struct Accumulator {
    double observed = 0, busy = 0, outside_busy = 0, mean_power = 0, peak_power = 0, noise_power = 0;
    double missing_position = 0, power_support_max = 0, partial_power = 0, boundary_position = 0;
    uint32_t quality = 0;
    Bounds positions;
    void add(double seconds, double mean, double peak, double noise, const SpectrumTile& tile, bool partial) {
        observed += seconds; mean_power += mean * seconds; peak_power = std::max(peak_power,peak);
        noise_power += noise * seconds; quality |= tile.quality;
        const double support = tile.power_elapsed_end > tile.power_elapsed_start ?
            tile.power_elapsed_end - tile.power_elapsed_start : tile.elapsed_end_seconds - tile.elapsed_start_seconds;
        power_support_max = std::max(power_support_max,support);
        if (partial) { partial_power += seconds; quality |= SurveyBoundary; }
        if (!has_position(tile.receiver_end)) { missing_position += seconds; quality |= SurveyPositionMissing; }
        positions.add(tile.receiver_start); positions.add(tile.receiver_end);
    }
};
static_assert(sizeof(Accumulator) < 256);
struct Cell {
    int64_t row = -1, column = -1;
    double south = 0, north = 0, west = 0, east = 0;
    bool located() const { return row >= 0; }
};
Cell cell_for(const std::optional<PositionFix>& fix, double metres) {
    if (!has_position(fix)) return {};
    const double latitude_step = metres / (earth_radius_m * radians);
    const int64_t bands = int64_t(std::ceil(180 / latitude_step));
    Cell result; result.row = std::clamp<int64_t>(int64_t(std::floor((fix->latitude + 90) / latitude_step)),0,bands-1);
    result.south = -90 + double(result.row) * latitude_step;
    result.north = std::min(90.,result.south + latitude_step);
    const double circumference = 2 * std::numbers::pi * earth_radius_m * std::cos((result.south + result.north) / 2 * radians);
    const int64_t columns = std::max<int64_t>(1,int64_t(std::ceil(circumference / metres)));
    const double longitude = fix->longitude == 180 ? -180 : fix->longitude;
    result.column = std::clamp<int64_t>(int64_t(std::floor((longitude + 180) / 360 * double(columns))),0,columns-1);
    result.west = -180 + double(result.column) * 360 / double(columns);
    result.east = -180 + double(result.column+1) * 360 / double(columns);
    return result;
}
struct GeoBin { double busy = 0, power_sum = 0, peak = 0; };
struct GeoValue { Cell cell; Accumulator common; std::vector<GeoBin> bins; };
using GeoKey = std::pair<int64_t,int64_t>;

struct AggregateReport {
    const Context& c;
    explicit AggregateReport(const Context& context) : c(context) {}
    std::vector<Accumulator> bins;
    std::map<uint64_t,Accumulator> times;
    std::map<GeoKey,GeoValue> cells;
    size_t first = 0, last = 0, count = 0;
    double grid_first = 0, width = 0, earliest = 1e10, latest = 0, rejected_gps = 0, explicit_gaps = 0, utc_anchor = 0;
    uint64_t previous_sample = 0;
    double previous_elapsed = 0;
    bool grid = false;
    double covered_lower() const { return grid_first + (double(first)-.5)*width; }
    double covered_upper() const { return grid_first + (double(last)-.5)*width; }
    uint64_t bucket(double time) const {
        const double relative = (time-c.options.query.elapsed_start)/c.options.query.time_bucket_seconds;
        if (relative < 0 || relative >= double(row_limit)) throw std::runtime_error("Time report exceeds one million groups; choose a wider time group or narrower selection");
        return uint64_t(std::floor(relative));
    }
    double edge(uint64_t index) const {
        return std::fma(double(index),c.options.query.time_bucket_seconds,c.options.query.elapsed_start);
    }
    template<class F> void split_time(double from, double to, F consume) {
        while (from < to) {
            auto index = bucket(from);
            while (index && edge(index) > from) --index;
            while (edge(index+1) <= from) ++index;
            const double end = std::min(to,edge(index+1));
            if (end <= from) throw std::runtime_error("Unrepresentable report time bucket");
            consume(index,from,end); from = end;
        }
    }
    Accumulator& time_value(uint64_t index) {
        auto found=times.find(index);
        if (found != times.end()) return found->second;
        if (times.size() >= aggregate_limit) throw std::runtime_error("Time report exceeds bounded memory; use larger time groups or a shorter selection");
        return times[index];
    }
    void consume(const SpectrumTile& tile) {
        if (tile.first_sample < previous_sample || tile.elapsed_start_seconds < previous_elapsed - 1e-8)
            throw std::runtime_error("Overlapping or unordered spectrum measurements");
        previous_sample=tile.end_sample; previous_elapsed=tile.elapsed_end_seconds;
        if (!grid) {
            grid=true; count=tile.mean_dbfs.size(); grid_first=tile.first_center_hz; width=tile.bin_width_hz;
            utc_anchor=tile.utc_start_seconds-tile.elapsed_start_seconds;
            while (first<count && grid_first+(double(first)+.5)*width<=c.lower) ++first;
            last=first; while (last<count && grid_first+(double(last)-.5)*width<c.upper) ++last;
            bins.resize(last-first);
        } else if (count!=tile.mean_dbfs.size() || std::abs(grid_first-tile.first_center_hz)>1e-5 || std::abs(width-tile.bin_width_hz)>1e-8)
            throw std::runtime_error("Frequency grid changed within the saved survey");
        const double from=std::max(c.options.query.elapsed_start,tile.elapsed_start_seconds), to=std::min(c.end,tile.elapsed_end_seconds);
        if (to<=from || first==last) return;
        earliest=std::min(earliest,from); latest=std::max(latest,to);
        if (!in_region(tile.receiver_end,c.options.query)) { if(!has_position(tile.receiver_end))rejected_gps+=to-from; return; }
        const double support_from=tile.power_elapsed_end>tile.power_elapsed_start?tile.power_elapsed_start:tile.elapsed_start_seconds;
        const double support_to=tile.power_elapsed_end>tile.power_elapsed_start?tile.power_elapsed_end:tile.elapsed_end_seconds;
        const bool partial=support_from<c.options.query.elapsed_start-1e-9 || support_to>c.end+1e-9 ||
            (c.options.query.geographic_filter && (tile.quality&SurveyPowerAggregated));
        double mean=0,peak=0; const double background=power(tile.background_dbfs);
        GeoValue* geo=nullptr;
        const bool geographic=c.options.kind==ReportKind::GeographicSummary;
        Cell end_cell,start_cell; bool cell_boundary=false;
        if(geographic) {
            end_cell=cell_for(tile.receiver_end,c.options.geographic_cell_m);
            start_cell=cell_for(tile.receiver_start,c.options.geographic_cell_m);
            cell_boundary=end_cell.row!=start_cell.row || end_cell.column!=start_cell.column;
            // Attribution follows the retained tile-end receiver fix. A crossing
            // is exposed explicitly; no interpolated trajectory is invented.
            const GeoKey key{end_cell.row,end_cell.column};auto found=cells.find(key);
            if(found==cells.end()) {
                if(cells.size()>=aggregate_limit || (cells.size()+1)*(last-first)>row_limit)
                    throw std::runtime_error("Geographic report exceeds row or memory bounds; use larger cells or narrow the frequency/time selection");
                found=cells.emplace(key,GeoValue{end_cell,{},std::vector<GeoBin>(last-first)}).first;
            }
            geo=&found->second;
            geo->common.add(to-from,0,0,background,tile,partial || bool(tile.quality&SurveyPowerAggregated));
            if(cell_boundary){geo->common.boundary_position+=to-from;geo->common.quality|=SurveyBoundary;}
        }
        for(size_t b=first;b<last;++b) {
            const double bin_mean=power(tile.mean_dbfs[b]),bin_peak=power(tile.peak_dbfs[b]);
            mean+=bin_mean;peak+=bin_peak;
            if(c.options.kind==ReportKind::FrequencySummary) bins[b-first].add(to-from,bin_mean,bin_peak,background,tile,partial);
            if(geographic) {
                auto& value=geo->bins[b-first];value.power_sum+=bin_mean*(to-from);value.peak=std::max(value.peak,bin_peak);
            }
        }
        if(c.options.kind==ReportKind::TimeSummary) split_time(from,to,[&](uint64_t index,double a,double b){
            const bool bucket_partial=partial || support_from<edge(index)-1e-9 || support_to>edge(index+1)+1e-9;
            time_value(index).add(b-a,mean,peak,background*double(last-first),tile,bucket_partial);
        });
        const size_t stride=(count+7)/8; const double frame_seconds=double(tile.fft_size)/c.summary.config.sample_rate;
        for(size_t f=0;f<tile.frame_count;++f) {
            const double a=std::max(from,tile.elapsed_start_seconds+double(f)*frame_seconds);
            const double b=std::min(to,tile.elapsed_start_seconds+double(f+1)*frame_seconds);
            if(b<=a)continue;
            bool busy=false,outside=false;
            for(size_t byte=first/8;byte<=(last-1)/8;++byte) {
                unsigned mask=tile.activity[f*stride+byte];
                if(byte==first/8)mask&=0xffu<<(first%8);
                if(byte==(last-1)/8 && last%8)mask&=(1u<<(last%8))-1;
                while(mask) {
                    const unsigned bit=unsigned(std::countr_zero(mask)); const size_t bin=byte*8+bit;
                    busy=true; const bool outside_center=std::abs(grid_first+double(bin)*width-double(c.summary.config.center_hz))>2*width+1e-5;
                    outside|=outside_center;
                    if(c.options.kind==ReportKind::FrequencySummary) {
                        bins[bin-first].busy+=b-a;
                        if(outside_center)bins[bin-first].outside_busy+=b-a;
                    }
                    if(geographic)geo->bins[bin-first].busy+=b-a;
                    mask&=mask-1;
                }
            }
            if(c.options.kind==ReportKind::TimeSummary) split_time(a,b,[&](uint64_t index,double x,double y){
                auto& value=time_value(index);if(busy)value.busy+=y-x;if(outside)value.outside_busy+=y-x;
            });
        }
    }
    void gap(const CoverageGap& gap) {
        const double from=std::max(c.options.query.elapsed_start,gap.elapsed_start_seconds),to=std::min(c.end,gap.elapsed_end_seconds);
        if(to<=from)return;
        earliest=std::min(earliest,from);latest=std::max(latest,to);
        // Gaps have no position. Their applicability to a geographic rectangle
        // cannot be inferred; keep a separate all-location gap total.
        explicit_gaps+=to-from;
    }
    std::vector<std::string> base_header() const {
        std::vector<std::string> h{"lower_hz","upper_hz","elapsed_start_s","elapsed_end_s","utc_start_s","utc_end_s","observed_s","busy_s","occupancy_pct",
            "outside_receiver_center_busy_s","outside_receiver_center_occupancy_pct","mean_dbfs","peak_envelope_dbfs",
            "estimated_background_dbfs","max_power_support_s","partial_power_support_s","missing_receiver_position_s",
            "quality_flags","quality","activity_method","power_method","selection_all_location_gap_s","excluded_missing_gps_s"};
        if(c.options.privacy.include_receiver_positions)append(h,{"reported_south","reported_north","reported_west","reported_east"});
        append(h,c.metadata_header());return h;
    }
    std::vector<std::string> base(const Accumulator& a,double low,double high,double from,double to,bool union_power) const {
        const bool outside_available=low< double(c.summary.config.center_hz)-2.5*width-1e-5 || high>double(c.summary.config.center_hz)+2.5*width+1e-5;
        std::vector<std::string> row{number(low),number(high),number(from),number(to),number(utc_anchor+from),number(utc_anchor+to),number(a.observed),number(a.busy),percent(a.busy,a.observed),
            outside_available?number(a.outside_busy):"",outside_available?percent(a.outside_busy,a.observed):"",a.observed>0?db(a.mean_power/a.observed):"",db(a.peak_power),
            a.observed>0?db(a.noise_power/a.observed):"",number(a.power_support_max),number(a.partial_power),number(a.missing_position),
            integer(a.quality),quality_name(a.quality),text("fixed-threshold FFT-frame activity; union over selected bins; RF identity unknown"),
            text(union_power?"linear summed bin power; peak is non-simultaneous envelope upper bound":"linear time-weighted bin power; peak over retained power support"),
            number(explicit_gaps),number(rejected_gps)};
        if(c.options.privacy.include_receiver_positions)append(row,a.positions.fields(c.options.privacy.coordinate_decimals));
        append(row,c.metadata());return row;
    }
    void write(Csv& csv) const {
        if(!grid || first==last || latest<=earliest)throw std::runtime_error("No fine spectrum history intersects this selection");
        auto header=base_header();
        if(c.options.kind==ReportKind::GeographicSummary) {
            const std::vector<std::string> geo_header{"location_group","grid_cell_id","grid_cell_size_m","cell_south","cell_north","cell_west","cell_east","cell_boundary_attribution_s"};
            header.insert(header.begin(),geo_header.begin(),geo_header.end());csv.header(header);
            for(const auto& [key,item]:cells) {
                (void)key;const auto& cell=item.cell;
                for(size_t bin=first;bin<last;++bin) {
                    auto value=item.common;const auto& powers=item.bins[bin-first];
                    value.mean_power=powers.power_sum;value.peak_power=powers.peak;value.busy=powers.busy;
                    if(std::abs(grid_first+double(bin)*width-double(c.summary.config.center_hz))>2*width+1e-5)value.outside_busy=value.busy;
                    std::vector<std::string> row{text(cell.located()?"receiver_tile_end_cell":"unlocated"),
                        cell.located()?text(std::to_string(cell.row)+":"+std::to_string(cell.column)):"",number(c.options.geographic_cell_m),
                        cell.located()?number(rounded(cell.south,c.options.privacy.coordinate_decimals)):"",
                        cell.located()?number(rounded(cell.north,c.options.privacy.coordinate_decimals)):"",
                        cell.located()?number(rounded(cell.west,c.options.privacy.coordinate_decimals)):"",
                        cell.located()?number(rounded(cell.east,c.options.privacy.coordinate_decimals)):"",number(value.boundary_position)};
                    append(row,base(value,grid_first+(double(bin)-.5)*width,grid_first+(double(bin)+.5)*width,earliest,latest,false));csv.write(row);
                }
            }
        } else if(c.options.kind==ReportKind::FrequencySummary) {
            csv.header(header);
            for(size_t b=first;b<last;++b)csv.write(base(bins[b-first],grid_first+(double(b)-.5)*width,grid_first+(double(b)+.5)*width,earliest,latest,false));
        } else {
            csv.header(header);
            const uint64_t last_bucket=bucket(std::nextafter(latest,-std::numeric_limits<double>::infinity()));
            const uint64_t first_bucket=bucket(earliest);
            const Accumulator empty;
            for(uint64_t i=first_bucket;i<=last_bucket;++i) {
                const auto found=times.find(i);
                csv.write(base(found==times.end()?empty:found->second,covered_lower(),covered_upper(),std::max(earliest,edge(i)),std::min(latest,edge(i+1)),true));
            }
        }
    }
};

std::vector<std::string> position_header() {
    return {"receiver_latitude","receiver_longitude","receiver_altitude_m","receiver_fix_utc_s",
        "receiver_fix_monotonic_s","receiver_hdop","receiver_satellites","receiver_manual","receiver_source"};
}
std::vector<std::string> position_fields(const std::optional<PositionFix>& p, const ExportOptions& o) {
    if(!has_position(p))return std::vector<std::string>(position_header().size());
    return {number(rounded(p->latitude,o.coordinate_decimals)),number(rounded(p->longitude,o.coordinate_decimals)),optional_number(p->altitude_m),
        number(p->utc_seconds),number(p->monotonic_seconds),optional_number(p->hdop),integer(p->satellites),integer(p->manual),text(p->source)};
}
void waveform_report(const SessionStore& store,const Context& c,Csv& csv) {
    std::vector<std::string> header{"observation_id","center_hz","inferred_bandwidth_hz","inferred_spreading_factor",
        "first_observed_preamble_s","delimiter_s","delimiter_utc_s","up_match","down_match","contributing_subbands",
        "complete_in_requested_range","association_ambiguous","method","protocol_identity","duration_interpretation",
        "discovery_failed","discovery_rejected_input_samples","discovery_abandoned_input_samples","discovery_result_overflows"};
    if(c.options.privacy.include_receiver_positions)append(header,position_header());append(header,c.metadata_header());csv.header(header);
    store.visit_waveforms([&](const WaveformObservation& w){
        if(!c.selected(w.center_hz,w.bandwidth_hz,w.delimiter_elapsed,w.receiver_position))return;
        std::vector<std::string> row{integer(w.id),number(w.center_hz),integer(w.bandwidth_hz),integer(w.spreading_factor),
            number(w.first_observed_elapsed),number(w.delimiter_elapsed),number(w.delimiter_utc),number(w.up_match),number(w.down_match),
            integer(w.contributing_subbands),integer(w.complete_in_requested_range),integer(w.association_ambiguous),text(c.summary.discovery.method),
            text("unknown; waveform evidence is not Meshtastic/MeshCore identity"),text("observed preamble only; not packet airtime"),
            integer(c.summary.discovery.failed),integer(c.summary.discovery.rejected_input_samples),integer(c.summary.discovery.abandoned_input_samples),integer(c.summary.discovery.result_overflows)};
        if(c.options.privacy.include_receiver_positions)append(row,position_fields(w.receiver_position,c.options.privacy));append(row,c.metadata());csv.write(row);
    });
}
void track_report(const SessionStore& store,const Context& c,Csv& csv) {
    // GPS fixes carry original UTC and monotonic clocks, not session elapsed.
    // Use the recorded acquisition UTC anchor, never wall clock at export time.
    std::optional<double> utc_anchor;
    if(c.schema==7) {
        store.visit_concentrator_scans([&](const ConcentratorScan& scan){if(!utc_anchor)utc_anchor=scan.utc_start_seconds-scan.elapsed_start_seconds;});
        if(!utc_anchor)store.visit_receptions([&](const Reception& r){if(!utc_anchor)utc_anchor=r.utc_seconds-r.elapsed_seconds;});
    } else store.visit_tiles([&](const SpectrumTile& tile){if(!utc_anchor)utc_anchor=tile.utc_start_seconds-tile.elapsed_start_seconds;});
    if(!utc_anchor)throw std::runtime_error("Receiver-track filtering requires a saved acquisition time anchor");
    std::vector<std::string> header{"elapsed_s","time_association"};
    append(header,position_header());append(header,c.metadata_header());csv.header(header);
    store.visit_positions([&](const PositionFix& fix){
        const double elapsed=fix.utc_seconds-*utc_anchor;
        if(!fix.valid||elapsed<c.options.query.elapsed_start||elapsed>=c.end||!in_region(fix,c.options.query))return;
        std::vector<std::string> row{number(elapsed),text("original fix UTC minus acquisition UTC anchor; frequency selection describes survey context")};
        append(row,position_fields(fix,c.options.privacy));append(row,c.metadata());csv.write(row);
    });
}
template<class T> std::string list(const std::vector<T>& values) {
    std::string result;for(const auto& v:values){if(!result.empty())result+=';';result+=std::to_string(v);}return text(result);
}
void content_report(const SessionStore& store,const Context& c,Csv& csv) {
    std::vector<std::string> header{"reception_id","utc_s","elapsed_s","frequency_hz","bandwidth_hz","spreading_factor","coding_rate",
        "classification","authentication","profile_id","origin","destination","packet_id","port","hop_limit","hop_start","channel_hash",
        "next_hop","relay_node","want_ack","via_mqtt","want_response","request_id","reply_id","signature_present",
        "content_kind","text","node_id","long_name","short_name","sender_latitude","sender_longitude","sender_altitude_m",
        "voltage","temperature","humidity","battery_percent","channel_utilization","air_util_tx","reported_time","hardware_model","role",
        "routing_error","routing_variant","route","route_back","snr_towards_db_x4","snr_back_db_x4","concentrator_board_index","packet_rssi_dbm_uncalibrated","board_hardware_timestamp_us","bandwidth_interpretation","hardware_timestamp_provenance"};
    if(c.options.privacy.include_receiver_positions)append(header,position_header());append(header,c.metadata_header());csv.header(header);
    store.visit_receptions([&](const Reception& r){
        if(!r.crc_valid||r.decoded.status!=protocol::Status::decoded||!r.decoded.authorized||!c.selected(r.frequency_hz,r.bandwidth_hz,r.elapsed_seconds,r.receiver_position))return;
        const auto& a=*r.decoded.authorized;const auto& p=a.content;
        std::vector<std::string> row{integer(r.id),number(r.utc_seconds),number(r.elapsed_seconds),integer(r.frequency_hz),integer(r.bandwidth_hz),integer(r.spreading_factor),integer(r.coding_rate),
            text(r.decoded.classification),text(r.decoded.authentication),text(a.profile_id),integer(a.from),integer(a.to),integer(a.packet_id),integer(a.port),integer(a.hop_limit),integer(a.hop_start),integer(a.channel_hash),
            integer(a.next_hop),integer(a.relay_node),integer(a.want_ack),integer(a.via_mqtt),integer(a.want_response),c.schema>=3?integer(a.request_id):"",c.schema>=3?integer(a.reply_id):"",c.schema>=3?integer(a.signature_present):"",
            text(p.kind),text(p.text),text(p.node_id),text(p.long_name),text(p.short_name),optional_coordinate(p.latitude,c.options.privacy.coordinate_decimals),optional_coordinate(p.longitude,c.options.privacy.coordinate_decimals),optional_number(p.altitude),
            optional_number(p.voltage),optional_number(p.temperature),optional_number(p.humidity),optional_number(p.battery_percent),optional_number(p.channel_utilization),optional_number(p.air_util_tx),
            optional_number(p.reported_time),optional_number(p.hardware_model),optional_number(p.role),optional_number(p.routing_error),c.schema>=3?text(p.routing_variant):"",list(p.route),c.schema>=3?list(p.route_back):"",c.schema>=3?list(p.snr_towards):"",c.schema>=3?list(p.snr_back):""};
        if(r.concentrator)append(row,{integer(r.concentrator->board_index),number(r.concentrator->rssi_dbm),integer(r.concentrator->hardware_timestamp_us),text("hardware configured modem bandwidth; not measured signal width"),text("board-local wrapping microsecond counter; not synchronized across boards or GPS")});
        else append(row,std::vector<std::string>(5));
        if(c.options.privacy.include_receiver_positions)append(row,position_fields(r.receiver_position,c.options.privacy));append(row,c.metadata());csv.write(row);
    });
}
#include "analysis_report.hpp"

// Concentrator scans have a different observation denominator from SDR FFTs.
// Never pass them through AggregateReport or manufacture unobserved time rows.
constexpr const char* rak_method="SX1261 sampled RSSI; nominal 234300 Hz filter; vendor -11 dB offset; uncalibrated; 4 dB quantized histogram";
constexpr const char* rak_limit="Sample exceedance is not continuous occupancy or packet airtime. Host transaction bounds are not exact RF dwell. Frequencies and times without samples are unassessed. Included boundary scans retain their whole histograms.";
struct RakAggregate {
    uint64_t scans=0,samples=0,above=0,boundaries=0,missing_gps=0;
    double first=1e10,last=0,utc_first=1e15,utc_last=0;
    std::array<uint64_t,33> counts{};
    std::optional<PositionFix> last_fix;
    void add(const ConcentratorScan& s,bool boundary) {
        const auto n=concentrator_sample_count(s);
        if(samples>uint64_t(INT64_MAX)-n)throw std::runtime_error("Concentrator report sample total exceeds supported bounds");
        ++scans;samples+=n;above+=uint64_t(std::llround(concentrator_fraction_above(s,-87)*double(n)));
        boundaries+=boundary?1:0;missing_gps+=s.receiver_position?0:1;
        first=std::min(first,s.elapsed_start_seconds);last=std::max(last,s.elapsed_end_seconds);utc_first=std::min(utc_first,s.utc_start_seconds);utc_last=std::max(utc_last,s.utc_end_seconds);
        for(size_t i=0;i<33;++i)counts[i]+=s.counts[i];if(s.receiver_position)last_fix=s.receiver_position;
    }
};
bool rak_selected(const ConcentratorScan& s,const Context& c) {
    return double(s.frequency_hz)+117150>c.lower&&double(s.frequency_hz)-117150<c.upper&&
        s.elapsed_end_seconds>c.options.query.elapsed_start&&s.elapsed_start_seconds<c.end&&in_region(s.receiver_position,c.options.query);
}
bool rak_boundary(const ConcentratorScan& s,const Context& c) {
    return double(s.frequency_hz)-117150<c.lower||double(s.frequency_hz)+117150>c.upper||
        s.elapsed_start_seconds<c.options.query.elapsed_start||s.elapsed_end_seconds>c.end;
}
void rak_csv_report(const SessionStore& store,const Context& c,Csv& csv) {
    const bool grouped=c.options.kind==ReportKind::FrequencySummary;
    std::vector<std::string> header{"record_type","board_index","frequency_hz","nominal_filter_bandwidth_hz","scans","rssi_samples","quantized_threshold_dbm","sample_exceedance_pct","host_elapsed_start_s","host_elapsed_end_s","host_utc_start_s","host_utc_end_s","boundary_scans","scans_without_receiver_fix","histogram_counts_33","configured_packet_frequency_hz","configured_packet_bandwidth_hz","configured_packet_sf","configured_packet_sync_word","packets_enabled","decode_enabled","method","coverage_interpretation","position_association","session_id","recording_schema_version","source","requested_lower_hz","requested_upper_hz","requested_elapsed_start_s","requested_elapsed_end_s","geographic_filter_applied","scan_step_hz","offset_hz","recording_incomplete"};
    if(c.options.privacy.include_receiver_positions)append(header,position_header());
    if(c.options.privacy.include_provenance)append(header,{"antenna_description","receiver_description","survey_notes"});
    csv.header(header);
    const auto write=[&](unsigned board,uint64_t frequency,const RakAggregate& a) {
        const auto& profile=c.summary.config.concentrators.boards.at(board);
        std::string counts;for(const auto count:a.counts){if(!counts.empty())counts+=';';counts+=integer(count);}
        std::vector<std::string> row{text(grouped?"concentrator_frequency_summary":"concentrator_scan"),integer(board),integer(frequency),integer(234300),integer(a.scans),integer(a.samples),number(-87),percent(double(a.above),double(a.samples)),number(a.first),number(a.last),number(a.utc_first),number(a.utc_last),integer(a.boundaries),integer(a.missing_gps),text(counts),integer(profile.frequency_hz),integer(profile.bandwidth_hz),integer(profile.spreading_factor),integer(profile.sync_word),integer(profile.packets_enabled),integer(c.summary.config.concentrators.decode_enabled),text(rak_method),text(rak_limit),text(grouped?"last included receiver fix; not representative of whole frequency summary":"receiver fix at host scan completion; no transmitter position"),text(c.summary.session_id),integer(7),text("rak5146"),number(c.lower),number(c.upper),number(c.options.query.elapsed_start),c.options.query.elapsed_end>0?number(c.end):"",integer(c.options.query.geographic_filter),integer(c.summary.config.concentrators.scan_step_hz),number(double(c.summary.config.tuning_offset_hz)),integer(c.summary.incomplete)};
        if(c.options.privacy.include_receiver_positions)append(row,position_fields(a.last_fix,c.options.privacy));
        if(c.options.privacy.include_provenance)append(row,{text(c.summary.config.antenna_description),text(c.summary.config.receiver_description),text(c.summary.config.survey_notes)});
        csv.write(row);
    };
    std::map<std::pair<unsigned,uint64_t>,RakAggregate> groups;
    store.visit_concentrator_scans([&](const ConcentratorScan& s){if(!rak_selected(s,c))return;
        if(grouped){auto key=std::pair{s.board_index,s.frequency_hz};if(!groups.contains(key)&&groups.size()>=aggregate_limit)throw std::runtime_error("Too many concentrator frequency groups");groups[key].add(s,rak_boundary(s,c));}
        else{RakAggregate a;a.add(s,rak_boundary(s,c));write(s.board_index,s.frequency_hz,a);}
    });
    for(const auto& [key,a]:groups)write(key.first,key.second,a);
}
void rak_html_report(const SessionStore& store,const Context& c,const std::function<void(std::string_view)>& emit) {
    std::map<std::pair<unsigned,uint64_t>,RakAggregate> groups;
    store.visit_concentrator_scans([&](const ConcentratorScan& s){if(!rak_selected(s,c))return;auto key=std::pair{s.board_index,s.frequency_hz};if(!groups.contains(key)&&groups.size()>=aggregate_limit)throw std::runtime_error("Too many concentrator report frequencies");groups[key].add(s,rak_boundary(s,c));});
    emit("<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Concentrator RF survey</title><style>body{font:16px system-ui;max-width:1200px;margin:2em auto;padding:1em}table{border-collapse:collapse;width:100%}th,td{padding:.5em;border-bottom:1px solid #bbb;text-align:left}small{color:#444}</style><h1>Concentrator RF survey</h1>");
    emit("<p>"+html_escape(c.summary.session_id)+"</p><p>"+html_escape(rak_method)+".</p><p>"+html_escape(rak_limit)+"</p><p>Sample threshold: <strong>-87 dBm</strong>, a 4 dB histogram boundary. This nominal vendor power scale has not been calibrated. Scan centers describe overlapping receiver filters; their sample counts must not be added as independent band occupancy.</p>");
    emit("<p>Requested frequency interval: "+human_number(c.lower/1e6,6)+"–"+human_number(c.upper/1e6,6)+" MHz. Requested elapsed interval: "+human_number(c.options.query.elapsed_start)+"–"+(c.options.query.elapsed_end>0?human_number(c.end):std::string("end"))+" s. Recording "+(c.summary.incomplete?std::string("incomplete"):std::string("completed"))+".</p>");
    emit("<p>Scan step: "+integer(c.summary.config.concentrators.scan_step_hz)+" Hz. Offset: "+number(double(c.summary.config.tuning_offset_hz))+" Hz; physical tune commands add this offset to the nominal scan centers. Geographic filter: "+std::string(c.options.query.geographic_filter?"applied":"none")+".</p>");
    if(c.options.privacy.include_provenance)emit("<p>Antenna: "+html_escape(c.summary.config.antenna_description)+"<br>Receiver: "+html_escape(c.summary.config.receiver_description)+"<br>Notes: "+html_escape(c.summary.config.survey_notes)+"</p>");
    emit("<h2>Configured packet receivers</h2><p>These are configured modem settings, not measured signal bandwidths. Packet decoding covers these profiles only; RSSI scanning does not identify protocols.</p><ul>");
    for(size_t i=0;i<c.summary.config.concentrators.boards.size();++i){const auto& b=c.summary.config.concentrators.boards[i];emit("<li>Board "+integer(i+1)+": "+human_number(double(b.frequency_hz)/1e6,6)+" MHz, "+integer(b.bandwidth_hz)+" Hz, SF"+integer(b.spreading_factor)+", sync word "+integer(b.sync_word)+"; packets "+(b.packets_enabled?"enabled":"disabled")+".</li>");}
    emit("</ul><p>Authorized payload decoding "+std::string(c.summary.config.concentrators.decode_enabled?"enabled":"disabled")+". Keys and device paths are not included.</p><h2>Samples by frequency</h2><table><thead><tr><th>Board</th><th>Center MHz</th><th>Scans / samples</th><th>Samples at or above -87 dBm</th><th>Host elapsed interval (s)</th><th>Host UTC interval</th><th>Boundary scans</th><th>RSSI histogram</th>");
    if(c.options.privacy.include_receiver_positions)emit("<th>Last included receiver fix</th>");emit("</tr></thead><tbody>");
    for(const auto& [key,a]:groups){emit("<tr><td>"+integer(key.first+1)+"</td><td>"+human_number(double(key.second)/1e6,6)+"</td><td>"+integer(a.scans)+" / "+integer(a.samples)+"</td><td>"+human_percent(double(a.above),double(a.samples))+"</td><td>"+human_number(a.first)+"–"+human_number(a.last)+"</td><td>"+html_escape(human_utc(a.utc_first))+"–"+html_escape(human_utc(a.utc_last))+"</td><td>"+integer(a.boundaries)+"</td><td><details><summary>33 bins</summary><pre>");
        for(unsigned i=0;i<33;++i){const std::string band=i==0?">= -11":i==32?"< -135":"["+number(concentrator_bin_lower_dbm(i))+", "+number(concentrator_bin_lower_dbm(i-1))+")";emit(html_escape(band)+" dBm: "+integer(a.counts[i])+"\n");}emit("</pre></details></td>");if(c.options.privacy.include_receiver_positions)emit("<td>"+(a.last_fix?human_number(a.last_fix->latitude,c.options.privacy.coordinate_decimals)+", "+human_number(a.last_fix->longitude,c.options.privacy.coordinate_decimals):std::string("Unavailable"))+"</td>");emit("</tr>");}
    emit("</tbody></table><p>Empty selections have no recorded samples, not proven quiet airwaves. A last receiver fix describes one scan endpoint, not every sample in its frequency summary. The detailed CSV preserves all 33 histogram bins and per-scan intervals. No external map, script or network resource is used.</p></html>");
}
}

const char* report_kind_name(ReportKind kind) {
    switch(kind) {
        case ReportKind::FrequencySummary:return "Frequency summary";
        case ReportKind::TimeSummary:return "Frequency interval over time";
        case ReportKind::GeographicSummary:return "Frequency by geographic area";
        case ReportKind::Waveforms:return "Waveform observations";
        case ReportKind::ReceiverTrack:return "Receiver GPS track";
        case ReportKind::AuthorizedContent:return "Authorized decoded content";
        case ReportKind::Analysis:return "RF survey analysis";
    }
    throw std::runtime_error("Unknown report kind");
}
void export_survey_report(const SessionStore& store,const std::string& path,const ReportOptions& options) {
    (void)report_kind_name(options.kind);
    // Application paths are UTF-8 on every platform, including Windows.
    auto extension=std::filesystem::path(std::u8string(path.begin(),path.end())).extension().string();
    std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return char(std::tolower(c));});
    if(extension!=(options.kind==ReportKind::Analysis?".html":".csv"))
        throw std::runtime_error("Choose .html for an analysis report or .csv for a data report");
    store.write_report_file(path,[&](const std::function<void(std::string_view)>& emit){
        const Context context(options,store.read(),store.schema_version());Csv csv{emit};
        if(context.summary.config.hardware_receiver==HardwareReceiver::Rak5146&&!context.summary.config.synthetic) {
            if(options.kind==ReportKind::Analysis){rak_html_report(store,context,emit);return;}
            if(options.kind==ReportKind::FrequencySummary||options.kind==ReportKind::TimeSummary||options.kind==ReportKind::GeographicSummary){rak_csv_report(store,context,csv);return;}
            if(options.kind==ReportKind::Waveforms)throw std::runtime_error("Concentrator surveys do not contain software waveform-discovery observations");
        }
        switch(options.kind) {
            case ReportKind::FrequencySummary:case ReportKind::TimeSummary:case ReportKind::GeographicSummary: {
                AggregateReport report{context};store.visit_tiles([&](const SpectrumTile& tile){report.consume(tile);});
                store.visit_gaps([&](const CoverageGap& gap){report.gap(gap);});report.write(csv);break;
            }
            case ReportKind::Waveforms:waveform_report(store,context,csv);break;
            case ReportKind::ReceiverTrack:track_report(store,context,csv);break;
            case ReportKind::AuthorizedContent:content_report(store,context,csv);break;
            case ReportKind::Analysis:analysis_report(store,context,emit);break;
        }
    });
}
}
