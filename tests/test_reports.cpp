// SPDX-License-Identifier: GPL-3.0-or-later
// Independent typed fixtures only: no USB, network, operational GPS or payloads.
#include "ovmesh/report.hpp"
#include "storage.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
using namespace ovmesh;
unsigned checks=0;
void require(bool condition,const std::string& message) { ++checks;if(!condition)throw std::runtime_error(message); }
void near(double actual,double expected,const std::string& message,double tolerance=1e-10) {
    require(std::abs(actual-expected)<=tolerance,message+": "+std::to_string(actual)+" != "+std::to_string(expected));
}
template<class F> void rejects(F action,const std::string& message) {
    bool threw=false;try{action();}catch(const std::exception&){threw=true;}require(threw,message);
}
std::string contents(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};
}
using Record=std::map<std::string,std::string>;
std::vector<Record> read_csv(const std::filesystem::path& path) {
    std::vector<std::vector<std::string>> rows;std::vector<std::string> row;std::string field;bool quoted=false;
    const auto data=contents(path);
    for(size_t i=0;i<data.size();++i) {
        const char c=data[i];
        if(quoted) { if(c=='"'&&i+1<data.size()&&data[i+1]=='"'){field+='"';++i;}else if(c=='"')quoted=false;else field+=c; }
        else if(c=='"')quoted=true;else if(c==','){row.push_back(field);field.clear();}
        else if(c=='\n'){row.push_back(field);field.clear();rows.push_back(row);row.clear();}else field+=c;
    }
    require(!quoted&&field.empty()&&row.empty()&&!rows.empty(),"valid CSV framing");
    std::vector<Record> result;
    for(size_t i=1;i<rows.size();++i) {
        require(rows[i].size()==rows[0].size(),"stable CSV column count");Record record;
        for(size_t n=0;n<rows[0].size();++n)require(record.emplace(rows[0][n],rows[i][n]).second,"unique report columns");
        result.push_back(std::move(record));
    }
    return result;
}
ReceiverConfig configuration(bool compact) {
    ReceiverConfig c;c.compact_recording=compact;c.discover_lora=true;c.lanes.clear();
    c.session_title="synthetic report fixture";c.antenna_description="PRIVATE_TEST_ANTENNA";
    c.receiver_description="PRIVATE_TEST_RECEIVER";c.survey_notes="=PRIVATE_TEST_NOTE()";return c;
}
PositionFix position(double elapsed=0,double longitude=.1234567) {
    PositionFix p;p.valid=true;p.latitude=10.1234567;p.longitude=longitude;p.altitude_m=17.25;
    p.utc_seconds=1700000000+elapsed;p.monotonic_seconds=100+elapsed;p.hdop=.75;p.satellites=10;
    p.source="=SYNTHETIC_GPS()";return p;
}
SpectrumTile tile(const ReceiverConfig& config,uint64_t index,bool simultaneous=false) {
    SpectrumTile t;t.id=index+1;t.frame_count=4;t.first_sample=index*16384;t.end_sample=(index+1)*16384;
    t.elapsed_start_seconds=double(t.first_sample)/config.sample_rate;t.elapsed_end_seconds=double(t.end_sample)/config.sample_rate;
    t.utc_start_seconds=1700000000+t.elapsed_start_seconds;t.utc_end_seconds=1700000000+t.elapsed_end_seconds;
    t.bin_width_hz=double(config.sample_rate)/4096;t.first_center_hz=double(config.center_hz)-10*t.bin_width_hz;
    const float mean=index==0?-60.f:index==1?-40.f:-80.f;
    t.mean_dbfs={mean,mean};t.peak_dbfs={mean+10,mean+10};t.background_dbfs=-100;
    t.activity=simultaneous?std::vector<uint8_t>{3,0,3,0}:std::vector<uint8_t>{1,2,1,2};
    if(index!=1){t.receiver_start=position(t.elapsed_start_seconds,index==0?.1234567:.35);t.receiver_end=position(t.elapsed_end_seconds,index==0?.1234567:.35);}
    return t;
}
void make_session(const std::filesystem::path& path,bool compact,bool simultaneous=false) {
    const auto config=configuration(compact);SessionStore store;store.create(path.string(),config,"=SYNTHETIC_SESSION()");
    for(uint64_t i=0;i<3;++i){const auto t=tile(config,i,simultaneous);store.append(t);if(t.receiver_end)store.append(*t.receiver_end);}
    WaveformObservation w;w.id=1;w.center_hz=906875000;w.bandwidth_hz=250000;w.spreading_factor=11;
    w.first_observed_elapsed=.0001;w.delimiter_elapsed=.0005;w.delimiter_utc=1700000000.0005;w.up_match=.8;w.down_match=.9;
    w.contributing_subbands=1;w.complete_in_requested_range=true;w.receiver_position=position(.0005);store.append(w);
    w.id=2;w.center_hz=908750000;w.bandwidth_hz=500000;w.delimiter_elapsed=.0025;w.first_observed_elapsed=.002;
    w.delimiter_utc=1700000000.0025;w.receiver_position.reset();store.append(w);
    Reception r;r.id=1;r.utc_seconds=1700000000.0005;r.elapsed_seconds=.0005;r.frequency_hz=906875000;r.bandwidth_hz=250000;
    r.spreading_factor=11;r.coding_rate=5;r.header_valid=true;r.crc_valid=true;r.decoded.status=protocol::Status::decoded;
    r.decoded.classification="likely Meshtastic";r.receiver_position=position(.0005);
    protocol::AuthorizedContent a;a.profile_id="synthetic";a.port=1;a.from=1;a.to=0xffffffff;a.packet_id=17;
    a.content.kind="text";a.content.text="=SYNTHETIC(\"hello\")\nquoted, value";
    a.content.latitude=.5555555;a.content.longitude=-.6666666;r.decoded.authorized=a;store.append(r);
    r.id=2;r.elapsed_seconds=.0025;r.frequency_hz=908750000;r.decoded.authorized.reset();r.decoded.status=protocol::Status::bad_phy_crc;
    r.decoded.classification="unknown LoRa";r.header_valid=false;r.crc_valid=false;store.append(r);
    Snapshot s;s.config=config;s.elapsed_seconds=double(3*16384)/config.sample_rate;s.input_seconds=s.measurement_seconds=s.elapsed_seconds;
    s.delivered_samples=3*16384;s.total_receptions=2;s.authorized_messages=1;s.discovery.enabled=true;s.discovery.finished=true;s.discovery.observations=2;
    store.update(s,true);
}
std::vector<Record> report(const SessionStore& reader,const std::filesystem::path& directory,const std::string& name,const ReportOptions& options={}) {
    const auto path=directory/(name+".csv");export_survey_report(reader,path.string(),options);return read_csv(path);
}
void frequency_and_time(const SessionStore& reader,const SessionStore& simultaneous,const std::filesystem::path& directory) {
    const auto config=configuration(false);const double frame=4096./config.sample_rate,duration=12*frame;
    const auto rows=report(reader,directory,"frequency");require(rows.size()==2,"one row per frequency bin");
    for(const auto& row:rows) {
        near(std::stod(row.at("observed_s")),duration,"per-bin exposure never summed across bins");
        near(std::stod(row.at("busy_s")),duration/2,"per-bin activity");near(std::stod(row.at("occupancy_pct")),50,"per-bin occupancy");
        near(std::stod(row.at("mean_dbfs")),10*std::log10((1e-6+1e-4+1e-8)/3),"linear power average",.011);
        near(std::stod(row.at("peak_envelope_dbfs")),-30,"peak preserved");
        require(!row.contains("reported_south")&&!row.contains("receiver_latitude"),"no GPS columns without opt-in");
        require(row.at("session_id")=="'=SYNTHETIC_SESSION()","formula session name neutralized");
        require(row.at("activity_method").find("identity unknown")!=std::string::npos,"energy report does not invent protocol");
        near(std::stod(row.at("missing_receiver_position_s")),4*frame,"missing location retained explicitly");
    }
    require(contents(directory/"frequency.csv").find("PRIVATE_TEST")==std::string::npos,"provenance opt-out");
    ReportOptions options;options.kind=ReportKind::TimeSummary;options.query.time_bucket_seconds=60;
    const auto alternating=report(reader,directory,"alternating",options),together=report(simultaneous,directory,"simultaneous",options);
    require(alternating.size()==1&&together.size()==1,"small time summaries");
    near(std::stod(alternating[0].at("occupancy_pct")),100,"alternating selected bins union 100 percent");
    near(std::stod(together[0].at("occupancy_pct")),50,"simultaneous selected bins union 50 percent");
    near(std::stod(alternating[0].at("observed_s")),duration,"union exposure counted once");
    near(std::stod(alternating[0].at("mean_dbfs")),10*std::log10(2*(1e-6+1e-4+1e-8)/3),"summed linear band power",.011);
    options.query.lower_hz=tile(config,0).first_center_hz+1;options.query.upper_hz=options.query.lower_hz+1;
    const auto one=report(reader,directory,"one-bin",options);near(std::stod(one[0].at("occupancy_pct")),50,"requested frequency selection honored");
    options.query.elapsed_start=frame/2;options.query.elapsed_end=frame*1.5;
    const auto clipped=report(reader,directory,"partial-frame",options);near(std::stod(clipped[0].at("observed_s")),frame,"partial FFT exposure");
    near(std::stod(clipped[0].at("busy_s")),frame/2,"partial FFT busy timing");
    options={};options.privacy.include_provenance=true;
    const auto provenance=report(reader,directory,"provenance",options);
    require(provenance[0].at("survey_notes")=="'=PRIVATE_TEST_NOTE()","provenance opt-in and formula safety");
}
void geography(const SessionStore& reader,const std::filesystem::path& directory) {
    const double duration=4*4096./configuration(false).sample_rate;
    ReportOptions options;options.kind=ReportKind::GeographicSummary;
    rejects([&]{report(reader,directory,"geo-not-approved",options);},"geographic report requires position opt-in");
    require(!std::filesystem::exists(directory/"geo-not-approved.csv"),"failed report leaves no successful-looking partial file");
    options.privacy.include_receiver_positions=true;options.privacy.coordinate_decimals=7;
    const auto rows=report(reader,directory,"geographic",options);require(rows.size()==6,"two distant locations and unlocated group times two bins");
    unsigned unknown=0;std::vector<std::string> cell_ids;
    for(const auto& row:rows) {
        near(std::stod(row.at("observed_s")),duration,"geographic per-bin exposure");
        near(std::stod(row.at("occupancy_pct")),50,"geographic activity independent of aggregation");
        if(row.at("location_group")=="unlocated") {
            ++unknown;require(row.at("cell_south").empty()&&row.at("reported_south").empty(),"unknown location has no invented coordinates");
            near(std::stod(row.at("missing_receiver_position_s")),duration,"unlocated exposure distinct from quiet");
        } else {
            cell_ids.push_back(row.at("grid_cell_id"));require(!row.at("reported_south").empty(),"original receiver bounds present");
            require(std::stod(row.at("cell_north"))>=std::stod(row.at("cell_south")),"ordered metric grid bounds");
        }
    }
    require(unknown==2&&cell_ids[0]!=cell_ids[2],"locations approximately 15 miles apart stay separate");
    options.query.geographic_filter=true;options.query.south=10;options.query.north=11;options.query.west=.1;options.query.east=.2;
    const auto selected=report(reader,directory,"geographic-selection",options);require(selected.size()==2,"geographic selection applied");
    near(std::stod(selected[0].at("excluded_missing_gps_s")),duration,"unassignable GPS-filter exposure disclosed");
    options.kind=ReportKind::FrequencySummary;options.privacy.include_receiver_positions=false;
    const auto hidden=report(reader,directory,"private-geographic-filter",options);require(hidden.size()==2,"private filter still works");
    const auto file=contents(directory/"private-geographic-filter.csv");require(file.find("10.1234567")==std::string::npos&&file.find("0.1234567")==std::string::npos,"filter does not leak coordinates when export GPS off");
}
void specialized(const SessionStore& reader,const std::filesystem::path& directory) {
    ReportOptions options;options.kind=ReportKind::Waveforms;
    auto rows=report(reader,directory,"waveforms",options);require(rows.size()==2,"all waveform records retained");
    require(rows[0].at("inferred_bandwidth_hz")=="250000"&&rows[1].at("inferred_bandwidth_hz")=="500000","inferred widths preserved");
    options.query.lower_hz=906700000;options.query.upper_hz=907000000;options.query.elapsed_end=.001;
    rows=report(reader,directory,"filtered-waveform",options);require(rows.size()==1,"waveform time and footprint filters");
    require(rows[0].at("protocol_identity").find("unknown")!=std::string::npos,"no identity inferred from bandwidth");
    options={};options.kind=ReportKind::ReceiverTrack;
    rejects([&]{report(reader,directory,"track-without-gps",options);},"track GPS opt-in required");
    options.privacy.include_receiver_positions=true;options.privacy.coordinate_decimals=7;
    rows=report(reader,directory,"track",options);require(rows.size()>=2,"retained original GPS fixes exported");
    bool full_precision=false;
    for(const auto& row:rows){if(row.at("receiver_longitude")=="0.1234567")full_precision=true;require(row.at("receiver_source")=="'=SYNTHETIC_GPS()","GPS source formula safe");}
    require(full_precision,"track coordinate precision retained on request");
    options.query.elapsed_start=.002;rows=report(reader,directory,"track-time-filter",options);require(!rows.empty(),"time-selected GPS fix exists");
    for(const auto& row:rows)require(std::stod(row.at("elapsed_s"))>=.002,"track time filter honored");
    options={};options.kind=ReportKind::AuthorizedContent;
    rejects([&]{report(reader,directory,"content-without-optin",options);},"content export opt-in required");
    options.privacy.include_content=true;rows=report(reader,directory,"content",options);require(rows.size()==1,"only accepted authorized record exported");
    require(rows[0].at("text").starts_with("'=SYNTHETIC"),"decoded spreadsheet formula neutralized");
    require(rows[0].at("authentication")=="not authenticated","content authentication limit retained");
    require(!rows[0].contains("receiver_latitude"),"content export has independent receiver GPS opt-in");
    require(rows[0].at("sender_latitude")=="0.556"&&rows[0].at("sender_longitude")=="-0.667","sender positions honor export coordinate precision like the detailed archive");
}
void compact_equivalence(const SessionStore& detailed,const SessionStore& compact,const std::filesystem::path& directory) {
    ReportOptions options;
    const auto old=report(detailed,directory,"comparison-detailed",options),small=report(compact,directory,"comparison-compact",options);
    require(old.size()==small.size(),"compact keeps all frequency rows");
    for(size_t i=0;i<old.size();++i)for(const auto* key:{"observed_s","busy_s","occupancy_pct","mean_dbfs","peak_envelope_dbfs"})
        near(std::stod(small[i].at(key)),std::stod(old[i].at(key)),std::string("compact full-interval equality ")+key,.011);
    options.query.elapsed_end=4*4096./configuration(false).sample_rate;
    const auto partial=report(compact,directory,"compact-partial-power",options);
    require(std::stoul(partial[0].at("quality_flags"))&SurveyPowerAggregated,"coarse power flag exported");
    require(std::stod(partial[0].at("partial_power_support_s"))>0,"coarse partial selection disclosed");
    require(std::stod(partial[0].at("max_power_support_s"))>options.query.elapsed_end,"actual power support retained");
    near(std::stod(partial[0].at("busy_s")),options.query.elapsed_end/2,"compact partial activity exact despite coarse power");
}
void gaps_and_limits(const std::filesystem::path& directory) {
    const auto config=configuration(false);const auto path=directory/"gaps.sqlite";
    {SessionStore store;store.create(path.string(),config,"synthetic-gap");auto t=tile(config,0);store.append(t);
        CoverageGap g;g.id=1;g.elapsed_start_seconds=t.elapsed_end_seconds;g.elapsed_end_seconds=.02;g.utc_start_seconds=1700000000+g.elapsed_start_seconds;g.utc_end_seconds=1700000000.02;g.reason="synthetic missing input";store.append(g);
        Snapshot s;s.config=config;s.elapsed_seconds=.02;s.discovery.enabled=config.discover_lora;store.update(s,true);}
    SessionStore reader;reader.open_readonly(path.string());ReportOptions options;options.kind=ReportKind::TimeSummary;options.query.time_bucket_seconds=.005;
    const auto rows=report(reader,directory,"gaps",options);require(rows.size()==4,"gap buckets retained");
    require(rows[1].at("observed_s")=="0"&&rows[1].at("occupancy_pct").empty(),"unobserved time never zero-percent quiet");
    require(std::stod(rows[0].at("selection_all_location_gap_s"))>0,"gap duration retained");
    options.query.time_bucket_seconds=std::numeric_limits<double>::quiet_NaN();rejects([&]{report(reader,directory,"invalid-time",options);},"NaN grouping rejected");
    options={};options.kind=ReportKind::GeographicSummary;options.privacy.include_receiver_positions=true;options.geographic_cell_m=0;
    rejects([&]{report(reader,directory,"invalid-cell",options);},"invalid cell size rejected");
    options={};options.privacy.coordinate_decimals=8;rejects([&]{report(reader,directory,"invalid-precision",options);},"invalid GPS precision rejected");
    rejects([&]{export_survey_report(reader,(directory/"wrong.geojson").string(),{});},"report format checked");
    rejects([&]{export_survey_report(reader,(directory/"gaps.csv").string(),{});},"existing export never overwritten");
    const auto oversized=directory/"oversized.sqlite";
    {SessionStore store;store.create(oversized.string(),config,"synthetic-large-gap");auto t=tile(config,0);store.append(t);
        CoverageGap g;g.id=1;g.elapsed_start_seconds=t.elapsed_end_seconds;g.elapsed_end_seconds=1001;g.utc_start_seconds=1700000000+g.elapsed_start_seconds;g.utc_end_seconds=1700001001;g.reason="synthetic long gap";store.append(g);}
    SessionStore long_reader;long_reader.open_readonly(oversized.string());options={};options.kind=ReportKind::TimeSummary;options.query.time_bucket_seconds=.001;
    rejects([&]{report(long_reader,directory,"over-row-limit",options);},"oversized report fails instead of silently truncating");
    require(!std::filesystem::exists(directory/"over-row-limit.csv"),"oversized partial output removed");
}
void geographic_boundaries(const std::filesystem::path& directory) {
    auto config=configuration(false);const auto path=directory/"geographic-boundaries.sqlite";
    {SessionStore store;store.create(path.string(),config,"synthetic-geographic-boundaries");
        auto t=tile(config,0);t.receiver_start=position(t.elapsed_start_seconds,179.9999);t.receiver_end=position(t.elapsed_end_seconds,-179.9999);store.append(t);
        t=tile(config,1);t.receiver_start=position(t.elapsed_start_seconds,0);t.receiver_end=position(t.elapsed_end_seconds,0);
        t.receiver_start->latitude=t.receiver_end->latitude=90;store.append(t);
        t=tile(config,2);t.receiver_start=position(t.elapsed_start_seconds,0);t.receiver_end=position(t.elapsed_end_seconds,0);
        t.receiver_start->latitude=t.receiver_end->latitude=-90;store.append(t);}
    SessionStore reader;reader.open_readonly(path.string());ReportOptions options;options.kind=ReportKind::GeographicSummary;
    options.privacy.include_receiver_positions=true;options.privacy.coordinate_decimals=7;
    const auto rows=report(reader,directory,"geographic-boundaries",options);require(rows.size()==6,"date-line and polar cells remain finite and separate");
    unsigned crossing=0;
    for(const auto& row:rows) {
        const double south=std::stod(row.at("cell_south")),north=std::stod(row.at("cell_north"));
        const double west=std::stod(row.at("cell_west")),east=std::stod(row.at("cell_east"));
        require(south>=-90&&north<=90&&south<=north&&west>=-180&&east<=180&&west<east,"valid spherical cell bounds including poles");
        if(std::stod(row.at("cell_boundary_attribution_s"))>0) {
            ++crossing;require(std::stoul(row.at("quality_flags"))&SurveyBoundary,"tile cell crossing explicitly flagged");
            near(std::stod(row.at("reported_west")),-179.9999,"original date-line west coordinate preserved",1e-7);
            near(std::stod(row.at("reported_east")),179.9999,"original date-line east coordinate preserved",1e-7);
        }
    }
    require(crossing==2,"crossing exposure flagged once per frequency bin without inventing motion");
    options.query.geographic_filter=true;options.query.west=170;options.query.east=-170;
    rejects([&]{report(reader,directory,"wrapped-filter",options);},"date-line rectangle crossing rejected explicitly");
    const auto center_path=directory/"center-guard.sqlite";
    {SessionStore store;store.create(center_path.string(),config,"synthetic-center-guard");auto t=tile(config,0);t.first_center_hz=double(config.center_hz);store.append(t);}
    SessionStore center;center.open_readonly(center_path.string());
    const auto center_rows=report(center,directory,"center-guard");
    for(const auto& row:center_rows) {
        near(std::stod(row.at("occupancy_pct")),50,"raw center-region activity remains visible");
        require(row.at("outside_receiver_center_occupancy_pct").empty(),"fully excluded center region never reported as quiet");
    }
    const auto unicode=directory/"survey-\xc3\xa9.csv";export_survey_report(reader,unicode.string(),{});
    require(std::filesystem::exists(unicode),"UTF-8 report filename accepted");
}
void sql(const std::filesystem::path& path,const std::string& command) {
    sqlite3* db=nullptr;require(sqlite3_open(path.string().c_str(),&db)==SQLITE_OK,"open synthetic legacy fixture");
    const int code=sqlite3_exec(db,command.c_str(),nullptr,nullptr,nullptr);sqlite3_close(db);
    require(code==SQLITE_OK,"construct exact synthetic legacy schema");
}
void legacy_availability(const std::filesystem::path& directory) {
    const auto original=directory/"legacy-source.sqlite";make_session(original,false);
    for(const int version:{1,2,3,4}) {
        const auto path=directory/("legacy-v"+std::to_string(version)+".sqlite");std::filesystem::copy_file(original,path);
        sql(path,"PRAGMA journal_mode=DELETE;DROP TABLE waveform_observations;DROP TABLE discovery_status;DROP TABLE discovery_bands;DROP TABLE discovery_gaps;ALTER TABLE session DROP COLUMN discover_lora;");
        if(version<4)sql(path,"DROP TABLE spectrum_tiles;DROP TABLE spectrum_events;DROP TABLE coverage_gaps;DROP TABLE survey_metrology;");
        if(version<3)sql(path,"DROP TABLE route_details;ALTER TABLE receptions DROP COLUMN evidence_port;ALTER TABLE receptions DROP COLUMN evidence_signature_present;"
            "ALTER TABLE receptions DROP COLUMN request_id;ALTER TABLE receptions DROP COLUMN reply_id;ALTER TABLE receptions DROP COLUMN signature_present;ALTER TABLE receptions DROP COLUMN routing_variant;");
        if(version==1)sql(path,"ALTER TABLE session DROP COLUMN tuning_offset_hz;");
        sql(path,"PRAGMA user_version="+std::to_string(version)+";");
        const auto before=contents(path);
        {
            SessionStore reader;reader.open_readonly(path.string());require(reader.schema_version()==version,"read-only schema getter preserves actual legacy version");
            ReportOptions options;options.kind=ReportKind::AuthorizedContent;options.privacy.include_content=true;
            const auto rows=report(reader,directory,"legacy-content-v"+std::to_string(version),options);
            require(rows.size()==1&&rows[0].at("recording_schema_version")==std::to_string(version),"legacy report identifies source format");
            for(const auto* key:{"request_id","reply_id","signature_present"})
                require(rows[0].at(key)==(version<3?"":"0"),"unknown legacy fields are blank rather than invented zero");
            for(const auto* key:{"routing_variant","route_back","snr_towards_db_x4","snr_back_db_x4"})
                require(rows[0].at(key).empty(),"unavailable or empty route detail remains empty");
            require(rows[0].at("offset_hz")== (version==1?"":"0"),"unknown schema-1 offset is not fabricated");
            require(rows[0].at("fft_bin_width_hz").empty()==(version<4),"FFT metrology availability follows schema");
            options.kind=ReportKind::Waveforms;
            const auto failed_name="legacy-waveforms-v"+std::to_string(version);
            rejects([&]{report(reader,directory,failed_name,options);},"pre-waveform schema fails instead of implying zero observations");
            require(!std::filesystem::exists(directory/(failed_name+".csv")),"unavailable waveform report leaves no partial CSV");
            if(version<4){options.kind=ReportKind::FrequencySummary;rejects([&]{report(reader,directory,"legacy-frequency-v"+std::to_string(version),options);},"pre-spectrum schema has no invented fine occupancy");}
        }
        require(contents(path)==before,"legacy reporting leaves source bytes unchanged");
    }
}
void narrative_analysis(const SessionStore& reader,const SessionStore& simultaneous,const std::filesystem::path& directory) {
    ReportOptions o;o.kind=ReportKind::Analysis;o.query.time_bucket_seconds=60;
    const auto path=directory/"analysis.html";
    export_survey_report(reader,path.string(),o);const auto page=contents(path);
    for(const auto* expected:{"What was observed","Activity by frequency","When activity occurred","Receiver locations",
            "LoRa waveform and decode evidence","Measurement setup and data quality","Interpretation and next survey work",
            "SYNTHETIC SOURCE","2023-11-14 22:13:20 UTC","100.000%; 0.003072 s busy / 0.003072 s observed",
            "Mean frequency-time occupancy</td><td>50.000%","eligible authorized decodes: 1","payload CRC failures: 1",
            "Coordinates and geographic cells were excluded","not antenna-port dBm","transmitter watts"}) {
        require(page.find(expected)!=std::string::npos,std::string("analysis contains ")+expected);
    }
    require(page.find("default-src 'none'")!=std::string::npos&&page.find("<script")==std::string::npos&&
        page.find("src=\"")==std::string::npos,"standalone report has restrictive CSP and no scripts/resources");
    for(const auto* private_value:{"PRIVATE_TEST","SYNTHETIC_GPS","SYNTHETIC(&quot;hello","10.1234567","0.5555555"})
        require(page.find(private_value)==std::string::npos,"default narrative excludes notes, coordinates and message contents");
    require(page.size()<50000,"small fixture generates a manageable narrative report");
    const auto together=directory/"analysis-simultaneous.html";export_survey_report(simultaneous,together.string(),o);
    require(contents(together).find("50.000%; 0.001536 s busy / 0.003072 s observed")!=std::string::npos,
        "narrative uses joint masks instead of summing per-bin airtime");
    rejects([&]{export_survey_report(reader,path.string(),o);},"narrative refuses overwrite");
    require(contents(path)==page,"existing report remains unchanged after overwrite refusal");
    rejects([&]{export_survey_report(reader,(directory/"wrong.csv").string(),o);},"HTML extension enforced");
    o.query.lower_hz=tile(configuration(false),0).first_center_hz+1;o.query.upper_hz=o.query.lower_hz+1;
    const auto filtered=directory/"analysis-filtered.html";export_survey_report(reader,filtered.string(),o);
    require(contents(filtered).find("50.000%; 0.001536 s busy / 0.003072 s observed")!=std::string::npos,
        "narrative honors frequency filter and exposes intersecting bin edges");
    o.query.lower_hz=o.query.upper_hz=0;o.query.geographic_filter=true;
    o.query.south=80;o.query.north=81;
    const auto empty=directory/"analysis-no-exposure.html";export_survey_report(reader,empty.string(),o);
    require(contents(empty).find("No measured exposure passed this selection")!=std::string::npos&&
        contents(empty).find("Any-bin occupancy</td><td>Unavailable")!=std::string::npos,
        "empty geography is unavailable rather than quiet");
    o.query.geographic_filter=false;o.privacy.include_receiver_positions=true;o.privacy.coordinate_decimals=7;
    const auto located=directory/"analysis-located.html";export_survey_report(reader,located.string(),o);
    require(contents(located).find("2 receiver cells contain measured exposure")!=std::string::npos&&
        contents(located).find("Cell ID")!=std::string::npos,"opt-in location summary uses complete retained positions");
    const auto hostile_path=directory/"analysis-hostile.sqlite";
    { auto config=configuration(false);config.survey_notes="<script>alert('fixture')</script><img src='https://invalid.example/tracker'>";
      config.session_title="<iframe src='https://invalid.example'>";SessionStore store;store.create(hostile_path.string(),config,"synthetic-escaped");
      store.append(tile(config,0)); }
    const auto before=contents(hostile_path);SessionStore hostile;hostile.open_readonly(hostile_path.string());
    o.privacy.include_provenance=true;
    const auto escaped=directory/"analysis-escaped.html";export_survey_report(hostile,escaped.string(),o);
    require(contents(escaped).find("&lt;script&gt;")!=std::string::npos&&contents(escaped).find("<script>")==std::string::npos&&
        contents(escaped).find("<iframe")==std::string::npos,"all opt-in untrusted narrative fields are HTML escaped");
    require(contents(hostile_path)==before,"report creation does not modify source recording");
    o.query.time_bucket_seconds=0;
    rejects([&]{export_survey_report(reader,(directory/"bad-analysis.html").string(),o);},"invalid time grouping rejected");
    require(!std::filesystem::exists(directory/"bad-analysis.html"),"failure removes partial HTML");
}
}
int main(int argc,char** argv) {
    const auto dir=std::filesystem::current_path()/("report-fixtures-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directory(dir);
        const auto legacy=dir/"detailed.sqlite",compact=dir/"compact.sqlite",together=dir/"simultaneous.sqlite";
        make_session(legacy,false);make_session(compact,true);make_session(together,false,true);
        SessionStore a,b,c;a.open_readonly(legacy.string());b.open_readonly(compact.string());c.open_readonly(together.string());
        frequency_and_time(a,c,dir);geography(a,dir);specialized(a,dir);compact_equivalence(a,b,dir);gaps_and_limits(dir);geographic_boundaries(dir);legacy_availability(dir);
        narrative_analysis(a,c,dir);
        if(argc==2&&std::string(argv[1])=="--example")std::filesystem::copy_file(dir/"analysis.html","synthetic-analysis-example.html");
        std::cout<<checks<<" compact report checks passed\n";std::filesystem::remove_all(dir);return 0;
    } catch(const std::exception& e) {std::cerr<<"Report test failed: "<<e.what()<<"; fixture directory "<<dir<<'\n';return 1;}
}
