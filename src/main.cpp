// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/engine.hpp"
#include "ovmesh/licenses.hpp"
#include "ovmesh/report.hpp"
#include <openssl/crypto.h>
#include <array>
#include <chrono>
#include <csignal>
#include <iostream>
#include <iomanip>
#include <filesystem>
#include <string>
#include <thread>
#include <cmath>
#include <charconv>
#include <cerrno>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {volatile std::sig_atomic_t interrupted=0;void interrupt(int){interrupted=1;}
template<class T> T integer(const std::string& text) {
    T result{};
    const auto parsed = std::from_chars(text.data(), text.data()+text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size())
        throw std::runtime_error("Expected an integer within the supported range");
    return result;
}
double duration(const std::string& text) {
    std::size_t used = 0;
    const double result = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(result))
        throw std::runtime_error("Expected a finite duration in seconds");
    return result;
}
float activity_threshold(const std::string& text) {
    try {
        std::size_t used = 0;
        const double threshold = std::stod(text, &used);
        if (used == text.size() && std::isfinite(threshold) && threshold >= -140 && threshold <= 0)
            return static_cast<float>(threshold);
    } catch (const std::invalid_argument&) {
    } catch (const std::out_of_range&) {
    }
    throw std::runtime_error("--activity-threshold-dbfs requires a finite number from -140 through 0 dBFS/bin");
}
template<size_t N> std::array<double, N> number_list(const std::string& text, const char* option) {
    std::array<double, N> values{};
    size_t start = 0;
    for (size_t i = 0; i < N; ++i) {
        const auto separator = text.find(',', start);
        if ((i + 1 < N && separator == std::string::npos) || (i + 1 == N && separator != std::string::npos))
            throw std::runtime_error(std::string(option) + " has the wrong number of comma-separated values");
        const auto field = text.substr(start, separator == std::string::npos ? std::string::npos : separator - start);
        if (field.empty()) throw std::runtime_error(std::string(option) + " fields must not be empty");
        values[i] = duration(field);
        if (separator != std::string::npos) start = separator + 1;
    }
    return values;
}
void print_analysis(const ovmesh::SurveyQuery& query, const ovmesh::SurveyAnalysis& result) {
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Spectrum survey analysis (saved measurements; no radio access)\n"
              << "detailed_available=" << result.detailed_available << '\n';
    if (!result.detailed_available) {
        std::cout << "Legacy session: detailed spectrum observations were not recorded; time, geographic and arbitrary-width occupancy analysis is unavailable.\n";
        return;
    }
    std::cout << "requested_lower_hz=" << query.lower_hz << " requested_upper_hz=" << query.upper_hz
              << " elapsed_start_s=" << query.elapsed_start << " elapsed_end_s=" << query.elapsed_end << '\n'
              << "covered_lower_hz=" << result.covered_lower_hz << " covered_upper_hz=" << result.covered_upper_hz
              << " bin_width_hz=" << result.bin_width_hz << '\n'
              << "observed_s=" << result.observed_seconds << " busy_s=" << result.busy_seconds;
    if (result.observed_seconds > 0) std::cout << " occupancy_percent=" << 100 * result.busy_seconds / result.observed_seconds;
    else std::cout << " occupancy_percent=unavailable";
    std::cout << "\nmissing_start_position_s=" << result.missing_start_position_seconds
              << " missing_end_position_s=" << result.missing_end_position_seconds;
    std::cout << "\nresolved_start_s=" << result.resolved_elapsed_start << " resolved_end_s=" << result.resolved_elapsed_end
              << " effective_time_bucket_s=" << result.effective_time_bucket_seconds
              << " observations_coarsened=" << result.observations_coarsened
              << " plotted_buckets=" << result.observations.size()
              << "\ncenter_guard_lower_hz=" << result.center_guard_lower_hz << " center_guard_upper_hz=" << result.center_guard_upper_hz
              << " center_guard_bins=" << result.center_guard_bin_count << " outside_center_bins=" << result.outside_center_bin_count
              << " center_busy_s=" << result.center_busy_seconds << " outside_center_busy_s=" << result.outside_center_busy_seconds;
    if (result.observed_seconds > 0 && result.outside_center_bin_count > 0)
        std::cout << " outside_center_occupancy_percent=" << 100 * result.outside_center_busy_seconds / result.observed_seconds;
    else std::cout << " outside_center_occupancy_percent=unavailable";
    std::cout << "\nCenter guard is a diagnostic comparison, not confirmed artifact removal; raw busy_s and all saved/exported measurements remain unchanged.\n";
    std::cout << "\ntiles=" << result.tile_count << " energy_events=" << result.event_count << " candidate_bursts=" << result.burst_count
              << " gaps=" << result.gaps.size() << " quality_flags=" << result.quality
              << " observations_truncated=" << result.observations_truncated << " events_truncated=" << result.events_truncated << '\n';
    if (query.geographic_filter) std::cout << "receiver_bounds_south=" << query.south << " north=" << query.north
        << " west=" << query.west << " east=" << query.east << '\n';
    for (const auto& event : result.events) std::cout << "energy_event=" << event.id
        << " start_s=" << event.elapsed_start_seconds << " end_s=" << event.elapsed_end_seconds
        << " lower_hz=" << event.lower_hz << " upper_hz=" << event.upper_hz
        << " measured_width_hz=" << event.upper_hz - event.lower_hz
        << " duration_s=" << event.elapsed_end_seconds - event.elapsed_start_seconds
        << " active_s=" << event.active_seconds << " peak_bin_dbfs=" << event.peak_dbfs << " quality_flags=" << event.quality << '\n';
    for (const auto& gap : result.gaps) {
        std::cout << "gap_start_s=" << gap.elapsed_start_seconds << " gap_end_s=" << gap.elapsed_end_seconds << " missing_samples=";
        if (gap.reason == "source_stall" && gap.missing_samples == 0) std::cout << "unknown";
        else std::cout << gap.missing_samples;
        std::cout << " reason=" << gap.reason << '\n';
    }
    std::cout << "candidate_burst_method=1 seed_ffts=2 tile_ms=approximately20 quiet_gap_ms=40 frequency_gap_bins=2 segment_s=10\n";
    for (const auto& burst : result.bursts) std::cout << "candidate_burst=" << burst.id
        << " start_s=" << burst.elapsed_start_seconds << " end_s=" << burst.elapsed_end_seconds
        << " lower_hz=" << burst.lower_hz << " upper_hz=" << burst.upper_hz
        << " envelope_width_hz=" << burst.upper_hz-burst.lower_hz << " active_s=" << burst.active_seconds
        << " peak_bin_dbfs=" << burst.peak_dbfs << " ambiguous=" << burst.ambiguous << " limited=" << burst.limited << '\n';
    std::cout << "waveform_observations=" << result.waveform_count << " waveform_list_truncated=" << result.waveforms_truncated << '\n';
    for(const auto& w:result.waveforms)std::cout << "LoRa_waveform=" << w.id
        << " center_hz=" << w.center_hz << " inferred_bandwidth_hz=" << w.bandwidth_hz
        << " sf=" << w.spreading_factor << " delimiter_s=" << w.delimiter_elapsed
        << " complete_in_requested_range=" << w.complete_in_requested_range << '\n';
    std::cout << "Waveform matches infer modem settings; they do not identify a mesh protocol or measure packet airtime.\n";
    if (query.geographic_filter) std::cout << "Coverage gaps have no receiver position and are omitted from geographic selections; their applicability is unknown.\n";
    std::cout << "Uncalibrated dBFS. Frequency coverage uses recorded bin boundaries. Energy events are not packets or modem bandwidths. Unknown upstream loss may remain.\n";
}
ovmesh::LaneConfig lane_configuration(const std::string& text) {
    std::array<std::string,4> fields;
    size_t start=0;
    for(size_t index=0;index<fields.size();++index) {
        const size_t separator=text.find(',',start);
        if((index+1<fields.size() && separator==std::string::npos) ||
           (index+1==fields.size() && separator!=std::string::npos))
            throw std::runtime_error("--lane requires HZ,BW_HZ,SF,CR_DENOM");
        fields[index]=text.substr(start,separator==std::string::npos?std::string::npos:separator-start);
        if(fields[index].empty())throw std::runtime_error("--lane fields must not be empty");
        if(separator!=std::string::npos)start=separator+1;
    }
    ovmesh::LaneConfig lane;
    lane.frequency_hz=integer<uint64_t>(fields[0]);
    lane.bandwidth_hz=integer<uint32_t>(fields[1]);
    lane.spreading_factor=integer<uint8_t>(fields[2]);
    lane.coding_rate=integer<uint8_t>(fields[3]);
    if(lane.spreading_factor==11 && lane.bandwidth_hz==500000 && lane.coding_rate==8)
        lane.channel_name="LongTurbo";
    else if(lane.spreading_factor==11 && lane.bandwidth_hz==250000 && lane.coding_rate==5)
        lane.channel_name="LongFast";
    return lane;
}
struct StdinKeyOption { size_t lane; std::string channel; bool survey = false; };
StdinKeyOption stdin_key_option(const std::string& text, bool survey = false) {
    const auto separator=text.find(',');
    if(separator==std::string::npos || separator==0 || separator+1==text.size() ||
       text.find(',',separator+1)!=std::string::npos || text.size()-separator-1>80)
        throw std::runtime_error(survey ? "--survey-key-stdin requires SLOT,LABEL with a nonempty label of at most 80 characters" : "--channel-key-stdin requires LANE,CHANNEL with a nonempty channel name of at most 80 characters");
    const auto lane=integer<unsigned>(text.substr(0,separator));
    if(lane<1 || lane>(survey ? 16u : 4u))throw std::runtime_error(survey ? "Survey key slot must be 1 through 16" : "Channel key lane must be 1 through 4");
    return {lane-1,text.substr(separator+1),survey};
}
void configure_stdin_key(ovmesh::Engine& engine,ovmesh::ReceiverConfig& config,const StdinKeyOption& option) {
#ifdef _WIN32
    // Read literal bytes: CRT text mode would translate CRLF and treat Ctrl-Z
    // as EOF. Restore the original mode on both success and exceptions.
    struct BinaryStdin {
        int descriptor=::_fileno(stdin);
        int previous=-1;
        BinaryStdin() {
            if(descriptor<0 || (previous=::_setmode(descriptor,_O_BINARY))==-1)
                throw std::runtime_error("Invalid redirected channel key input");
        }
        ~BinaryStdin(){if(previous!=-1)::_setmode(descriptor,previous);}
    } stdin_mode;
#endif
    // Fixed allocation before reading avoids secret-bearing string reallocations.
    // Only the populated prefix is retained after resize; the unused tail held
    // zeros and the line terminator. All exit paths clear the populated key.
    struct KeyLine {
        std::string text=std::string(65,'\0');
        ~KeyLine(){OPENSSL_cleanse(text.data(),text.size());}
    } key;
    size_t count=0;
    for(;;) {
        // Bypass stdio/iostream buffering so no extra user-space key copy is
        // retained in a CRT input buffer or read past this one bounded line.
#ifdef _WIN32
        const auto received=::_read(stdin_mode.descriptor,&key.text[count],1);
#else
        const auto received=::read(STDIN_FILENO,&key.text[count],1);
#endif
        if(received<0) {
            if(errno==EINTR)continue;
            throw std::runtime_error("Invalid redirected channel key input");
        }
        if(received==0) {
            if(count==0)throw std::runtime_error("Invalid redirected channel key input");
            break;
        }
        if(key.text[count]=='\n')break;
        if(count==64)throw std::runtime_error("Invalid redirected channel key input");
        ++count;
    }
    if(count==0)throw std::runtime_error("Invalid redirected channel key input");
    key.text.resize(count);
    std::string error;
    const bool configured=option.survey ? engine.set_survey_key(option.lane,option.channel,key.text,error)
                                        : engine.set_channel_key(option.lane,option.channel,key.text,error);
    if(!configured)
        throw std::runtime_error("Invalid redirected channel key input");
    if(!option.survey)config.lanes[option.lane].channel_name=option.channel;
}
void print_phy_counts(const ovmesh::PhyDiagnostics& phy) {
    std::cout << " preamble_candidates=" << phy.preamble_candidates << " sync_matches=" << phy.sync_matches
              << " sync_rejections=" << phy.sync_rejections << " sync_low_ratio=" << phy.sync_low_ratio
              << " sync_timeout=" << phy.sync_timeout << " sync_first_mismatch=" << phy.sync_first_mismatch
              << " sync_second_mismatch=" << phy.sync_second_mismatch << " headers_valid=" << phy.headers_valid
              << " headers_failed=" << phy.headers_failed << " completed_frames=" << phy.completed_frames;
}
void usage(){std::cout<<"OVMeshDRpp "<<ovmesh::Engine::version()<<" — local receive-only RF surveys\n"
    "  --headless-demo [--seconds 10] [--session NEW.sqlite]\n"
    "  --receive-hackrf --confirm-radio-access [--seconds 60]\n"
    "  --desktop-receive-hackrf --confirm-radio-access --seconds 180\n"
    "      (visible desktop reception; stops and closes after the requested duration)\n"
    "      Use --until-stopped instead of --seconds to keep the window open.\n"
    "      Stop reception (or SIGINT on macOS/Linux) stops acquisition; results remain visible.\n"
    "      [--session NEW.sqlite] retains authorized content and survey metadata.\n"
    "  --prepare-desktop-hackrf --seconds 180\n"
    "      (passive setup/key entry; receiver and timer start only after UI consent)\n"
    "      [--center-hz N] [--sample-rate N] [--span-hz N]\n"
    "      [--tuning-offset-hz N] (signed Hz, -100000..100000; added to hardware tune)\n"
    "      [--spectrum-only] (RF measurements only; protocol decoding paused)\n"
    "      [--discover-lora] (experimental blind preamble BW/SF observations; no payload decode)\n"
    "      [--detailed-recording] (20 ms power history; default compact ~1 s power, full activity/GPS)\n"
    "      [--antenna-description TEXT] [--receiver-description TEXT] [--survey-notes TEXT]\n"
    "      [--gps-device PATH --confirm-gps-access] [--gps-baud 9600]\n"
    "      GPS opens only the explicitly specified local serial device.\n"
    "      [--lane HZ,BW_HZ,SF,CR_DENOM] (repeat for up to four lanes)\n"
    "      [--lane-hz N] (legacy single-lane option; cannot combine with --lane)\n"
    "      [--lna-gain N] [--vga-gain N] [--rf-amplifier]\n"
    "      [--activity-threshold-dbfs N] (fixed per-bin threshold, -140..0; default -55 dBFS/bin)\n"
    "      The saved activity mask cannot be rethresholded retroactively.\n"
    "      [--channel-key-stdin LANE,CHANNEL] (one explicit hardware-mode key; lane 1..4)\n"
    "      [--survey-key-stdin SLOT,LABEL] (explicit key-only record; slot 1..16)\n"
    "      Choose one stdin key option. Keys apply across receiver profiles.\n"
    "      Automatic payload decoder dispatch is not implemented yet.\n"
    "      Key input requires redirected stdin: one line, at most 64 characters.\n"
    "      Receiver options also apply to --headless-demo. Amplifier defaults off.\n"
    "  --view-survey ABS_PATH      open saved analysis in desktop; no device access\n"
    "  --analyze-saved ABS_PATH [--frequency-range LOW_HZ,HIGH_HZ]\n"
    "      [--time-range START_SECONDS,END_SECONDS] [--bounds SOUTH,NORTH,WEST,EAST]\n"
    "      Zero frequency bounds select the full range; end time zero selects all remaining time.\n"
    "      Prints saved spectrum coverage, occupancy, energy events and gaps. No IQ replay.\n"
    "  --export EXISTING.sqlite --output NEW.csv [--content] [--positions] [--provenance]\n"
    "      [--report frequency|time|geographic|waveforms|gps|content] (default frequency CSV)\n"
    "      [--frequency-range LOW_HZ,HIGH_HZ] [--time-range START,END] [--bounds SOUTH,NORTH,WEST,EAST]\n"
    "      [--bucket-seconds 60] [--grid-metres 100] [--precision 0..7]\n"
    "      --detailed-archive instead exports the whole retained session as CSV/GeoJSON; no filters.\n"
    "  --demo                     start synthetic reception in desktop UI\n"
    "  --settings-directory PATH  use a local desktop settings folder (ordinary GUI only)\n"
    "      Ordinary GUI remembers recording/GPS preferences; GPS opens when a real survey starts.\n"
    "  --ui-smoke N --demo         render N frames, then exit (desktop only)\n"
    "  --help | --version | --licenses\n"
    "Help, passive startup, analysis and export do not open devices. Synthetic reception uses no radio.\n"
    "Keys are entered in the desktop UI or explicit redirected stdin, never as argument values.\n";}
}
int main(int argc,char** argv){
    try {
        ovmesh::ReceiverConfig config;ovmesh::ExportOptions export_options;bool headless=false,hardware=false,consent=false,demo=false,prepare=false;unsigned source_options=0;int frames=0;double seconds=10;std::string input,output,analysis_input,view_input,gps_device;
        ovmesh::SurveyQuery analysis_query;
        ovmesh::ReportOptions report_options;
        bool detailed_archive=false,report_given=false,report_settings=false;
        double report_bucket_seconds=60;
        std::string settings_directory;
        bool analysis_filter=false,spectrum_only=false,gps_consent=false,explicit_gps_baud=false,receiver_options_given=false,export_options_given=false;
        unsigned gps_baud=9600;
        bool explicit_lanes=false,legacy_lane=false,receiver_comparison=false,until_stopped=false,explicit_seconds=false,explicit_smoke=false;
        std::optional<StdinKeyOption> stdin_key;
        for(int n=1;n<argc;++n){std::string arg=argv[n];auto value=[&](){if(++n>=argc)throw std::runtime_error("Missing value for "+arg);return std::string(argv[n]);};
            if(arg=="--output"||arg=="--content"||arg=="--positions"||arg=="--provenance"||arg=="--precision"||arg=="--report"||arg=="--detailed-archive"||arg=="--bucket-seconds"||arg=="--grid-metres")export_options_given=true;
            if(arg=="--session"||arg=="--spectrum-only"||arg=="--discover-lora"||arg=="--antenna-description"||arg=="--receiver-description"||
               arg=="--survey-notes"||arg=="--center-hz"||arg=="--tuning-offset-hz"||arg=="--sample-rate"||arg=="--span-hz"||
               arg=="--lane"||arg=="--lane-hz"||arg=="--activity-threshold-dbfs"||arg=="--lna-gain"||arg=="--vga-gain"||arg=="--rf-amplifier"||arg=="--detailed-recording")receiver_options_given=true;
            if(arg=="--help"||arg=="-h"){usage();return 0;}
            else if(arg=="--version"){std::cout<<ovmesh::Engine::version()<<'\n';return 0;}
            else if(arg=="--licenses"){
                for(const auto& notice:ovmesh::license_notices())
                    std::cout<<"\n===== "<<notice.name<<" ("<<notice.source_path<<") =====\n\n"<<notice.text<<'\n';
                return 0;
            }
            else if(arg=="--headless-demo"){++source_options;headless=true;config.synthetic=true;}
            else if(arg=="--receive-hackrf"){++source_options;headless=true;hardware=true;config.synthetic=false;}
            else if(arg=="--desktop-receive-hackrf"){++source_options;hardware=true;config.synthetic=false;}
            else if(arg=="--prepare-desktop-hackrf"){++source_options;hardware=true;prepare=true;config.synthetic=false;}
            else if(arg=="--confirm-radio-access")consent=true;
            else if(arg=="--demo"){++source_options;demo=true;}
            else if(arg=="--ui-smoke"){frames=integer<int>(value());explicit_smoke=true;}
            else if(arg=="--seconds"){seconds=duration(value());explicit_seconds=true;}
            else if(arg=="--until-stopped")until_stopped=true;
            else if(arg=="--session")config.session_path=value();
            else if(arg=="--settings-directory") {
                settings_directory=value();
                if(settings_directory.empty() || !std::filesystem::path(std::u8string(settings_directory.begin(),settings_directory.end())).is_absolute())
                    throw std::runtime_error("--settings-directory requires an absolute local folder");
            }
            else if(arg=="--spectrum-only")spectrum_only=true;
            else if(arg=="--discover-lora")config.discover_lora=true;
            else if(arg=="--detailed-recording")config.compact_recording=false;
            else if(arg=="--antenna-description")config.antenna_description=value();
            else if(arg=="--receiver-description")config.receiver_description=value();
            else if(arg=="--survey-notes")config.survey_notes=value();
            else if(arg=="--gps-device")gps_device=value();
            else if(arg=="--gps-baud"){gps_baud=integer<unsigned>(value());explicit_gps_baud=true;}
            else if(arg=="--confirm-gps-access")gps_consent=true;
            else if(arg=="--center-hz")config.center_hz=integer<uint64_t>(value());
            else if(arg=="--tuning-offset-hz"){config.tuning_offset_hz=integer<int64_t>(value());receiver_comparison=true;}
            else if(arg=="--sample-rate")config.sample_rate=integer<uint32_t>(value());
            else if(arg=="--span-hz")config.survey_span_hz=integer<uint32_t>(value());
            else if(arg=="--lane") {
                if(legacy_lane)throw std::runtime_error("Do not combine --lane and --lane-hz");
                auto lane=lane_configuration(value());
                if(!explicit_lanes){config.lanes.clear();explicit_lanes=true;}
                if(config.lanes.size()>=4)throw std::runtime_error("At most four --lane options are supported");
                lane.label="CLI lane "+std::to_string(config.lanes.size()+1);
                config.lanes.push_back(std::move(lane));receiver_comparison=true;
            }
            else if(arg=="--lane-hz") {
                if(explicit_lanes)throw std::runtime_error("Do not combine --lane and --lane-hz");
                config.lanes[0].frequency_hz=integer<uint64_t>(value());legacy_lane=true;
            }
            else if(arg=="--activity-threshold-dbfs"){config.activity_threshold_dbfs=activity_threshold(value());receiver_comparison=true;}
            else if(arg=="--lna-gain"){config.lna_gain=integer<unsigned>(value());receiver_comparison=true;}
            else if(arg=="--vga-gain"){config.vga_gain=integer<unsigned>(value());receiver_comparison=true;}
            else if(arg=="--rf-amplifier"){config.amplifier=true;receiver_comparison=true;}
            else if(arg=="--channel-key-stdin" || arg=="--survey-key-stdin") {
                if(stdin_key)throw std::runtime_error("Only one redirected-stdin key option is supported");
                stdin_key=stdin_key_option(value(),arg=="--survey-key-stdin");
            }
            else if(arg=="--view-survey"){++source_options;view_input=value();if(view_input.empty()||!std::filesystem::path(view_input).is_absolute())throw std::runtime_error("--view-survey requires an absolute local path");}
            else if(arg=="--analyze-saved"){++source_options;analysis_input=value();if(analysis_input.empty())throw std::runtime_error("--analyze-saved requires an absolute local path");}
            else if(arg=="--frequency-range") {
                const auto range=number_list<2>(value(),"--frequency-range");
                analysis_query.lower_hz=range[0];analysis_query.upper_hz=range[1];analysis_filter=true;
            }
            else if(arg=="--time-range") {
                const auto range=number_list<2>(value(),"--time-range");
                analysis_query.elapsed_start=range[0];analysis_query.elapsed_end=range[1];analysis_filter=true;
            }
            else if(arg=="--bounds") {
                const auto range=number_list<4>(value(),"--bounds");
                analysis_query.south=range[0];analysis_query.north=range[1];analysis_query.west=range[2];analysis_query.east=range[3];
                analysis_query.geographic_filter=true;analysis_filter=true;
            }
            else if(arg=="--export"){++source_options;input=value();}
            else if(arg=="--detailed-archive")detailed_archive=true;
            else if(arg=="--report") {
                if(report_given)throw std::runtime_error("Choose one report type");
                report_given=true;const auto kind=value();
                if(kind=="frequency")report_options.kind=ovmesh::ReportKind::FrequencySummary;
                else if(kind=="time")report_options.kind=ovmesh::ReportKind::TimeSummary;
                else if(kind=="geographic")report_options.kind=ovmesh::ReportKind::GeographicSummary;
                else if(kind=="waveforms")report_options.kind=ovmesh::ReportKind::Waveforms;
                else if(kind=="gps")report_options.kind=ovmesh::ReportKind::ReceiverTrack;
                else if(kind=="content")report_options.kind=ovmesh::ReportKind::AuthorizedContent;
                else throw std::runtime_error("Unknown report type");
            }
            else if(arg=="--bucket-seconds"){report_settings=true;report_bucket_seconds=duration(value());}
            else if(arg=="--grid-metres"){report_settings=true;report_options.geographic_cell_m=duration(value());}
            else if(arg=="--output")output=value();
            else if(arg=="--content")export_options.include_content=true;
            else if(arg=="--provenance")export_options.include_provenance=true;
            else if(arg=="--positions")export_options.include_receiver_positions=true;
            else if(arg=="--precision")export_options.coordinate_decimals=integer<unsigned>(value());
            else throw std::runtime_error("Unknown argument: "+arg);
        }
        if(!settings_directory.empty() && (headless||hardware||demo||!view_input.empty()||!analysis_input.empty()||!input.empty()||explicit_smoke))
            throw std::runtime_error("--settings-directory is for ordinary desktop startup only; tests and explicit modes use isolated settings");
        if(!view_input.empty() && (explicit_seconds||until_stopped||receiver_options_given||stdin_key||gps_consent||
           explicit_gps_baud||!gps_device.empty()||consent||analysis_filter||export_options_given||!output.empty()||export_options.include_content||
           export_options.include_receiver_positions||export_options.include_provenance))
            throw std::runtime_error("--view-survey is passive; do not combine receiver, key, GPS, duration, analysis-filter or export options");
        if(until_stopped && (explicit_seconds || explicit_smoke || headless || (!hardware&&!demo)))
            throw std::runtime_error("--until-stopped requires a desktop receiver/setup or demo; do not combine with --seconds or --ui-smoke");
        if(source_options>1)throw std::runtime_error("Choose one reception, saved-view, analysis or export mode");
        if(analysis_filter&&analysis_input.empty()&&input.empty())throw std::runtime_error("Filters require --analyze-saved or --export");
        if(detailed_archive&&(report_given||report_settings||analysis_filter))throw std::runtime_error("Detailed archive is whole-session; use a report to apply filters");
        if(!input.empty()&&(receiver_options_given||stdin_key||gps_consent||explicit_gps_baud||!gps_device.empty()||consent||explicit_seconds))
            throw std::runtime_error("Saved export cannot be combined with reception options");
        if(analysis_query.lower_hz<0 || analysis_query.upper_hz<0 ||
           (analysis_query.upper_hz && analysis_query.upper_hz<=analysis_query.lower_hz))
            throw std::runtime_error("Invalid --frequency-range: require nonnegative ascending frequency edges");
        if(analysis_query.elapsed_start<0 || analysis_query.elapsed_end<0 ||
           (analysis_query.elapsed_end && analysis_query.elapsed_end<=analysis_query.elapsed_start))
            throw std::runtime_error("Invalid --time-range: require nonnegative ascending elapsed times");
        if(analysis_query.geographic_filter && (analysis_query.south < -90 || analysis_query.north > 90 ||
           analysis_query.south>analysis_query.north || analysis_query.west < -180 || analysis_query.east > 180 ||
           analysis_query.west>analysis_query.east))throw std::runtime_error("Invalid --bounds geographic rectangle");
        if(spectrum_only) {
            if(explicit_lanes||legacy_lane||stdin_key)throw std::runtime_error("--spectrum-only cannot be combined with decoder lanes or keys");
            if(!input.empty()||!analysis_input.empty())throw std::runtime_error("--spectrum-only applies to a new reception session");
            config.lanes.clear();
        }
        if(gps_device.empty()&&(gps_consent||explicit_gps_baud))throw std::runtime_error("GPS options require --gps-device");
        if(!gps_device.empty()&&(!gps_consent||!(headless||hardware||demo)))
            throw std::runtime_error("GPS access requires an explicit reception mode and --confirm-gps-access; no GPS device was opened");
        if(!gps_device.empty()&&!std::filesystem::path(gps_device).is_absolute())
            throw std::runtime_error("--gps-device requires an absolute local serial-device path");
        if(!headless&&!hardware&&!explicit_lanes&&!legacy_lane&&!stdin_key)config.lanes.clear();
        if(receiver_comparison&&!headless&&!hardware&&!demo)throw std::runtime_error("Receiver comparison options require a demo or an explicit HackRF reception mode");
        if(consent&&!hardware)throw std::runtime_error("Radio consent applies only to an explicit HackRF reception mode");
        if(prepare&&consent)throw std::runtime_error("Passive setup requires consent in the UI; do not supply --confirm-radio-access");
        if(hardware&&!headless&&!prepare&&frames!=0)throw std::runtime_error("Desktop hardware reception uses --seconds, not --ui-smoke");
        if(export_options.coordinate_decimals>7)throw std::runtime_error("Coordinate precision must be 0 through 7");
        if(input.empty()&&export_options_given)throw std::runtime_error("Export options require --export");
        if(explicit_smoke&&frames<=0)throw std::runtime_error("--ui-smoke requires a positive frame count");
        if(!std::isfinite(seconds)||seconds<.1||seconds>86400||frames<0||frames>100000)throw std::runtime_error("Requested duration is out of range");
        if(hardware&&!prepare&&!consent)throw std::runtime_error("Hardware access requires explicit --confirm-radio-access; no device was opened");
#ifndef OVMESH_DESKTOP
        if(!settings_directory.empty())throw std::runtime_error("--settings-directory requires the desktop executable");
        if(!view_input.empty())throw std::runtime_error("--view-survey requires the desktop executable; no device was opened");
        if(until_stopped)throw std::runtime_error("--until-stopped requires the desktop executable; no device was opened");
        if(demo&&!gps_device.empty())throw std::runtime_error("GPS with --demo requires the desktop executable; use --headless-demo instead");
        if(hardware&&!headless)throw std::runtime_error("Desktop reception requires the desktop executable; no device was opened");
#endif
        if(stdin_key) {
            if(!hardware)throw std::runtime_error("--channel-key-stdin requires an explicit HackRF reception or setup mode");
            if(!stdin_key->survey && (stdin_key->lane>=config.lanes.size() || !config.lanes[stdin_key->lane].enabled))
                throw std::runtime_error("Channel key lane must exist and be enabled");
#ifdef _WIN32
            const bool interactive=::_isatty(::_fileno(stdin))!=0;
#else
            const bool interactive=::isatty(STDIN_FILENO)!=0;
#endif
            if(interactive)throw std::runtime_error("Channel key input requires redirected stdin; interactive entry is disabled");
        }
        ovmesh::Engine engine;std::string error;
        if(stdin_key)configure_stdin_key(engine,config,*stdin_key);
        if(!input.empty()) {
            if(output.empty())throw std::runtime_error("Export requires --output with a new file path");
            if(!engine.open_session(input,error))throw std::runtime_error(error);
            report_options.query=analysis_query;report_options.query.time_bucket_seconds=report_bucket_seconds;
            report_options.privacy=export_options;
            const bool ok=detailed_archive?engine.export_session(output,export_options,error):engine.export_report(output,report_options,error);
            if(!ok)throw std::runtime_error(error);
            std::cout<<(detailed_archive?"Whole-session detailed archive":"Filtered survey report")<<" completed. No radio access.\n";return 0;
        }
        if(!analysis_input.empty()) {
            if(!std::filesystem::path(analysis_input).is_absolute())throw std::runtime_error("--analyze-saved requires an absolute local path");
            ovmesh::SurveyAnalysis analysis;
            if(!engine.open_session(analysis_input,error)||!engine.analyze_survey(analysis_query,analysis,error))throw std::runtime_error(error);
            print_analysis(analysis_query,analysis);return 0;
        }
        if(!view_input.empty()&&!engine.open_session(view_input,error))throw std::runtime_error(error);
        if(!gps_device.empty()&&!engine.connect_gps(gps_device,gps_baud,error))throw std::runtime_error(error);
        if(headless){if(hardware&&!consent)throw std::runtime_error("Hardware access requires explicit --confirm-radio-access; no device was opened");
            std::signal(SIGINT,interrupt);std::signal(SIGTERM,interrupt);
            if(!engine.start(config,hardware&&consent,error))throw std::runtime_error(error);
            std::cout << "Started " << (hardware ? "receive-only HackRF" : "synthetic reception")
                      << ": center_hz=" << config.center_hz << " tuning_offset_hz=" << config.tuning_offset_hz
                      << " tuner_command_hz=" << ovmesh::tuned_center_hz(config) << " sample_rate=" << config.sample_rate
                      << " span_hz=" << config.survey_span_hz << " duration_s=" << seconds
                      << " lna_gain_db=" << config.lna_gain << " vga_gain_db=" << config.vga_gain
                      << " rf_amplifier=" << (config.amplifier?"on":"off")
                      << " activity_threshold_dbfs=" << config.activity_threshold_dbfs << '\n';
            std::cout << "waveform_discovery=" << config.discover_lora << " automatic_decoder_dispatch=0 decoding_scope=" << (config.lanes.empty()?"paused_spectrum_only":"selected_profiles")
                      << " configured_key_records=" << engine.configured_key_count() << '\n';
            for(size_t lane=0;lane<config.lanes.size();++lane){const auto& profile=config.lanes[lane];if(!profile.enabled)continue;
                std::cout << "lane=" << lane << " frequency_hz=" << profile.frequency_hz
                          << " bandwidth_hz=" << profile.bandwidth_hz << " sf=" << unsigned(profile.spreading_factor)
                          << " cr=4/" << unsigned(profile.coding_rate) << '\n';}
            std::cout << std::flush;
            auto start=std::chrono::steady_clock::now();unsigned tick=0;
            while(!interrupted&&std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<seconds){std::this_thread::sleep_for(std::chrono::milliseconds(100));auto state=engine.snapshot();if(!state.error.empty()){engine.stop();throw std::runtime_error(state.error);}if(++tick%10==0) {
                std::cout<<"elapsed="<<state.elapsed_seconds<<" input_s="<<state.input_seconds<<" measurement_s="<<state.measurement_seconds<<" energy_events="<<state.spectrum_events<<" receptions="<<state.total_receptions<<" clipped_samples="<<state.clipped_samples<<" authorized="<<state.authorized_messages<<" dropped_samples="<<state.dropped_samples
                    <<" waveform_observations="<<state.discovery.observations<<" discovery_rejected_samples="<<state.discovery.rejected_input_samples<<'\n';
                for(size_t lane=0;lane<state.lane_health.size();++lane){const auto& health=state.lane_health[lane];
                    std::cout<<"lane="<<lane<<" processed_s="<<health.processed_seconds;
                    print_phy_counts(health.phy);std::cout<<'\n';}
                std::cout<<std::flush;
            }}
            engine.stop();auto state=engine.snapshot();std::cout<<"Stopped. "<<state.total_receptions<<" receptions, "<<state.authorized_messages<<" authorized decodes, "<<state.dropped_samples<<" application-dropped samples.\n";
            std::cout << "Final input_s=" << state.input_seconds << " measurement_s=" << state.measurement_seconds
                      << " energy_events=" << state.spectrum_events << " clipped_samples=" << state.clipped_samples
                      << " spectrum_bin_width_hz=" << state.spectrum_bin_width_hz << " spectrum_enbw_hz=" << state.spectrum_enbw_hz
                      << " estimated_background_per_bin_dbfs=" << state.background_dbfs << '\n';
            if(state.discovery.enabled) {
                const auto& d=state.discovery;
                std::cout << "Discovery method=" << d.method << " observations=" << d.observations << " finished=" << d.finished
                    << " failed=" << d.failed << " accepted_samples=" << d.accepted_input_samples
                    << " rejected_samples=" << d.rejected_input_samples << " abandoned_samples=" << d.abandoned_input_samples
                    << " result_overflows=" << d.result_overflows << " gap_overflows=" << d.gap_overflows << '\n';
                for(const auto& w:state.waveforms)std::cout << std::fixed << std::setprecision(3)
                    << "LoRa_waveform=" << w.id << " center_hz=" << w.center_hz << " inferred_bandwidth_hz=" << w.bandwidth_hz
                    << " sf=" << w.spreading_factor << " delimiter_s=" << w.delimiter_elapsed
                    << " up_match=" << w.up_match << " down_match=" << w.down_match
                    << " complete_in_requested_range=" << w.complete_in_requested_range << '\n';
            }
            for(size_t lane=0;lane<state.lane_health.size();++lane){const auto& health=state.lane_health[lane];
                std::cout << "lane=" << lane << " processed_s=" << health.processed_seconds
                          << " frames=" << health.frames << " crc_failures=" << health.crc_failures
                          << " resets=" << health.resets;
                print_phy_counts(health.phy);std::cout<<'\n';}
            return state.error.empty()?0:1;
        }
#ifdef OVMESH_DESKTOP
        if(hardware){
            if(!prepare&&!consent)throw std::runtime_error("Hardware access requires explicit --confirm-radio-access; no device was opened");
            return ovmesh::run_desktop(engine,prepare?frames:0,false,&config,seconds,prepare,until_stopped);
        }
        if(demo&&until_stopped)return ovmesh::run_desktop(engine,0,true,&config,seconds,false,true);
        // Initial UI settings do not imply an automatic or timed receiver launch.
        return ovmesh::run_desktop(engine,frames,demo,(receiver_options_given || demo) ? &config : nullptr,0,false,false,settings_directory);
#else
        if(hardware)throw std::runtime_error("Desktop reception requires the desktop executable; no device was opened");
        (void)demo;usage();return 0;
#endif
    }catch(const std::exception& e){std::cerr<<"OVMeshDRpp: "<<e.what()<<'\n';return 1;}
}
