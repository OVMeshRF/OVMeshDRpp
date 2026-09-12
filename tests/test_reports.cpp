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
void sql(const std::filesystem::path& path,const std::string& command) {
    sqlite3* db=nullptr;require(sqlite3_open(path.string().c_str(),&db)==SQLITE_OK,"open synthetic legacy fixture");
    const int code=sqlite3_exec(db,command.c_str(),nullptr,nullptr,nullptr);sqlite3_close(db);
    require(code==SQLITE_OK,"construct exact synthetic legacy schema");
}
void inject_legacy_content(const std::filesystem::path& path) {
    sql(path,"UPDATE receptions SET profile='LEGACY_PRIVATE_PROFILE',origin=305419896,destination=2271560481,packet_id=19088743,"
        "port=1,hop_limit=3,hop_start=3,channel_hash=42,next_hop=12,relay_node=13,want_ack=1,via_mqtt=1,want_response=1,"
        "kind='LEGACY_PRIVATE_KIND',text='=LEGACY_PRIVATE_TEXT()',node_id='LEGACY_PRIVATE_NODE',long_name='LEGACY_PRIVATE_LONG',"
        "short_name='LEGACY_PRIVATE_SHORT',latitude=0.5555555,longitude=-0.6666666,altitude=54321,voltage=3.1415926,"
        "temperature=27.182818,humidity=61.803398,battery=73,channel_utilization=37.12345,air_util_tx=21.54321,"
        "reported_time=1701234567,hardware_model=17,role=4,routing_error=2,request_id=7654321,reply_id=1234567,"
        "signature_present=1,routing_variant='LEGACY_PRIVATE_ROUTE' WHERE id=1;");
    require(contents(path).find("LEGACY_PRIVATE_TEXT")!=std::string::npos,"synthetic legacy payload is present in source bytes");
}
void no_semantic_values(const std::filesystem::path& path) {
    const auto value=contents(path);
    for(const auto* marker:{"LEGACY_PRIVATE_","305419896","2271560481","19088743","0.5555555","-0.6666666"})
        require(value.find(marker)==std::string::npos,"export excludes legacy semantic value "+std::string(marker));
}
using Record=std::map<std::string,std::string>;
void no_semantic_columns(const Record& row) {
    for(const auto* column:{"profile","profile_id","from","to","origin","destination","packet_id","port","hop_limit","hop_start",
            "channel_hash","next_hop","relay_node","want_ack","via_mqtt","want_response","kind","content_kind","text","node_id","long_name","short_name",
            "sender_latitude","sender_longitude","sender_altitude","latitude","longitude","altitude","voltage","temperature","humidity",
            "battery","battery_percent","channel_utilization","air_util_tx","reported_time","hardware_model","role","routing_error","request_id","reply_id",
            "signature_present","routing_variant","route","route_back","snr_towards_db_x4","snr_back_db_x4"})
        require(!row.contains(column),"export omits semantic column "+std::string(column));
}
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
void low_activity_report(const std::filesystem::path& directory) {
    const auto path=directory/"low-activity.sqlite", report=directory/"low-activity.html";
    auto config=configuration(false);config.discover_lora=false;
    {
        SessionStore store;store.create(path.string(),config,"synthetic-low-activity");
        for(uint64_t index=0;index<16;++index) {
            auto t=tile(config,index);t.frame_count=128;t.first_sample=index*128*4096;t.end_sample=(index+1)*128*4096;
            t.elapsed_start_seconds=double(t.first_sample)/config.sample_rate;t.elapsed_end_seconds=double(t.end_sample)/config.sample_rate;
            t.utc_start_seconds=1700000000+t.elapsed_start_seconds;t.utc_end_seconds=1700000000+t.elapsed_end_seconds;
            t.mean_dbfs.assign(21,-60);t.peak_dbfs.assign(21,-40);t.activity.assign(128*3,0);
            t.receiver_start.reset();t.receiver_end.reset();
            for(size_t frame=0;frame<128;++frame) {
                t.activity[frame*3+1]=4; // Continuous receiver-center activity.
                if(index==0 && frame<7)t.activity[frame*3]=1;
                if(index==0 && frame<3)t.activity[frame*3+2]=16;
            }
            store.append(t);
        }
        Snapshot snapshot;snapshot.config=config;snapshot.elapsed_seconds=double(2048*4096)/config.sample_rate;
        store.update(snapshot,true);
    }
    const auto before=contents(path);SessionStore reader;reader.open_readonly(path.string());
    ReportOptions options;options.kind=ReportKind::Analysis;export_survey_report(reader,report.string(),options);
    const auto html=contents(report);
    const auto height=[&](const std::string& percent) {
        const auto title=html.find("<title>Maximum bin occupancy: "+percent+"%</title>");
        require(title!=std::string::npos,"original exact occupancy tooltip retained: "+percent);
        const auto rect=html.rfind("<rect",title), attr=html.find("height=\"",rect);
        require(rect!=std::string::npos && attr<title,"bar geometry accompanies percentage");
        return std::stod(html.substr(attr+8));
    };
    require(height("0.342")>100 && height("0.146")>90,"sub-percent activity remains visible next to continuous center");
    near(height("100.000"),195,"continuous activity remains at 100 percent");
    near(height("0.000"),0,"zero occupancy has no invented activity");
    for(const auto* label:{"0.001%","0.01%","0.1%","10%","Receiver-center guard","zero-preserving logarithmic"})
        require(html.find(label)!=std::string::npos,"readable nonlinear chart legend");
    require(contents(path)==before,"report rendering never modifies recorded measurements");
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
    r.spreading_factor=11;r.coding_rate=5;r.header_valid=true;r.crc_valid=true;r.decoded.status=protocol::Status::classified;
    r.decoded.classification="likely Meshtastic";r.receiver_position=position(.0005);
    r.decoded.evidence=protocol::EnvelopeEvidence{1,false};store.append(r);
    r.id=2;r.elapsed_seconds=.0025;r.frequency_hz=908750000;r.decoded.evidence.reset();r.decoded.status=protocol::Status::bad_phy_crc;
    r.decoded.classification="unknown LoRa";r.header_valid=false;r.crc_valid=false;store.append(r);
    Snapshot s;s.config=config;s.elapsed_seconds=double(3*16384)/config.sample_rate;s.input_seconds=s.measurement_seconds=s.elapsed_seconds;
    s.delivered_samples=3*16384;s.total_receptions=2;s.classified_receptions=1;s.discovery.enabled=true;s.discovery.finished=true;s.discovery.observations=2;
    store.update(s,true);
}
void rtl_receiver_reports(const std::filesystem::path& directory) {
    for(const bool compact:{false,true})for(const bool automatic:{false,true}) {
        const auto stem=std::string("rtl-report-")+(compact?"compact-":"detailed-")+(automatic?"auto":"manual");
        const auto path=directory/(stem+".sqlite");
        auto config=configuration(compact);config.synthetic=false;config.hardware_receiver=HardwareReceiver::RtlSdr;
        config.sample_rate=2000000;config.survey_span_hz=1500000;config.discover_lora=false;
        config.rtl_gain_tenths_db=297;config.rtl_auto_gain=automatic;
        {
            SessionStore store;store.create(path.string(),config,"rtl-report-fixture");
            auto t=tile(config,0);t.receiver_start.reset();t.receiver_end.reset();store.append(t);
            Snapshot snapshot;snapshot.config=config;snapshot.elapsed_seconds=t.elapsed_end_seconds;
            snapshot.input_seconds=snapshot.measurement_seconds=snapshot.elapsed_seconds;snapshot.delivered_samples=t.end_sample;
            store.update(snapshot,true);
        }
        SessionStore reader;reader.open_readonly(path.string());
        const auto csv=directory/(stem+".csv");export_survey_report(reader,csv.string(),{});
        const auto rows=read_csv(csv);require(rows.size()==2,"RTL report retains frequency measurements");
        for(const auto& row:rows) {
            require(row.at("source")=="RTL-SDR"&&row.at("rtl_auto_gain")==std::to_string(automatic),"Every RTL frequency row identifies receiver and gain mode");
            require(row.at("lna_gain_db").empty()&&row.at("vga_gain_db").empty()&&row.at("rf_amplifier").empty(),
                "RTL frequency reports leave inapplicable HackRF controls blank");
            require(automatic?row.at("rtl_tuner_gain_db").empty():std::stod(row.at("rtl_tuner_gain_db"))==29.7,
                "Only manual RTL gain is reported as a fixed numeric value");
        }
        ReportOptions options;options.kind=ReportKind::Analysis;
        const auto html=directory/(stem+".html");export_survey_report(reader,html.string(),options);
        const auto text=contents(html);
        require(text.find("RTL-SDR")!=std::string::npos&&text.find("LNA / VGA / RF amplifier")==std::string::npos,
            "Narrative report explains RTL setup rather than HackRF gain controls");
        require(text.find(automatic?"Automatic; sensitivity varies":"29.7 dB (applied manual gain)")!=std::string::npos,
            "Narrative gain description distinguishes automatic sensitivity from fixed applied gain");
    }
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
}
void metadata_only_exports(const SessionStore& reader,const std::filesystem::path& directory,const std::string& stem) {
    for(const bool private_fields:{false,true}) {
        ExportOptions privacy;privacy.include_receiver_positions=private_fields;privacy.include_provenance=private_fields;
        privacy.coordinate_decimals=7;
        const auto path=directory/(stem+(private_fields?"-located.csv":"-default.csv"));reader.export_csv(path.string(),privacy);
        const auto rows=read_csv(path);require(!rows.empty(),"metadata archive has rows");no_semantic_columns(rows.front());no_semantic_values(path);
        unsigned receptions=0,classified=0;
        for(const auto& row:rows) {
            if(row.at("record_type")=="session")require(row.at("total_receptions")=="2"&&row.at("classified_receptions")=="1","archive retains reception and classification counts");
            if(row.at("record_type")!="reception")continue;
            ++receptions;require(row.at("authentication")=="not authenticated","classification never claims authentication");
            require(row.at("receiver_latitude").empty()!=private_fields,"archive GPS opt-in remains independent of protocol metadata");
            if(row.at("classification")=="likely Meshtastic") {
                ++classified;require(row.at("evidence_port")=="1"&&row.at("evidence_signature_present")=="0","archive retains envelope-only evidence");
                require(row.at("frequency_hz")=="906875000"&&row.at("crc_valid")=="1","classified RF evidence remains intact");
            }
        }
        require(receptions==2&&classified==1,"archive keeps classified and CRC-failed RF receptions");
    }
    ReportOptions options;options.privacy.include_receiver_positions=true;options.privacy.include_provenance=true;options.privacy.coordinate_decimals=7;
    for(const auto kind:{ReportKind::FrequencySummary,ReportKind::TimeSummary,ReportKind::GeographicSummary,ReportKind::Waveforms,ReportKind::ReceiverTrack}) {
        options.kind=kind;const auto name=stem+"-report-"+std::to_string(static_cast<int>(kind));
        const auto rows=report(reader,directory,name,options);require(!rows.empty(),"metadata-only CSV report retains observations");
        no_semantic_columns(rows.front());no_semantic_values(directory/(name+".csv"));
    }
    const auto geo=directory/(stem+".geojson");reader.export_geojson(geo.string(),options.privacy);no_semantic_values(geo);
    for(const auto* column:{"text","node_id","long_name","short_name","sender_latitude","sender_longitude","packet_id","routing_variant"})
        require(contents(geo).find("\""+std::string(column)+"\"")==std::string::npos,"GeoJSON omits semantic property "+std::string(column));
    options.kind=ReportKind::Analysis;const auto html=directory/(stem+".html");export_survey_report(reader,html.string(),options);no_semantic_values(html);
    require(contents(html).find("likely Meshtastic classifications: 1")!=std::string::npos,"narrative retains classification count without semantic content");
    for(const auto* heading:{"<th>Text</th>","<th>Message", "<th>Sender", "<th>Node", "<th>Routing", "Authorized content"})
        require(contents(html).find(heading)==std::string::npos,"narrative omits semantic content tables");
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
void legacy_availability(const std::filesystem::path& directory) {
    const auto original=directory/"legacy-source.sqlite";make_session(original,false);inject_legacy_content(original);
    for(const int version:{1,2,3,4}) {
        const auto path=directory/("legacy-v"+std::to_string(version)+".sqlite");std::filesystem::copy_file(original,path);
        sql(path,"PRAGMA journal_mode=DELETE;DROP TABLE metadata_policy;DROP TABLE automatic_decoder;DROP TABLE waveform_observations;DROP TABLE discovery_status;DROP TABLE discovery_bands;DROP TABLE discovery_gaps;ALTER TABLE session DROP COLUMN discover_lora;");
        if(version<4)sql(path,"DROP TABLE spectrum_tiles;DROP TABLE spectrum_events;DROP TABLE coverage_gaps;DROP TABLE survey_metrology;");
        if(version<3)sql(path,"DROP TABLE route_details;ALTER TABLE receptions DROP COLUMN evidence_port;ALTER TABLE receptions DROP COLUMN evidence_signature_present;"
            "ALTER TABLE receptions DROP COLUMN request_id;ALTER TABLE receptions DROP COLUMN reply_id;ALTER TABLE receptions DROP COLUMN signature_present;ALTER TABLE receptions DROP COLUMN routing_variant;");
        if(version==1)sql(path,"ALTER TABLE session DROP COLUMN tuning_offset_hz;");
        sql(path,"PRAGMA user_version="+std::to_string(version)+";");
        const auto before=contents(path);
        {
            SessionStore reader;reader.open_readonly(path.string());require(reader.schema_version()==version,"read-only schema getter preserves actual legacy version");
            const auto archive=directory/("legacy-archive-v"+std::to_string(version)+".csv");reader.export_csv(archive.string(),{});
            const auto rows=read_csv(archive);require(!rows.empty(),"legacy archive contains metadata");
            require(rows.front().at("session_schema_version")==std::to_string(version),"legacy archive identifies source format");
            no_semantic_columns(rows.front());no_semantic_values(archive);
            unsigned receptions=0,classified=0,metrology=0;
            for(const auto& row:rows) {
                if(row.at("record_type")=="spectrum_metrology")++metrology;
                if(row.at("record_type")!="reception")continue;
                ++receptions;require(row.at("authentication")=="not authenticated","legacy classification retains authentication limit");
                if(row.at("classification")=="likely Meshtastic") {
                    ++classified;require(row.at("frequency_hz")=="906875000","legacy classified reception retains RF frequency");
                    require(row.at("evidence_port")== (version<3?"":"1"),"legacy envelope-port availability follows schema");
                    require(row.at("evidence_signature_present")== (version<3?"":"0"),"unavailable legacy envelope evidence is blank rather than invented");
                }
            }
            require(receptions==2&&classified==1,"legacy semantic redaction preserves reception and classification counts");
            require(metrology==(version<4?0u:1u),"FFT metrology availability follows schema");
            ReportOptions options;
            if(version==4) {
                const auto frequency=report(reader,directory,"legacy-frequency-v4",options);
                require(frequency.size()==2&&frequency[0].at("recording_schema_version")=="4","legacy report retains source schema metadata");
                require(frequency[0].at("offset_hz")=="0"&&!frequency[0].at("fft_bin_width_hz").empty(),"legacy report retains recorded offset and FFT metrology");
                no_semantic_columns(frequency.front());
            }
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
            "LoRa waveform and protocol evidence","Measurement setup and data quality","Interpretation and next survey work",
            "SYNTHETIC SOURCE","2023-11-14 22:13:20 UTC","100.000%; 0.003072 s busy / 0.003072 s observed",
            "Mean frequency-time occupancy</td><td>50.000%","likely Meshtastic classifications: 1","payload CRC failures: 1",
            "Coordinates and geographic cells were excluded","not antenna-port dBm","transmitter watts"}) {
        require(page.find(expected)!=std::string::npos,std::string("analysis contains ")+expected);
    }
    require(page.find("default-src 'none'")!=std::string::npos&&page.find("<script")==std::string::npos&&
        page.find("src=\"")==std::string::npos,"standalone report has restrictive CSP and no scripts/resources");
    for(const auto* private_value:{"PRIVATE_TEST","SYNTHETIC_GPS","LEGACY_PRIVATE_","10.1234567","0.5555555"})
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
void acquisition_reports(const std::filesystem::path& directory) {
    for(const bool compact:{false,true}) {
        const auto stem=std::string(compact?"segments-compact":"segments-detailed");
        auto first=configuration(compact);first.discover_lora=false;
        auto second=first;second.center_hz=915000000;second.sample_rate=8000000;second.survey_span_hz=5000000;second.lna_gain=24;second.activity_threshold_dbfs=-65;
        auto t1=tile(first,0);auto t2=tile(second,1);
        t2.elapsed_start_seconds=1.;t2.elapsed_end_seconds=1.+double(t2.end_sample-t2.first_sample)/second.sample_rate;
        t2.utc_start_seconds=1700000000+t2.elapsed_start_seconds;t2.utc_end_seconds=1700000000+t2.elapsed_end_seconds;
        t2.receiver_start=position(t2.elapsed_start_seconds);t2.receiver_end=position(t2.elapsed_end_seconds);
        const AcquisitionSegment a{1,0,t1.elapsed_end_seconds,true,first},b{2,1.,t2.elapsed_end_seconds,true,second};
        const auto path=directory/(stem+".sqlite");
        {
            SessionStore store;store.create(path.string(),first,"synthetic-acquisition-report");store.enable_acquisitions();store.begin_acquisition(a);store.append(t1);
            Snapshot state;state.config=first;state.acquisitions={a};state.elapsed_seconds=t1.elapsed_end_seconds;
            state.input_seconds=state.measurement_seconds=state.elapsed_seconds;state.delivered_samples=t1.end_sample;store.update(state,true);
            store.begin_acquisition(b);store.append(t2);
            CoverageGap pause;pause.id=1;pause.elapsed_start_seconds=t1.elapsed_end_seconds;pause.elapsed_end_seconds=1.;pause.utc_start_seconds=1700000000+pause.elapsed_start_seconds;pause.utc_end_seconds=1700000001.;pause.reason="reception paused";store.append(pause);
            state.config=second;state.acquisitions={a,b};state.elapsed_seconds=t2.elapsed_end_seconds;state.input_seconds=state.measurement_seconds=t1.elapsed_end_seconds+t2.elapsed_end_seconds-1.;state.delivered_samples=t2.end_sample;store.update(state,true);
        }
        SessionStore reader;reader.open_readonly(path.string());const auto summary=reader.read();
        require(summary.acquisitions.size()==2,"reopen retains both acquisition configurations");
        require(summary.config.sample_rate==first.sample_rate&&summary.acquisitions.back().config.sample_rate==second.sample_rate,"initial and resumed rates remain distinct");
        size_t seen=0;reader.visit_tiles([&](const SpectrumTile&){++seen;});require(seen==2,"stream validates distinct acquisition grids");
        const auto analysis=reader.analyze({});require(analysis.mixed_acquisitions&&analysis.bins.size()==4,"analysis preserves both real frequency grids");
        near(analysis.observed_seconds,t1.elapsed_end_seconds+t2.elapsed_end_seconds-1.,"pause never adds observed time");
        near(analysis.busy_seconds,analysis.observed_seconds,"per-acquisition sample rate gives exact activity time");
        require(analysis.bin_width_hz==0&&analysis.center_guard_lower_hz==0,"mixed grids do not claim one spacing or center guard");
        require(analysis.bins.front().center_hz<908000000&&analysis.bins.back().center_hz>914000000,"full-range query includes both tuned ranges");
        const auto csv=directory/(stem+"-summary.csv");ReportOptions options;export_survey_report(reader,csv.string(),options);
        const auto rows=read_csv(csv);require(rows.size()==4,"frequency report includes rows from both acquisitions");
        for(const auto& row:rows){const bool later=row.at("acquisition_id")=="2";require(row.at("sample_rate_hz")==std::to_string(later?second.sample_rate:first.sample_rate),"row uses its acquisition sample rate");near(std::stod(row.at("threshold_dbfs")),later?-65:-55,"row uses its acquisition threshold");require(row.at("discovery_health_scope").find(later?"latest acquisition":"unavailable")!=std::string::npos,"earlier acquisition health is explicitly unavailable");}
        options.kind=ReportKind::Analysis;const auto html=directory/(stem+".html");export_survey_report(reader,html.string(),options);
        const auto document=contents(html);require(document.find("Acquisition 1")!=std::string::npos&&document.find("Acquisition 2")!=std::string::npos,"narrative report separates both configurations");
        require(document.find("Unavailable for this earlier acquisition")!=std::string::npos,"narrative does not apply latest discovery health to earlier acquisition");
        require(document.find("<!doctype html>")==document.rfind("<!doctype html>"),"segmented analysis remains one HTML document");
        options.kind=ReportKind::GeographicSummary;options.privacy.include_receiver_positions=true;export_survey_report(reader,(directory/(stem+"-geo.csv")).string(),options);
        const auto detail=directory/(stem+"-detail.csv");reader.export_csv(detail.string(),{});const auto detailed=read_csv(detail);size_t metadata=0;
        for(const auto& row:detailed)if(row.at("record_type")=="acquisition_segment")++metadata;
        require(metadata==2,"detailed export carries both acquisition provenance rows");
        ExportOptions geo;geo.include_receiver_positions=true;const auto geojson=directory/(stem+".geojson");reader.export_geojson(geojson.string(),geo);
        const auto geo_text=contents(geojson);require(geo_text.find("\"busy_seconds\":0.002048")!=std::string::npos&&geo_text.find("\"acquisition_id\":2")!=std::string::npos,"geographic detailed export uses resumed rate and acquisition identity");
        const auto copy=directory/(stem+"-copy.sqlite");reader.save_copy(copy.string());SessionStore copied;copied.open_readonly(copy.string());
        require(copied.read().acquisitions.size()==2&&copied.analyze({}).bins.size()==4,"save copy retains acquisition history and old measurements");
        const auto invalid=directory/(stem+"-invalid.sqlite");reader.save_copy(invalid.string());sql(invalid,"UPDATE acquisition_segments SET elapsed_end=0.0001 WHERE id=1;");
        rejects([&]{SessionStore bad;bad.open_readonly(invalid.string());bad.visit_tiles([](const auto&){});},"measurement crossing acquisition bounds is rejected");
        const auto missing=directory/(stem+"-missing.sqlite");reader.save_copy(missing.string());sql(missing,"DELETE FROM acquisition_lanes; DELETE FROM acquisition_segments;");
        rejects([&]{SessionStore bad;bad.open_readonly(missing.string());bad.read();},"marked recording never falls back to legacy config after provenance is deleted");
        const auto guard_path=directory/(stem+"-guard.sqlite");
        auto moved=first;moved.center_hz+=62500;
        auto guarded=tile(first,0),usable=tile(moved,1);guarded.first_center_hz=usable.first_center_hz=double(first.center_hz);
        usable.elapsed_start_seconds=1.;usable.elapsed_end_seconds=1.+double(usable.end_sample-usable.first_sample)/moved.sample_rate;
        usable.utc_start_seconds=1700000001.;usable.utc_end_seconds=1700000000+usable.elapsed_end_seconds;
        const AcquisitionSegment g1{1,0,guarded.elapsed_end_seconds,true,first},g2{2,1.,usable.elapsed_end_seconds,true,moved};
        {
            SessionStore store;store.create(guard_path.string(),first,"synthetic-moving-center-guard");store.enable_acquisitions();store.begin_acquisition(g1);store.append(guarded);
            Snapshot state;state.config=first;state.acquisitions={g1};state.elapsed_seconds=guarded.elapsed_end_seconds;store.update(state,true);
            store.begin_acquisition(g2);store.append(usable);state.config=moved;state.acquisitions={g1,g2};state.elapsed_seconds=usable.elapsed_end_seconds;store.update(state,true);
        }
        SessionStore guard_reader;guard_reader.open_readonly(guard_path.string());const auto guard_analysis=guard_reader.analyze({});
        near(guard_analysis.outside_center_observed_seconds,usable.elapsed_end_seconds-1.,"center-only acquisition is excluded from outside-guard observation denominator");
        near(guard_analysis.outside_center_busy_seconds,guard_analysis.outside_center_observed_seconds,"usable outside-guard interval remains 100 percent busy");
        require(guard_analysis.observations.size()==2&&guard_analysis.observations.front().outside_center_observed_seconds==0&&guard_analysis.observations.back().outside_center_observed_seconds>0,"time buckets preserve unavailable versus measured outside-guard exposure");
    }
}

}
int main(int argc,char** argv) {
    const auto dir=std::filesystem::current_path()/("report-fixtures-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directory(dir);
        const auto legacy=dir/"detailed.sqlite",compact=dir/"compact.sqlite",together=dir/"simultaneous.sqlite";
        make_session(legacy,false);make_session(compact,true);make_session(together,false,true);
        inject_legacy_content(legacy);inject_legacy_content(compact);inject_legacy_content(together);
        SessionStore a,b,c;a.open_readonly(legacy.string());b.open_readonly(compact.string());c.open_readonly(together.string());
        frequency_and_time(a,c,dir);geography(a,dir);specialized(a,dir);compact_equivalence(a,b,dir);gaps_and_limits(dir);geographic_boundaries(dir);legacy_availability(dir);
        metadata_only_exports(a,dir,"metadata-detailed");metadata_only_exports(b,dir,"metadata-compact");
        narrative_analysis(a,c,dir);
        low_activity_report(dir);
        rtl_receiver_reports(dir);
        acquisition_reports(dir);
        if(argc==2&&std::string(argv[1])=="--example")std::filesystem::copy_file(dir/"analysis.html","synthetic-analysis-example.html");
        std::cout<<checks<<" compact report checks passed\n";std::filesystem::remove_all(dir);return 0;
    } catch(const std::exception& e) {std::cerr<<"Report test failed: "<<e.what()<<"; fixture directory "<<dir<<'\n';return 1;}
}
