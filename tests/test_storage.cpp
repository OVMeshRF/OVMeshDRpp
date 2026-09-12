// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic typed fixtures only: no operational routes, keys, RF bytes, or messages.
#include "storage.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
ovmesh::ReceiverConfig detailed_config(){ovmesh::ReceiverConfig c;c.compact_recording=false;return c;}
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
template<typename F> void rejects(F action,const char* message) {
    bool threw=false;try{action();}catch(const std::exception&){threw=true;}require(threw,message);
}
std::string contents(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};
}
void execute_sql(const std::filesystem::path& path,const std::string& sql) {
    sqlite3* db=nullptr;require(sqlite3_open(path.string().c_str(),&db)==SQLITE_OK,"Open fixture database");
    const int result=sqlite3_exec(db,sql.c_str(),nullptr,nullptr,nullptr);sqlite3_close(db);
    require(result==SQLITE_OK,"Fixture SQL failed");
}
std::vector<std::vector<std::string>> parse_csv(const std::string& input) {
    std::vector<std::vector<std::string>> rows;std::vector<std::string> row;std::string field;bool quoted=false;
    for(size_t i=0;i<input.size();++i) {
        const char c=input[i];
        if(quoted) {
            if(c=='"' && i+1<input.size() && input[i+1]=='"'){field+='"';++i;}
            else if(c=='"')quoted=false;else field+=c;
        } else if(c=='"')quoted=true;
        else if(c==','){row.push_back(field);field.clear();}
        else if(c=='\n'){row.push_back(field);field.clear();rows.push_back(row);row.clear();}
        else field+=c;
    }
    require(!quoted && field.empty() && row.empty(),"Malformed CSV output");
    for(const auto& record:rows)require(record.size()==rows.front().size(),"CSV row width changed");
    return rows;
}
using Record=std::map<std::string,std::string>;
std::vector<Record> csv_records(const std::filesystem::path& path) {
    const auto rows=parse_csv(contents(path));require(!rows.empty(),"CSV header missing");
    std::vector<Record> records;
    for(size_t i=1;i<rows.size();++i){Record record;for(size_t n=0;n<rows[0].size();++n)record[rows[0][n]]=rows[i][n];records.push_back(record);}
    return records;
}
ovmesh::PositionFix fix() {
    ovmesh::PositionFix f;f.latitude=0.1234567;f.longitude=-0.7654321;f.altitude_m=42.5;
    f.utc_seconds=1700000000.25;f.monotonic_seconds=123.75;f.valid=true;f.manual=true;
    f.source="synthetic \"GPS\"\nfixture";f.hdop=0.9;f.satellites=11;return f;
}
ovmesh::Reception fixture(uint64_t id=1) {
    ovmesh::Reception r;r.id=id;r.utc_seconds=1700000001.5;r.elapsed_seconds=1.5;r.frequency_hz=906875000;
    r.bandwidth_hz=250000;r.spreading_factor=11;r.coding_rate=5;r.duration_seconds=0.125;r.snr_db=-4.25;
    r.frequency_error_hz=-100.5;r.header_valid=true;r.crc_valid=true;r.lane_label="Synthetic profile";
    r.decoded.status=ovmesh::protocol::Status::classified;r.decoded.classification="likely Meshtastic";
    r.decoded.authentication="not authenticated";r.receiver_position=fix();
    r.decoded.evidence=ovmesh::protocol::EnvelopeEvidence{1,true};return r;
}
void metadata_equal(const ovmesh::Reception& actual,const ovmesh::Reception& original) {
    require(actual.id==original.id && actual.frequency_hz==original.frequency_hz && actual.bandwidth_hz==original.bandwidth_hz &&
        actual.spreading_factor==original.spreading_factor && actual.coding_rate==original.coding_rate &&
        actual.duration_seconds==original.duration_seconds && actual.snr_db==original.snr_db &&
        actual.frequency_error_hz==original.frequency_error_hz,"RF reception roundtrip");
    require(actual.decoded.status==original.decoded.status && actual.decoded.classification==original.decoded.classification &&
        actual.decoded.authentication==original.decoded.authentication,"Protocol metadata roundtrip");
    require(actual.decoded.evidence.has_value()==original.decoded.evidence.has_value(),"Envelope evidence presence roundtrip");
    if(original.decoded.evidence)require(actual.decoded.evidence->port==original.decoded.evidence->port &&
        actual.decoded.evidence->signature_present==original.decoded.evidence->signature_present,"Envelope evidence roundtrip");
    require(actual.receiver_position && actual.receiver_position->latitude==fix().latitude && actual.receiver_position->longitude==fix().longitude &&
        actual.receiver_position->monotonic_seconds==fix().monotonic_seconds && actual.receiver_position->hdop==fix().hdop &&
        actual.receiver_position->source==fix().source && actual.receiver_position->satellites==11,"Receiver fix roundtrip");
}
void assert_metadata_columns(const Record& row) {
    for(const char* field:{"profile_id","content_kind","text","origin","destination","packet_id","port","hop_limit","hop_start",
        "channel_hash","next_hop","relay_node","want_ack","via_mqtt","want_response","node_id","long_name","short_name",
        "sender_latitude","sender_longitude","sender_altitude","voltage","temperature","humidity","battery_percent",
        "channel_utilization","air_util_tx","reported_time","hardware_model","role","routing_error","route","request_id",
        "reply_id","signature_present","routing_variant","route_back","snr_towards_db_x4","snr_back_db_x4"})
        require(!row.contains(field),"Semantic/identity column remains in metadata export");
}
int schema_version(const std::filesystem::path& path) {
    sqlite3* db=nullptr;sqlite3_stmt* statement=nullptr;
    require(sqlite3_open_v2(path.string().c_str(),&db,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK,"Open schema version fixture");
    require(sqlite3_prepare_v2(db,"PRAGMA user_version",-1,&statement,nullptr)==SQLITE_OK,"Prepare version read");
    require(sqlite3_step(statement)==SQLITE_ROW,"Read schema version");
    const int version=sqlite3_column_int(statement,0);sqlite3_finalize(statement);sqlite3_close(db);return version;
}
void make_legacy(const std::filesystem::path& path,int version) {
    // These fixtures predate both optional extensions; changing user_version
    // alone must not leave a newly created decoder table in an old schema.
    execute_sql(path,"DROP TABLE metadata_policy;DROP TABLE automatic_decoder;");
    if(version<5)execute_sql(path,"DROP TABLE waveform_observations;DROP TABLE discovery_status;DROP TABLE discovery_bands;DROP TABLE discovery_gaps;ALTER TABLE session DROP COLUMN discover_lora;");
    if(version<4)execute_sql(path,"PRAGMA journal_mode=DELETE;DROP TABLE spectrum_tiles;DROP TABLE spectrum_events;DROP TABLE coverage_gaps;DROP TABLE survey_metrology;");
    if(version<3)execute_sql(path,"DROP TABLE route_details;"
        "ALTER TABLE receptions DROP COLUMN evidence_port;ALTER TABLE receptions DROP COLUMN evidence_signature_present;"
        "ALTER TABLE receptions DROP COLUMN request_id;ALTER TABLE receptions DROP COLUMN reply_id;"
        "ALTER TABLE receptions DROP COLUMN signature_present;ALTER TABLE receptions DROP COLUMN routing_variant;");
    if(version==1)execute_sql(path,"ALTER TABLE session DROP COLUMN tuning_offset_hz;");
    execute_sql(path,"PRAGMA user_version="+std::to_string(version)+";");
}
void rtl_receiver_storage(const std::filesystem::path& directory) {
    unsigned serial=0;
    for(const bool compact:{false,true})for(const bool automatic:{false,true}) {
        const auto stem="rtl-metadata-"+std::to_string(serial++);
        const auto path=directory/(stem+".sqlite");
        ovmesh::ReceiverConfig config;config.synthetic=false;config.hardware_receiver=ovmesh::HardwareReceiver::RtlSdr;
        config.compact_recording=compact;config.center_hz=906875000;config.sample_rate=2000000;
        config.survey_span_hz=1500000;config.tuning_offset_hz=-600;
        config.rtl_gain_tenths_db=297;config.rtl_auto_gain=automatic;config.lanes.clear();
        {ovmesh::SessionStore store;store.create(path.string(),config,"rtl-typed-fixture");}
        const auto before=contents(path);
        ovmesh::SessionStore reader;reader.open_readonly(path.string());const auto saved=reader.read();
        require(!saved.config.synthetic&&saved.config.hardware_receiver==ovmesh::HardwareReceiver::RtlSdr&&
            saved.config.rtl_gain_tenths_db==297&&saved.config.rtl_auto_gain==automatic&&
            saved.config.sample_rate==2000000&&saved.config.survey_span_hz==1500000&&saved.config.tuning_offset_hz==-600&&
            saved.config.compact_recording==compact,"RTL identity, applied gain/mode and acquisition setup round trip in both storage modes");
        require(reader.schema_version()==(compact?6:5),"RTL extension preserves existing measurement encoding versions");
        const auto exported=directory/(stem+".csv");reader.export_csv(exported.string(),{});
        const auto rows=csv_records(exported);
        require(!rows.empty()&&rows.front().at("source")=="RTL-SDR"&&rows.front().at("rtl_auto_gain")==std::to_string(automatic),
            "Detailed CSV identifies RTL receiver and gain mode");
        require(rows.front().at("lna_gain_db").empty()&&rows.front().at("vga_gain_db").empty()&&
            rows.front().at("rf_amplifier_enabled").empty(),"RTL export does not invent HackRF gain stages");
        require(automatic?rows.front().at("rtl_tuner_gain_db").empty():std::stod(rows.front().at("rtl_tuner_gain_db"))==29.7,
            "Automatic gain exports no fixed gain; manual gain exports the applied value");
        const auto copy=directory/(stem+"-copy.sqlite");reader.save_copy(copy.string());
        ovmesh::SessionStore copied;copied.open_readonly(copy.string());
        require(copied.read().config.hardware_receiver==ovmesh::HardwareReceiver::RtlSdr&&
            copied.read().config.rtl_auto_gain==automatic,"Saving a copy retains RTL provenance without rewriting the recording");
        ovmesh::ExportOptions options;options.include_receiver_positions=true;
        const auto geo=directory/(stem+".geojson");reader.export_geojson(geo.string(),options);
        require(contents(geo).find("\"source\":\"RTL-SDR\"")!=std::string::npos&&
            contents(geo).find("\"rtl_auto_gain\":"+std::to_string(automatic))!=std::string::npos,
            "GeoJSON includes RTL acquisition metadata even when no GPS fix was recorded");
        require(contents(path)==before,"RTL read/copy/export leave original recording unchanged");
        for(const auto* mutation:{"DROP TABLE receiver_setup;", "UPDATE session SET synthetic=0;", "UPDATE session SET synthetic=3;",
                "UPDATE receiver_setup SET hardware='HackRF';", "UPDATE receiver_setup SET gain_tenths_db=601;",
                "UPDATE receiver_setup SET gain_tenths_db=-101;", "UPDATE receiver_setup SET gain_tenths_db=29.7;",
                "UPDATE receiver_setup SET auto_gain=2;", "UPDATE receiver_setup SET id=2;", "DELETE FROM receiver_setup;",
                "INSERT INTO receiver_setup VALUES(2,'rtl_sdr',280,0);", "ALTER TABLE receiver_setup ADD COLUMN extra TEXT;"}) {
            const auto invalid=directory/(stem+"-bad-"+std::to_string(serial++)+".sqlite");std::filesystem::copy_file(path,invalid);
            execute_sql(invalid,mutation);
            rejects([&]{ovmesh::SessionStore bad;bad.open_readonly(invalid.string());(void)bad.read();},
                "Inconsistent, ambiguous, malformed or altered RTL metadata is rejected");
        }
    }
    const auto legacy=directory/"hackrf-metadata.sqlite";
    auto config=detailed_config();config.synthetic=false;
    {ovmesh::SessionStore store;store.create(legacy.string(),config,"typed-hackrf-fixture");}
    ovmesh::SessionStore reader;reader.open_readonly(legacy.string());
    require(!reader.read().config.synthetic&&reader.read().config.hardware_receiver==ovmesh::HardwareReceiver::HackRf,
        "Legacy boolean hardware source remains HackRF without RTL metadata");
}
void calibration_storage(const std::filesystem::path& directory) {
    std::filesystem::path positive;
    unsigned counter=0;
    for(const int64_t offset:{900LL,-900LL,0LL,100000LL,-100000LL}) {
        ovmesh::ReceiverConfig config;config.compact_recording=false;config.center_hz=907500000;config.tuning_offset_hz=offset;
        const auto path=directory/("calibration-"+std::to_string(counter++)+".sqlite");
        {ovmesh::SessionStore writer;writer.create(path.string(),config,"synthetic-calibration");}
        require(schema_version(path)==5,"New sessions must write schema 5");
        const auto before=contents(path);
        {
            ovmesh::SessionStore reader;reader.open_readonly(path.string());const auto saved=reader.read();
            require(saved.config.tuning_offset_hz==offset && saved.config.center_hz==config.center_hz,"Calibration must preserve nominal center and signed correction");
            const auto exported=directory/("calibration-"+std::to_string(counter)+".csv");reader.export_csv(exported.string(),{});
            const auto records=csv_records(exported);
            require(!records.empty() && records[0].at("record_type")=="session","Calibration export session row");
            require(records[0].at("tuning_offset_hz")==std::to_string(offset),"CSV calibration correction lost");
            require(records[0].at("tuner_command_hz")==std::to_string(907500000LL+offset),"CSV tuner command must be nominal plus correction");
            require(records[0].at("frequency_hz")=="907500000","Calibration must not shift nominal report axis");
        }
        require(contents(path)==before,"Reading/exporting schema 3 changed the session");
        if(offset==900)positive=path;
    }
    // Construct the former exact schema from a synthetic copy, then prove the
    // production reader neither migrates it nor writes a correction into it.
    const auto legacy=directory/"legacy-v1.sqlite";std::filesystem::copy_file(positive,legacy);
    make_legacy(legacy,1);
    const auto legacy_before=contents(legacy);
    {
        ovmesh::SessionStore reader;reader.open_readonly(legacy.string());const auto saved=reader.read();
        require(saved.config.tuning_offset_hz==0 && ovmesh::tuned_center_hz(saved.config)==saved.config.center_hz,"Version 1 must imply zero correction");
        const auto exported=directory/"legacy-v1.csv";reader.export_csv(exported.string(),{});
        const auto records=csv_records(exported);
        require(records[0].at("tuning_offset_hz")=="0" && records[0].at("tuner_command_hz")=="907500000","Version 1 CSV calibration provenance");
        rejects([&]{reader.append(fix());},"Legacy readonly reader accepted a write");
        const auto copied=directory/"legacy-v1-copy.sqlite";
        rejects([&]{reader.save_copy(copied.string());},"Unmarked historical copy accepted");
        require(!std::filesystem::exists(copied),"Rejected historical copy created a destination");
    }
    require(schema_version(legacy)==1 && contents(legacy)==legacy_before,"Legacy schema was migrated or modified");
    require(!std::filesystem::exists(legacy.string()+"-wal") && !std::filesystem::exists(legacy.string()+"-shm"),"Legacy readonly open created journal files");

    for(const std::string& value:{"100001","-100001","9223372036854775807","-9223372036854775808","900.5","'not-an-integer'","X'00'"}) {
        const auto path=directory/("invalid-offset-"+std::to_string(counter++)+".sqlite");std::filesystem::copy_file(positive,path);
        execute_sql(path,"UPDATE session SET tuning_offset_hz="+value+";");
        ovmesh::SessionStore reader;reader.open_readonly(path.string());
        rejects([&]{reader.read();},"Malformed or out-of-range saved correction accepted");
        rejects([&]{reader.export_csv((directory/("invalid-offset-"+std::to_string(counter)+".csv")).string(),{});},"Malformed correction exported");
    }
    for(const auto& pair:std::vector<std::pair<uint64_t,int64_t>>{
            {1000000,-1},{6000000000ULL,1},{999999,1000},{6000000001ULL,-1000},
            {907500000,100001},{907500000,-100001},{907500000,std::numeric_limits<int64_t>::min()}}) {
        ovmesh::ReceiverConfig config;config.compact_recording=false;config.center_hz=pair.first;config.tuning_offset_hz=pair.second;
        const auto path=directory/("invalid-create-"+std::to_string(counter++)+".sqlite");ovmesh::SessionStore writer;
        rejects([&]{writer.create(path.string(),config,"synthetic-invalid");},"Invalid tuner configuration accepted at creation");
        require(!writer.is_open() && !std::filesystem::exists(path),"Invalid calibration created a session file");
        const auto malformed=directory/("invalid-tuner-"+std::to_string(counter)+".sqlite");std::filesystem::copy_file(positive,malformed);
        execute_sql(malformed,"UPDATE session SET center="+std::to_string(pair.first)+",tuning_offset_hz="+std::to_string(pair.second)+";");
        ovmesh::SessionStore reader;reader.open_readonly(malformed.string());rejects([&]{reader.read();},"Invalid saved nominal/corrected tuner frequency accepted");
    }
    for(const int version:{1,2,3,4,6}) {
        const auto path=directory/("wrong-schema-"+std::to_string(version)+".sqlite");std::filesystem::copy_file(positive,path);
        execute_sql(path,"PRAGMA user_version="+std::to_string(version)+";");
        ovmesh::SessionStore reader;rejects([&]{reader.open_readonly(path.string());},"Unexpected schema/layout combination accepted");
    }
    const auto missing=directory/"v2-missing-offset.sqlite";std::filesystem::copy_file(legacy,missing);
    execute_sql(missing,"PRAGMA user_version=2;");ovmesh::SessionStore reader;
    rejects([&]{reader.open_readonly(missing.string());},"Version 2 missing calibration column accepted");
}
void spectrum_storage(const std::filesystem::path& directory) {
    ovmesh::ReceiverConfig config;config.compact_recording=false;config.antenna_description="SYNTHETIC_PRIVATE_ANTENNA";config.survey_notes="SYNTHETIC_PRIVATE_NOTES";
    const double frame=4096.0/config.sample_rate,duration=frame*4,width=double(config.sample_rate)/4096;
    const auto make_tile=[&](bool simultaneous,uint64_t id=1) {
        ovmesh::SpectrumTile tile;tile.id=id;tile.frame_count=4;tile.first_sample=(id-1)*16384;tile.end_sample=id*16384;
        tile.elapsed_start_seconds=double(id-1)*duration;tile.elapsed_end_seconds=double(id)*duration;
        tile.utc_start_seconds=1700000000+tile.elapsed_start_seconds;tile.utc_end_seconds=1700000000+tile.elapsed_end_seconds;
        tile.first_center_hz=double(config.center_hz);tile.bin_width_hz=width;tile.mean_dbfs={-60,-60};tile.peak_dbfs={-40,-40};tile.background_dbfs=-100;
        tile.activity=simultaneous?std::vector<uint8_t>{3,0,3,0}:std::vector<uint8_t>{1,2,1,2};tile.receiver_start=fix();tile.receiver_end=fix();return tile;
    };
    const auto alternate=directory/"spectrum-alternating.sqlite",simultaneous=directory/"spectrum-simultaneous.sqlite";
    for(const bool simultaneous_activity:{false,true}) {
        ovmesh::SessionStore writer;writer.create((simultaneous_activity?simultaneous:alternate).string(),config,"synthetic-spectrum");
        writer.append(make_tile(simultaneous_activity));
        ovmesh::Snapshot snapshot;snapshot.config=config;snapshot.elapsed_seconds=duration;snapshot.measurement_seconds=duration;writer.update(snapshot);
        // A second independent connection sees only committed batches while the writer stays open.
        ovmesh::SessionStore live;live.open_readonly((simultaneous_activity?simultaneous:alternate).string());require(live.analyze({}).tile_count==1,"Live committed spectrum unavailable");
        auto invalid=make_tile(false,2);invalid.activity={3};rejects([&]{writer.append(invalid);},"Malformed tile mask persisted");
        invalid=make_tile(false,2);invalid.first_center_hz+=width/2;rejects([&]{writer.append(invalid);},"Off-grid tile persisted");
        invalid=make_tile(false,2);invalid.activity.back()=128;rejects([&]{writer.append(invalid);},"Nonzero padded activity persisted");
        invalid=make_tile(false,2);invalid.mean_dbfs[0]=std::numeric_limits<float>::infinity();rejects([&]{writer.append(invalid);},"Nonfinite power persisted");
        rejects([&]{writer.append(make_tile(false));},"Overlapping tiles persisted");
    }
    ovmesh::SessionStore a,b;a.open_readonly(alternate.string());b.open_readonly(simultaneous.string());
    const auto aa=a.analyze({}),bb=b.analyze({});
    require(aa.detailed_available&&aa.bins.size()==2&&aa.tile_count==1,"Detailed spectrum unavailable");
    require(std::abs(aa.busy_seconds-duration)<1e-12&&std::abs(bb.busy_seconds-duration/2)<1e-12,"Joint occupancy confused simultaneous and alternating bins");
    require(std::abs(aa.bins[0].active_seconds-duration/2)<1e-12&&std::abs(bb.bins[0].active_seconds-duration/2)<1e-12,"Marginal occupancy changed");
    require(std::abs(aa.observations[0].mean_dbfs-(-60+10*std::log10(2)))<0.001,"Linear power integration incorrect");
    ovmesh::SurveyQuery query;query.elapsed_start=frame/2;query.elapsed_end=frame*1.5;
    auto half=a.analyze(query);require(std::abs(half.observed_seconds-frame)<1e-12&&std::abs(half.busy_seconds-frame)<1e-12&&(half.quality&ovmesh::SurveyBoundary),"Partial FFT interval filtering incorrect");
    query={};query.lower_hz=double(config.center_hz)+1;query.upper_hz=double(config.center_hz)+2;
    auto narrow=a.analyze(query);require(narrow.bins.size()==1&&std::abs(narrow.busy_seconds-duration/2)<1e-12&&narrow.covered_lower_hz==double(config.center_hz)-width/2,"Frequency selection did not expose actual bin edges");
    query={};query.max_observations=0;query.max_events=0;require(a.analyze(query).observations_truncated&&std::abs(a.analyze(query).busy_seconds-duration)<1e-12,"Display cap changed totals");
    query={};query.time_bucket_seconds=std::numeric_limits<double>::quiet_NaN();rejects([&]{a.analyze(query);},"NaN query accepted");
    query={};query.max_observations=2001;rejects([&]{a.analyze(query);},"Unbounded query accepted");
    query={};query.time_bucket_seconds=0.001;const auto fine=a.analyze(query);
    require(fine.observations.size()==2&&(fine.observations[0].quality&ovmesh::SurveyBoundary)&&(fine.observations[1].quality&ovmesh::SurveyBoundary),"Sub-tile power averaging lacks boundary quality");
    const auto boundary_path=directory/"spectrum-bucket-boundary.sqlite";
    {ovmesh::SessionStore writer;writer.create(boundary_path.string(),config,"synthetic-bucket-boundary");auto tile=make_tile(false);tile.elapsed_start_seconds=43*0.1;tile.elapsed_end_seconds=tile.elapsed_start_seconds+duration;tile.utc_start_seconds=1700000000+tile.elapsed_start_seconds;tile.utc_end_seconds=1700000000+tile.elapsed_end_seconds;writer.append(tile);}
    {ovmesh::SessionStore reader;reader.open_readonly(boundary_path.string());query={};query.time_bucket_seconds=0.1;const auto result=reader.analyze(query);require(result.observations.size()==1&&std::abs(result.observed_seconds-duration)<1e-12,"Floating bucket boundary dropped or rejected tile");}
    const auto located=directory/"spectrum-geography.sqlite";
    {
        ovmesh::SessionStore writer;writer.create(located.string(),config,"synthetic-spectrum-geography");
        auto first=make_tile(false);writer.append(first);auto second=make_tile(false,2);second.receiver_end.reset();writer.append(second);
        auto third=make_tile(false,3);third.receiver_end->latitude=20;writer.append(third);
        for(uint64_t id=1;id<=3;++id){ovmesh::SpectrumEvent event;event.id=id;event.first_sample=(id-1)*16384;event.end_sample=id*16384;event.elapsed_start_seconds=double(id-1)*duration;event.elapsed_end_seconds=double(id)*duration;event.utc_start_seconds=1700000000+event.elapsed_start_seconds;event.utc_end_seconds=1700000000+event.elapsed_end_seconds;event.lower_hz=double(config.center_hz)-width/2;event.upper_hz=event.lower_hz+width;event.active_seconds=duration/2;event.mean_dbfs=-60;event.peak_dbfs=-40;event.receiver_start=fix();if(id!=2)event.receiver_end=fix();if(id==3)event.receiver_end->latitude=20;writer.append(event);}
        ovmesh::CoverageGap gap;gap.id=1;gap.elapsed_start_seconds=duration*3;gap.elapsed_end_seconds=duration*4;gap.utc_start_seconds=1700000000+gap.elapsed_start_seconds;gap.utc_end_seconds=1700000000+gap.elapsed_end_seconds;gap.missing_samples=16384;gap.reason="application_drop";writer.append(gap);
    }
    ovmesh::SessionStore geographic;geographic.open_readonly(located.string());query={};query.geographic_filter=true;query.south=-1;query.north=1;query.west=-1;query.east=1;
    const auto region=geographic.analyze(query);require(region.tile_count==1&&region.event_count==1&&std::abs(region.observed_seconds-duration)<1e-12,"Geographic filter included missing or outside positions");
    query={};query.max_events=1;auto all=geographic.analyze(query);require(all.event_count==3&&all.events.size()==1&&all.events_truncated&&all.gaps.size()==1,"Events truncated totals or lost exact gaps");
    require(all.gaps[0].missing_samples==16384&&all.gaps[0].reason=="application_drop","Coverage gap metadata changed");
    const auto csv=directory/"spectrum-redacted.csv";geographic.export_csv(csv.string(),{});const auto csv_data=contents(csv);
    require(csv_data.find("SYNTHETIC_PRIVATE")==std::string::npos&&csv_data.find("0.1234567")==std::string::npos,"Spectrum export disclosed notes or receiver coordinates");
    size_t activity_rows=0,tiles=0,events=0,gaps=0;for(const auto& row:csv_records(csv)){if(row.at("record_type")=="spectrum_activity_run"){++activity_rows;require(!row.at("activity_mask_hex").empty()&&!row.at("activity_frame_count").empty(),"Activity reconstruction metadata missing");}if(row.at("record_type")=="spectrum_tile"){++tiles;require(row.at("mean_centidb_le_hex")=="90e890e8","Quantized power array export changed");}if(row.at("record_type")=="spectrum_event")++events;if(row.at("record_type")=="coverage_gap")++gaps;}
    require(tiles==3&&events==3&&gaps==1&&activity_rows==12,"Export lost full detailed RF records");
    ovmesh::ExportOptions provenance;provenance.include_provenance=true;const auto provenance_csv=directory/"spectrum-provenance.csv";geographic.export_csv(provenance_csv.string(),provenance);
    require(contents(provenance_csv).find("SYNTHETIC_PRIVATE_NOTES")!=std::string::npos&&contents(provenance_csv).find("0.1234567")==std::string::npos,"Independent provenance opt-in failed");
    ovmesh::ExportOptions full;full.include_receiver_positions=true;full.coordinate_decimals=3;
    const auto geo=directory/"spectrum.geojson";geographic.export_geojson(geo.string(),full);require(contents(geo).find("\"record_type\":\"rf_observation\"")!=std::string::npos&&contents(geo).find("\"coordinates\":[-0.765,0.123]")!=std::string::npos,"GeoJSON RF observations or coordinate rounding missing");
    unsigned malformed_index=0;
    for(const std::string& sql:{"UPDATE spectrum_tiles SET activity=X'01000001';","UPDATE spectrum_tiles SET activity=X'01050001';","UPDATE spectrum_tiles SET activity=X'01010001';","UPDATE spectrum_tiles SET activity=X'02010203';","UPDATE spectrum_tiles SET mean_cdb=X'00';","UPDATE spectrum_tiles SET peak_cdb='text';","UPDATE spectrum_tiles SET frame_count=129;","UPDATE spectrum_tiles SET bin_count=4097;","UPDATE spectrum_tiles SET first_center=first_center+1;","UPDATE spectrum_tiles SET elapsed_end=elapsed_start;","UPDATE spectrum_tiles SET activity=X'0080020102';","UPDATE spectrum_tiles SET end_lat=NULL;","UPDATE survey_metrology SET enbw=1;","UPDATE spectrum_tiles SET first_sample=-1;"}) {
        const auto path=directory/("bad-spectrum-"+std::to_string(malformed_index++)+".sqlite");std::filesystem::copy_file(alternate,path);execute_sql(path,sql);ovmesh::SessionStore bad;bad.open_readonly(path.string());rejects([&]{bad.analyze({});},"Malformed saved spectrum accepted");
        rejects([&]{bad.export_csv((directory/("bad-spectrum-"+std::to_string(malformed_index)+".csv")).string(),{});},"Malformed spectrum exported");
    }
    // The reader also accepts original schema 3 and does not invent detail.
    const auto legacy=directory/"spectrum-legacy-v3.sqlite";std::filesystem::copy_file(alternate,legacy);make_legacy(legacy,3);const auto before=contents(legacy);
    {ovmesh::SessionStore old;old.open_readonly(legacy.string());require(!old.analyze({}).detailed_available,"Legacy aggregate-only survey claimed detailed data");rejects([&]{old.append(make_tile(false,2));},"Legacy spectrum reader accepted a write");}
    require(contents(legacy)==before&&schema_version(legacy)==3,"Legacy schema 3 modified");
    const auto quiet=directory/"spectrum-quiet-rle.sqlite";
    {ovmesh::SessionStore writer;writer.create(quiet.string(),config,"synthetic-quiet");auto tile=make_tile(false);tile.frame_count=128;tile.end_sample=128*4096;tile.elapsed_end_seconds=double(tile.end_sample)/config.sample_rate;tile.utc_end_seconds=1700000000+tile.elapsed_end_seconds;tile.activity.assign(128,0);writer.append(tile);}
    {ovmesh::SessionStore reader;reader.open_readonly(quiet.string());const auto result=reader.analyze({});require(result.tile_count==1&&result.observed_seconds>0&&result.busy_seconds==0,"Valid compressed quiet mask failed roundtrip");}
}
void metadata_evidence_storage(const std::filesystem::path& directory) {
    const auto path=directory/"metadata-evidence.sqlite";
    {
        ovmesh::SessionStore writer;writer.create(path.string(),detailed_config(),"synthetic-metadata-evidence");
        for(uint64_t id=1;id<=4;++id){auto record=fixture(id);record.decoded.evidence->port=id==1?1:id==2?5:id==3?70:67;writer.append(record);}
        auto unsupported=fixture(5);unsupported.decoded.status=ovmesh::protocol::Status::unsupported_payload;
        unsupported.decoded.classification="possible Meshtastic";unsupported.decoded.evidence=ovmesh::protocol::EnvelopeEvidence{42,true};writer.append(unsupported);
        auto bad=unsupported;bad.crc_valid=false;rejects([&]{writer.append(bad);},"Bad CRC evidence retained");
        bad=unsupported;bad.header_valid=false;rejects([&]{writer.append(bad);},"Bad header evidence retained");
        for(uint32_t port:{0u,65536u}){bad=unsupported;bad.decoded.evidence->port=port;rejects([&]{writer.append(bad);},"Invalid evidence port retained");}
        bad=unsupported;bad.decoded.status=ovmesh::protocol::Status::no_matching_key;rejects([&]{writer.append(bad);},"Unmatched-key evidence retained");
        bad=unsupported;bad.decoded.evidence.reset();rejects([&]{writer.append(bad);},"Possible classification lacks evidence");
    }
    const auto before=contents(path);
    ovmesh::SessionStore reader;reader.open_readonly(path.string());const auto restored=reader.read();
    require(restored.receptions.size()==5&&restored.classified_receptions==4,"Classification counts include only classified records");
    require(restored.receptions[0].decoded.evidence->port==42&&restored.receptions[0].decoded.classification=="possible Meshtastic","Unsupported envelope evidence preserved");
    const auto exported=directory/"metadata-evidence.csv";reader.export_csv(exported.string(),{});
    for(const auto& row:csv_records(exported))assert_metadata_columns(row);
    require(contents(path)==before,"Metadata reading/export changed original");
    unsigned bad_index=0;
    for(const std::string& mutation:{"UPDATE receptions SET evidence_port=NULL WHERE id=5;","UPDATE receptions SET evidence_signature_present=2 WHERE id=5;",
        "UPDATE receptions SET evidence_port=-1 WHERE id=5;","UPDATE receptions SET evidence_port=4294967296 WHERE id=5;",
        "UPDATE receptions SET evidence_port=0 WHERE id=5;","UPDATE receptions SET evidence_port=65536 WHERE id=5;"}) {
        const auto malformed=directory/("bad-evidence-"+std::to_string(bad_index++)+".sqlite");std::filesystem::copy_file(path,malformed);
        execute_sql(malformed,mutation);ovmesh::SessionStore rejected;rejected.open_readonly(malformed.string());
        rejects([&]{rejected.read();},"Malformed evidence accepted");
        rejects([&]{rejected.export_csv((directory/("bad-export-"+std::to_string(bad_index)+".csv")).string(),{});},"Malformed evidence exported");
    }
    // Oversized and malformed semantic fields intentionally would fail the old
    // reader. Metadata projection must never load or validate those values.
    for(const int version:{1,2,3,4,5,6}) {
        const auto legacy=directory/("legacy-private-v"+std::to_string(version)+".sqlite");
        auto config=detailed_config();config.compact_recording=version==6;
        {ovmesh::SessionStore writer;writer.create(legacy.string(),config,"synthetic-legacy-private");writer.append(fixture());writer.append(fix());}
        make_legacy(legacy,version);
        execute_sql(legacy,"UPDATE receptions SET profile='SYNTHETIC_PRIVATE_PROFILE',origin=286331153,destination=572662306,packet_id=858993459,"
            "text='SYNTHETIC_PRIVATE_MESSAGE',node_id='SYNTHETIC_PRIVATE_ID',long_name='SYNTHETIC_PRIVATE_NAME',short_name='SECRET',"
            "latitude=999,longitude=999,voltage=X'ff',temperature='SYNTHETIC_PRIVATE_TELEMETRY';"
            "INSERT INTO routes VALUES(1,999999,'SYNTHETIC_PRIVATE_ROUTE');");
        if(version>=3)execute_sql(legacy,"INSERT INTO route_details VALUES(1,'SYNTHETIC_PRIVATE_ROUTE_BACK',999999,X'ff');UPDATE receptions SET routing_variant='SYNTHETIC_PRIVATE_ROUTING',request_id=X'ff';");
        const auto legacy_before=contents(legacy);
        ovmesh::SessionStore old;old.open_readonly(legacy.string());const auto saved=old.read();
        require(saved.total_receptions==1&&saved.classified_receptions==1&&saved.receptions.size()==1&&saved.track.size()==1,"Legacy RF/GPS/counts lost");
        auto expected=fixture();if(version<3)expected.decoded.evidence.reset();metadata_equal(saved.receptions[0],expected);
        size_t n=0;old.visit_receptions([&](const auto& r){metadata_equal(r,expected);++n;});require(n==1,"Legacy visitor lost metadata");
        const auto csv=directory/("legacy-private-v"+std::to_string(version)+".csv");
        ovmesh::ExportOptions options;options.include_receiver_positions=true;options.include_provenance=true;old.export_csv(csv.string(),options);
        require(contents(csv).find("SYNTHETIC_PRIVATE_")==std::string::npos,"Legacy semantic fields leaked through archive");
        for(const auto& row:csv_records(csv))assert_metadata_columns(row);
        const auto geo=directory/("legacy-private-v"+std::to_string(version)+".geojson");old.export_geojson(geo.string(),options);
        require(contents(geo).find("SYNTHETIC_PRIVATE_")==std::string::npos,"Legacy semantic fields leaked through GeoJSON");
        const auto copy=directory/("legacy-copy-v"+std::to_string(version)+".sqlite");
        rejects([&]{old.save_copy(copy.string());},"Unmarked historical content copied");require(!std::filesystem::exists(copy),"Rejected old copy left destination");
        require(contents(legacy)==legacy_before,"Legacy private source changed");
    }
    // A marked file is not proof that externally added data is harmless. Rebuild
    // from allowed values, dropping live reserved fields and free-page remnants.
    const auto injected=directory/"marked-injected.sqlite";std::filesystem::copy_file(path,injected);
    execute_sql(injected,"PRAGMA secure_delete=OFF;UPDATE receptions SET text='SYNTHETIC_PRIVATE_LIVE',origin=286331153;"
        "INSERT INTO routes VALUES(1,0,'SYNTHETIC_PRIVATE_ROUTE');"
        "INSERT INTO route_details VALUES(1,'SYNTHETIC_PRIVATE_FREED',0,zeroblob(200000));"
        "DELETE FROM route_details;");
    require(contents(injected).find("SYNTHETIC_PRIVATE_FREED")!=std::string::npos,"Deleted-content fixture must leave identifiable remnants");
    const auto injected_before=contents(injected);ovmesh::SessionStore marked;marked.open_readonly(injected.string());
    const auto clean=directory/"reconstructed-copy.sqlite";marked.save_copy(clean.string());
    require(contents(clean).find("SYNTHETIC_PRIVATE_")==std::string::npos,"Logical copy included private live values or freed pages");
    require(contents(injected)==injected_before,"Reconstruction modified its source");
    ovmesh::SessionStore copied;copied.open_readonly(clean.string());require(copied.read().total_receptions==5,"Reconstruction lost receptions");
    size_t n=0;copied.visit_receptions([&](const auto& r){require(r.frequency_hz==fixture().frequency_hz&&r.receiver_position->latitude==fix().latitude,"Reconstruction changed RF/GPS values");++n;});require(n==5,"Reconstructed full metadata history lost");
    for(const std::string& mutation:{"UPDATE metadata_policy SET policy='unknown';","DELETE FROM metadata_policy;","INSERT INTO metadata_policy VALUES(2,'metadata-only-v1');","ALTER TABLE metadata_policy ADD COLUMN extra TEXT;"}) {
        const auto bad=directory/("bad-policy-"+std::to_string(bad_index++)+".sqlite");std::filesystem::copy_file(path,bad);execute_sql(bad,mutation);
        rejects([&]{ovmesh::SessionStore invalid;invalid.open_readonly(bad.string());},"Malformed metadata policy accepted");
    }
}

}

int main() {
    try {
        auto root=std::filesystem::current_path();
        while(!std::filesystem::exists(root/"CMakeLists.txt") && root.has_parent_path() && root!=root.parent_path())root=root.parent_path();
        require(std::filesystem::exists(root/"include/ovmesh/engine.hpp"),"Run tests from within this repository");
        const auto unique=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto directory=root/"build/publication-review"/("storage-"+unique);std::filesystem::create_directories(directory);
        calibration_storage(directory);
        rtl_receiver_storage(directory);
        spectrum_storage(directory);
        metadata_evidence_storage(directory);
        const auto path=directory/"roundtrip.sqlite";
        ovmesh::ReceiverConfig config;config.compact_recording=false;config.session_title="Synthetic storage fixture";
        ovmesh::Snapshot state;state.config=config;state.elapsed_seconds=100;state.input_seconds=98;state.measurement_seconds=1;
        state.delivered_samples=1000;state.dropped_samples=12;state.total_receptions=600;state.classified_receptions=599;
        state.frequencies={{906875000,250000,-80,-20,1,0.25},{907125000,250000,-120,-120,0,0}};
        state.lane_health.push_back({"Synthetic profile",906875000,98,600,599,1,2,"stopped",{}});
        {
            ovmesh::SessionStore store;store.create(path.string(),config,"synthetic-session");
            auto bad=fixture();bad.crc_valid=false;rejects([&]{store.append(bad);},"Bad CRC content retained");
            bad=fixture();bad.decoded.status=ovmesh::protocol::Status::no_matching_key;rejects([&]{store.append(bad);},"Unauthorized content retained");
            bad=fixture();bad.receiver_position->latitude=91;rejects([&]{store.append(bad);},"Invalid receiver coordinates retained");
            bad=fixture();bad.snr_db=std::numeric_limits<double>::quiet_NaN();rejects([&]{store.append(bad);},"NaN retained");
            bad=fixture();bad.decoded.authentication="authenticated sender";rejects([&]{store.append(bad);},"Unjustified authentication claim retained");
            for(uint64_t i=1;i<=599;++i)store.append(fixture(i));
            auto unknown=fixture(600);unknown.decoded.status=ovmesh::protocol::Status::no_matching_key;
            unknown.decoded.evidence.reset();
            unknown.decoded.classification="SYNTHETIC_UNAPPROVED_METADATA";store.append(unknown);store.append(fix());
            ovmesh::SurveyWindow window;window.id=1;window.utc_start_seconds=1700000000;window.utc_end_seconds=1700000005;
            window.elapsed_start_seconds=0;window.elapsed_end_seconds=5;window.frequencies=state.frequencies;window.receiver_position=fix();
            auto bad_window=window;bad_window.elapsed_end_seconds=0;rejects([&]{store.append(bad_window);},"Backward window timing accepted");
            bad_window=window;bad_window.utc_end_seconds=bad_window.utc_start_seconds;rejects([&]{store.append(bad_window);},"Empty window timing accepted");
            bad_window=window;bad_window.receiver_position->longitude=181;rejects([&]{store.append(bad_window);},"Bad window GPS accepted");
            bad_window=window;bad_window.frequencies[0].active_seconds=2;rejects([&]{store.append(bad_window);},"Window activity exceeds observations");
            bad_window=window;bad_window.frequencies.push_back(bad_window.frequencies[0]);rejects([&]{store.append(bad_window);},"Duplicate window bins accepted");
            store.append(window);rejects([&]{store.append(window);},"Duplicate window overwritten");
            window.id=2;window.utc_start_seconds+=5;window.utc_end_seconds+=5;window.elapsed_start_seconds=5;window.elapsed_end_seconds=10;
            window.receiver_position.reset();store.append(window);
            store.update(state,false);require(store.read().incomplete,"In-progress session marked complete");
            state.incomplete=true;store.update(state,true);require(store.read().incomplete,"Failed final session marked complete");
            state.incomplete=false;store.update(state,true);
        }
#ifndef _WIN32
        struct stat mode{};require(::stat(path.c_str(),&mode)==0 && (mode.st_mode&0777)==0600,"Session permissions not private");
#endif
        const auto before=contents(path);
        require(before.find("SYNTHETIC_UNAPPROVED_METADATA")==std::string::npos,"Arbitrary classification text persisted");
        {ovmesh::SessionStore collision;rejects([&]{collision.create(path.string(),config,"other");},"Existing session overwritten");}
        require(contents(path)==before,"Existing session contents changed");
        ovmesh::SessionStore store;store.open_readonly(path.string());const auto restored=store.read();
        require(!restored.incomplete && restored.total_receptions==600 && restored.classified_receptions==599,"Complete counts wrong");
        require(restored.receptions.size()==512,"Bounded UI history wrong");metadata_equal(restored.receptions[1],fixture(599));
        require(restored.track.size()==1 && restored.track[0].monotonic_seconds==fix().monotonic_seconds,"Track timing lost");
        require(restored.lane_health.size()==1 && restored.lane_health[0].resets==2,"Lane health lost");
        const auto copied_path=directory/"saved-copy.sqlite";store.save_copy(copied_path.string());
        require(!std::filesystem::exists(copied_path.string()+"-wal")&&!std::filesystem::exists(copied_path.string()+"-shm"),
            "Save copy is a standalone database without required WAL sidecars");
        {
            ovmesh::SessionStore copied;copied.open_readonly(copied_path.string());const auto result=copied.read();
            require(result.total_receptions==restored.total_receptions&&result.classified_receptions==restored.classified_receptions&&
                result.session_id==restored.session_id&&!result.incomplete,"Save copy preserves complete session metadata");
            metadata_equal(result.receptions[1],fixture(599));
            size_t count=0;copied.visit_receptions([&](const auto&){++count;});
            require(count==600,"Save copy contains full saved history beyond the 512-row UI limit");
        }
        require(contents(path)==before,"Save copy changed its historical source");
        const auto copied_before=contents(copied_path);
        rejects([&]{store.save_copy(copied_path.string());},"Save copy overwrote an existing destination");
        require(contents(copied_path)==copied_before,"Failed copy modified an existing file");
        rejects([&]{store.save_copy("relative-copy.sqlite");},"Save copy accepted a relative path");
        rejects([&]{store.save_copy((directory/"missing"/"copy.sqlite").string());},"Save copy created missing parent directories");
#ifndef _WIN32
        struct stat copy_mode{};require(::stat(copied_path.c_str(),&copy_mode)==0&&(copy_mode.st_mode&0777)==0600,
            "Saved copies retain private owner-only permissions");
        const auto copied_alias=directory/"copy-alias.sqlite";std::filesystem::create_symlink(copied_path,copied_alias);
        rejects([&]{store.save_copy(copied_alias.string());},"Save copy followed a destination symlink");
        require(contents(copied_path)==copied_before,"Rejected symlink copy changed its target");
#endif
        const auto redacted=directory/"redacted.csv";store.export_csv(redacted.string(),{});
        const auto redacted_bytes=contents(redacted);require(redacted_bytes.find("SYNTHETIC_NAME")==std::string::npos && redacted_bytes.find("0.1234567")==std::string::npos,"Default export disclosed content or GPS");
        const auto redacted_rows=csv_records(redacted);size_t receptions=0,window_bins=0;bool unavailable=false;
        for(const auto& row:redacted_rows) {
            if(row.at("record_type")=="reception"){
                ++receptions;assert_metadata_columns(row);require(row.at("receiver_latitude").empty(),"Default GPS redaction failed");
                if(row.at("classification")=="likely Meshtastic")require(row.at("evidence_port")=="1" && row.at("evidence_signature_present")=="1","Approved envelope evidence redacted unexpectedly");
            }
            if(row.at("record_type")=="frequency" && row.at("observed_seconds")=="0")unavailable=row.at("occupancy_fraction").empty();
            if(row.at("record_type")=="survey_window_bin") {
                ++window_bins;require(row.at("receiver_latitude").empty(),"Window export disclosed GPS/content by default");
                require(row.at("classification")=="sampled RF activity" && row.at("position_association")=="window-end","Unknown RF/window association lost");
                require(!row.at("window_utc_start_seconds").empty() && !row.at("window_elapsed_end_seconds").empty(),"Window timing absent");
            }
        }
        require(receptions==600,"Export used recent UI subset");require(window_bins==4,"Window RF bins missing");require(unavailable,"Zero observation became zero occupancy");
        ovmesh::ExportOptions full;full.include_receiver_positions=true;full.coordinate_decimals=3;
        const auto full_path=directory/"full.csv";store.export_csv(full_path.string(),full);const auto full_rows=csv_records(full_path);
        bool located_reception=false;
        for(const auto& row:full_rows){assert_metadata_columns(row);if(row.at("record_type")=="reception") {
            require(row.at("receiver_latitude")=="0.123"&&row.at("receiver_longitude")=="-0.765","GPS precision failed");located_reception=true;
        }}
        require(located_reception,"No reception metadata exported");
        require(ovmesh::csv_text("  =SYNTHETIC()") == "\"'  =SYNTHETIC()\"","CSV formula neutralization failed");
        bool located_window=false,unlocated_window=false;
        for(const auto& row:full_rows)if(row.at("record_type")=="survey_window_bin") {
            if(row.at("window_id")=="1")located_window=row.at("receiver_latitude")=="0.123" && row.at("receiver_longitude")=="-0.765";
            if(row.at("window_id")=="2")unlocated_window=row.at("receiver_latitude").empty() && row.at("receiver_longitude").empty();
        }
        require(located_window && unlocated_window,"Window end GPS inclusion or missing-position behavior wrong");
        rejects([&]{store.export_csv(full_path.string(),full);},"Existing export overwritten");
        rejects([&]{store.export_geojson((directory/"redacted.geojson").string(),{});},"GeoJSON allowed without GPS opt-in");
        full.coordinate_decimals=8;rejects([&]{store.export_csv((directory/"bad-precision.csv").string(),full);},"Invalid coordinate precision accepted");full.coordinate_decimals=3;
        const auto geo=directory/"track.geojson";store.export_geojson(geo.string(),full);
        require(contents(geo).find("\"coordinates\":[-0.765,0.123]")!=std::string::npos && contents(geo).find("\\\"GPS\\\"\\u000afixture")!=std::string::npos,"GeoJSON coordinates/escaping failed");
        for(const auto& invalid:{std::string("//server/share/file.sqlite"),std::string("\\\\server\\share\\file.sqlite"),std::string("file:///tmp/file"),std::string("/Volumes/no-access/file"),std::string("relative.sqlite")}) {
            ovmesh::SessionStore denied;rejects([&]{denied.open_readonly(invalid);},"Nonlocal or relative path accepted");
        }
#ifndef _WIN32
        const auto alias=directory/"alias.sqlite";std::filesystem::create_symlink(path,alias);
        ovmesh::SessionStore symlink;rejects([&]{symlink.open_readonly(alias.string());},"Symlink session opened");
#endif
        const auto foreign=directory/"foreign.sqlite";execute_sql(foreign,"PRAGMA application_id=1331055940;PRAGMA user_version=1;CREATE TABLE unrelated(value TEXT);");
        ovmesh::SessionStore invalid;rejects([&]{invalid.open_readonly(foreign.string());},"Foreign schema accepted");require(!invalid.is_open(),"Failed database handle retained");
        const auto hostile=directory/"hostile.sqlite";std::filesystem::copy_file(path,hostile);
        execute_sql(hostile,"CREATE VIEW unapproved AS SELECT * FROM receptions;");
        rejects([&]{invalid.open_readonly(hostile.string());},"Unexpected view accepted");
        const auto malformed=directory/"malformed.sqlite";std::filesystem::copy_file(path,malformed);
        execute_sql(malformed,"UPDATE receptions SET receiver_latitude=100;");
        invalid.open_readonly(malformed.string());rejects([&]{invalid.read();},"Malformed saved coordinates accepted");
        const auto bad_windows=directory/"malformed-windows.sqlite";std::filesystem::copy_file(path,bad_windows);
        execute_sql(bad_windows,"UPDATE survey_windows SET utc_end=utc_start;");
        ovmesh::SessionStore bad_window_store;bad_window_store.open_readonly(bad_windows.string());
        rejects([&]{bad_window_store.export_csv((directory/"bad-window-export.csv").string(),{});},"Malformed saved window timing exported");
        std::cout<<"Storage safety, complete export, typed roundtrip, privacy, and malformed-input tests passed.\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"Storage test failed: "<<error.what()<<'\n';return 1;}
}
