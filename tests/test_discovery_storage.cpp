// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic typed metadata only. No hardware, operational positions, IQ or payloads.
#include "storage.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
unsigned checks=0;
void require(bool value,const std::string& message) {
    ++checks;if(!value)throw std::runtime_error(message);
}
template<typename F> void rejects(F action,const std::string& message) {
    bool threw=false;try{action();}catch(const std::exception&){threw=true;}require(threw,message);
}
std::string contents(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};
}
void sql(const std::filesystem::path& path,const std::string& command) {
    sqlite3* db=nullptr;require(sqlite3_open(path.string().c_str(),&db)==SQLITE_OK,"fixture database open");
    const int result=sqlite3_exec(db,command.c_str(),nullptr,nullptr,nullptr);sqlite3_close(db);
    require(result==SQLITE_OK,"fixture SQL: "+command);
}
using Record=std::map<std::string,std::string>;
std::vector<Record> csv(const std::filesystem::path& path) {
    std::vector<std::vector<std::string>> rows;std::vector<std::string> row;std::string field;bool quoted=false;
    const auto input=contents(path);
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
    require(!quoted&&field.empty()&&row.empty()&&!rows.empty(),"CSV well formed");
    std::vector<Record> out;
    for(size_t r=1;r<rows.size();++r) {
        require(rows[r].size()==rows[0].size(),"CSV stable columns");Record record;
        for(size_t c=0;c<rows[0].size();++c)record.emplace(rows[0][c],rows[r][c]);
        out.push_back(std::move(record));
    }
    return out;
}
ovmesh::ReceiverConfig config() {
    ovmesh::ReceiverConfig c;c.compact_recording=false;c.discover_lora=true;c.center_hz=907500000;c.sample_rate=8000000;
    c.survey_span_hz=5000000;c.lanes.clear();return c;
}
ovmesh::PositionFix fix() {
    ovmesh::PositionFix f;f.valid=true;f.latitude=.1234567;f.longitude=-.7654321;f.altitude_m=42.25;
    f.utc_seconds=1700000000.;f.monotonic_seconds=100.;f.manual=false;f.source="PRIVATE_SYNTHETIC_FIX";
    f.hdop=1.25;f.satellites=8;return f;
}
ovmesh::WaveformObservation waveform(uint64_t id=1) {
    ovmesh::WaveformObservation w;w.id=id;w.center_hz=906875000.;w.bandwidth_hz=250000;w.spreading_factor=11;
    w.first_observed_elapsed=.125;w.delimiter_elapsed=.3;w.delimiter_utc=1700000000.3;
    w.up_match=.8;w.down_match=.7;w.contributing_subbands=1;w.complete_in_requested_range=true;
    w.receiver_position=fix();return w;
}
ovmesh::Snapshot snapshot() {
    ovmesh::Snapshot s;s.config=config();s.elapsed_seconds=2;s.input_seconds=1;s.delivered_samples=8000000;
    auto& d=s.discovery;d.enabled=true;d.finished=true;d.accepted_input_samples=8000000;d.rejected_input_samples=32;
    d.channelized_input_samples=7000000;d.abandoned_input_samples=1000000;d.source_queue_drops=2;d.stream_resets=3;
    d.result_overflows=4;d.gap_overflows=5;d.observations=2;
    d.bands.push_back({0,906500000.,1700000,300000,32,7,8});
    d.bands.push_back({1,907500000.,1600000,400000,32,9,10});
    return s;
}
void make_base(const std::filesystem::path& path) {
    ovmesh::SessionStore writer;writer.create(path.string(),config(),"synthetic-waveform-storage");
    const auto initial=writer.read();require(initial.config.discover_lora&&initial.discovery.enabled,"initial enabled state persists");
    auto w=waveform();writer.append(w);
    auto other=w;other.id=2;other.center_hz=908750000.;other.bandwidth_hz=500000;other.first_observed_elapsed=.4;
    other.delimiter_elapsed=.55;other.delimiter_utc=1700000000.55;other.receiver_position.reset();writer.append(other);
    w.center_hz+=27.;w.up_match=.9;w.down_match=.85;w.contributing_subbands=2;w.association_ambiguous=true;writer.append(w);
    writer.append(ovmesh::DiscoveryGap{1,8000000,8000032,-1,"source_queue_full"});
    writer.append(ovmesh::DiscoveryGap{2,8000100,8000200,1,"processing_failure"});
    writer.update(snapshot(),true);
}
void roundtrip_and_exports(const std::filesystem::path& directory,const std::filesystem::path& path) {
    ovmesh::SessionStore reader;reader.open_readonly(path.string());const auto out=reader.read();
    require(out.config.discover_lora&&out.discovery.enabled&&out.discovery.finished&&!out.discovery.failed,"status flags");
    require(out.waveforms.size()==2&&out.waveforms[0].id==2&&out.waveforms[1].id==1,"upsert is one row; recent descending");
    const auto& w=out.waveforms[1];
    require(w.center_hz==906875027.&&w.bandwidth_hz==250000&&w.spreading_factor==11&&w.contributing_subbands==2&&w.association_ambiguous,"upsert retained fields");
    require(w.first_observed_elapsed==.125&&w.delimiter_elapsed==.3&&w.delimiter_utc==1700000000.3&&w.up_match==.9&&w.down_match==.85,"evidence time and match roundtrip");
    require(w.receiver_position&&w.receiver_position->source==fix().source&&w.receiver_position->latitude==fix().latitude&&w.receiver_position->hdop==fix().hdop,"full receiver position roundtrip");
    require(!out.waveforms[0].receiver_position,"missing position remains missing");
    const auto& d=out.discovery;
    require(d.method=="lora-preamble-v1"&&d.accepted_input_samples==8000000&&d.rejected_input_samples==32&&d.channelized_input_samples==7000000&&d.abandoned_input_samples==1000000,"distinct input work counters");
    require(d.source_queue_drops==2&&d.stream_resets==3&&d.result_overflows==4&&d.gap_overflows==5&&d.observations==2,"loss and observation counters");
    require(d.bands.size()==2&&d.bands[1].subband_index==1&&d.bands[1].center_hz==907500000.&&d.bands[1].processed_samples==1600000&&d.bands[1].abandoned_samples==400000&&d.bands[1].source_gap_input_samples==32&&d.bands[1].candidate_limit_hits==9&&d.bands[1].track_limit_hits==10,"per-band counters and units");
    rejects([&]{reader.append(waveform(3));},"readonly waveform write rejected");
    rejects([&]{reader.append(ovmesh::DiscoveryGap{3,1,2,-1,"input_discontinuity"});},"readonly gap write rejected");
    rejects([&]{reader.update(snapshot());},"readonly checkpoint rejected");
    const auto redacted=directory/"waveforms-redacted.csv";reader.export_csv(redacted.string(),{});
    require(contents(redacted).find("PRIVATE_SYNTHETIC_FIX")==std::string::npos&&contents(redacted).find("0.1234567")==std::string::npos,"CSV default GPS redaction");
    unsigned waves=0,bands=0,gaps=0,status=0;
    for(const auto& r:csv(redacted)) {
        if(r.at("record_type")=="session")require(r.at("session_schema_version")=="5","schema marker");
        if(r.at("record_type")=="waveform_observation") {
            ++waves;require(!r.at("inferred_bandwidth_hz").empty()&&!r.at("inferred_spreading_factor").empty(),"explicit inferred settings");
            for(const auto* field:{"duration_seconds","active_seconds","occupancy_fraction","text","origin","packet_id","authentication","receiver_latitude","receiver_source","bandwidth_hz"})require(r.at(field).empty(),std::string("waveform must not invent packet/payload/GPS field: ")+field);
            require(r.at("classification").find("no packet decode")!=std::string::npos&&r.at("time_association").find("not packet airtime")!=std::string::npos,"waveform interpretation");
        }
        if(r.at("record_type")=="discovery_band_coverage"){++bands;require(r.at("discovery_output_sample_rate")=="2000000"&&r.at("sample_rate")=="8000000","explicit output/input units");}
        if(r.at("record_type")=="discovery_gap") {++gaps;require(!r.at("gap_reason").empty()&&!r.at("discovery_gap_end_input_sample").empty(),"gap evidence exported");}
        if(r.at("record_type")=="discovery_status") {++status;require(r.at("discovery_result_overflows")=="4"&&r.at("discovery_gap_overflows")=="5","overflow exported separately");}
    }
    require(waves==2&&bands==2&&gaps==2&&status==1,"all durable discovery evidence exported");
    ovmesh::ExportOptions options;options.include_receiver_positions=true;options.coordinate_decimals=3;
    const auto located=directory/"waveforms-located.csv";reader.export_csv(located.string(),options);
    bool rounded=false;for(const auto& r:csv(located))if(r.at("record_type")=="waveform_observation"&&r.at("waveform_id")=="1")rounded=r.at("receiver_latitude")=="0.123"&&r.at("receiver_longitude")=="-0.765";
    require(rounded,"GPS precision honored");
    rejects([&]{reader.export_geojson((directory/"no-gps.geojson").string(),{});},"GeoJSON explicit GPS opt-in required");
    const auto geo=directory/"waveforms.geojson";reader.export_geojson(geo.string(),options);const auto json=contents(geo);
    require(json.find("\"coordinates\":[-0.765,0.123]")!=std::string::npos,"GeoJSON rounding");
    require(json.find("\"geometry\":null,\"properties\":{\"record_type\":\"waveform_observation\"")!=std::string::npos,"unlocated waveform exported as null geometry");
    require(json.find("\"record_type\":\"discovery_gap\"")!=std::string::npos&&json.find("\"record_type\":\"discovery_band_coverage\"")!=std::string::npos&&json.find("\"discovery_result_overflows\":4")!=std::string::npos,"GeoJSON retains coverage limitations");
    require(json.find("\"duration_seconds\"")==std::string::npos&&json.find("\"text\"")==std::string::npos,"GeoJSON no packet airtime or payload");
}
void analysis(const std::filesystem::path& path) {
    ovmesh::SessionStore reader;reader.open_readonly(path.string());
    auto full=reader.analyze({});require(full.waveform_count==2&&full.waveforms.size()==2&&full.observed_seconds==0&&full.busy_seconds==0,"waveforms independent from occupancy");
    ovmesh::SurveyQuery q;q.lower_hz=906750000;q.upper_hz=907000000;
    auto narrow=reader.analyze(q);require(narrow.waveform_count==1&&narrow.waveforms[0].id==1,"inferred RF footprint selection");
    q.lower_hz=906990000;q.upper_hz=907010000;require(reader.analyze(q).waveform_count==1,"partial footprint overlap selected");
    q={};q.elapsed_start=.3;q.elapsed_end=.55;require(reader.analyze(q).waveform_count==1&&reader.analyze(q).waveforms[0].id==1,"delimiter half-open interval");
    q.elapsed_start=.31;q.elapsed_end=.54;require(reader.analyze(q).waveform_count==0,"overlapping preamble is not packet airtime selection");
    q={};q.geographic_filter=true;q.south=.12;q.north=.13;q.west=-.77;q.east=-.76;
    require(reader.analyze(q).waveform_count==1,"GPS rectangle excludes missing position");
    q.south=1;q.north=2;require(reader.analyze(q).waveform_count==0,"GPS rectangle excludes distant fix");
    q={};q.max_events=0;const auto hidden=reader.analyze(q);require(hidden.waveform_count==2&&hidden.waveforms.empty()&&hidden.waveforms_truncated,"count independent from bounded display");
}
void bounded_history(const std::filesystem::path& directory) {
    const auto path=directory/"bounded.sqlite";
    {
        ovmesh::SessionStore writer;writer.create(path.string(),config(),"synthetic-bounded");
        for(uint64_t id=1;id<=300;++id){auto w=waveform(id);w.receiver_position.reset();writer.append(w);}
        auto s=snapshot();s.discovery.observations=300;writer.update(s,true);
    }
    {
        ovmesh::SessionStore reader;reader.open_readonly(path.string());const auto out=reader.read();
        require(out.waveforms.size()==256&&out.waveforms.front().id==300&&out.waveforms.back().id==45&&out.discovery.observations==300,"bounded last256 readback");
        ovmesh::SurveyQuery q;q.max_events=3;const auto a=reader.analyze(q);require(a.waveform_count==300&&a.waveforms.size()==3&&a.waveforms_truncated&&a.waveforms[0].id==300,"bounded analysis counts all");
        const auto export_path=directory/"bounded.csv";reader.export_csv(export_path.string(),{});unsigned rows=0;
        for(const auto& r:csv(export_path))rows+=r.at("record_type")=="waveform_observation";
        require(rows==300,"export is not capped to display history");
    }
    sql(path,"UPDATE waveform_observations SET center_hz='malformed' WHERE id=1;");
    ovmesh::SessionStore reader;reader.open_readonly(path.string());require(reader.read().waveforms.size()==256,"bounded recent read does not scan old history");
    rejects([&]{reader.analyze({});},"analysis validates old waveform rows");
    rejects([&]{reader.export_csv((directory/"malformed-old.csv").string(),{});},"export validates old waveform rows");
}
void invalid_writes(const std::filesystem::path& directory) {
    const auto path=directory/"invalid-writes.sqlite";ovmesh::SessionStore writer;writer.create(path.string(),config(),"synthetic-rejects");writer.append(waveform());
    const std::vector<std::function<void(ovmesh::WaveformObservation&)>> mutations{
        [](auto& w){w.id=0;},[](auto& w){w.id=UINT64_MAX;},[](auto& w){w.center_hz=NAN;},[](auto& w){w.center_hz=1;},
        [](auto& w){w.bandwidth_hz=200000;},[](auto& w){w.spreading_factor=6;},[](auto& w){w.spreading_factor=13;},
        [](auto& w){w.first_observed_elapsed=-1;},[](auto& w){w.delimiter_elapsed=w.first_observed_elapsed;},[](auto& w){w.delimiter_utc=INFINITY;},
        [](auto& w){w.up_match=1.001;},[](auto& w){w.up_match=0;},[](auto& w){w.down_match=-.001;},[](auto& w){w.down_match=NAN;},
        [](auto& w){w.contributing_subbands=0;},[](auto& w){w.contributing_subbands=33;},
        [](auto& w){w.center_hz=905000000.;w.complete_in_requested_range=true;},
        [](auto& w){w.receiver_position->latitude=91;},[](auto& w){w.receiver_position->valid=false;}};
    for(const auto& mutate:mutations){auto w=waveform(3);mutate(w);rejects([&]{writer.append(w);},"invalid waveform write");}
    auto changed=waveform();changed.bandwidth_hz=500000;rejects([&]{writer.append(changed);},"existing identity cannot change profile");
    auto tolerated=waveform(3);tolerated.up_match=1.000009;writer.append(tolerated);
    for(const auto& g:std::vector<ovmesh::DiscoveryGap>{{0,1,2,-1,"source_queue_full"},{2,1,1,-1,"source_queue_full"},{2,2,1,-1,"source_queue_full"},{2,0,(uint64_t{1}<<53)+1,-1,"source_queue_full"},{2,1,2,-2,"source_queue_full"},{2,1,2,32,"source_queue_full"},{2,1,2,-1,"payload bytes"}})
        rejects([&]{writer.append(g);},"invalid gap write");
    auto s=snapshot();s.discovery.result_overflows=UINT64_MAX;rejects([&]{writer.update(s);},"counter signed overflow");
    s=snapshot();s.discovery.enabled=false;rejects([&]{writer.update(s);},"status must match recorded enablement");
    s=snapshot();s.discovery.bands.push_back(s.discovery.bands[0]);rejects([&]{writer.update(s);},"duplicate subband rejected");
    s=snapshot();s.discovery.bands[0].subband_index=32;rejects([&]{writer.update(s);},"subband index bound");
    s=snapshot();s.discovery.method="other";rejects([&]{writer.update(s);},"unsupported method rejected");
    s=snapshot();s.discovery.fault=std::string(161,'x');rejects([&]{writer.update(s);},"bounded fault");
    writer.update(snapshot(),true);require(writer.read().waveforms.size()==2,"rejected writes preserve prior valid records");
    const auto disabled=directory/"disabled.sqlite";ovmesh::SessionStore off;auto c=config();c.discover_lora=false;off.create(disabled.string(),c,"synthetic-disabled");
    require(!off.read().config.discover_lora&&!off.read().discovery.enabled,"disabled state persists");
    rejects([&]{off.append(waveform());},"disabled waveform rejected");
    rejects([&]{off.append(ovmesh::DiscoveryGap{1,1,2,-1,"source_queue_full"});},"disabled gap rejected");
}
void hostile_inputs(const std::filesystem::path& directory,const std::filesystem::path& base) {
    unsigned n=0;
    for(const auto& mutation:std::vector<std::string>{
        "UPDATE session SET discover_lora=2", "UPDATE waveform_observations SET id=0 WHERE id=1",
        "UPDATE waveform_observations SET center_hz='NaN'", "UPDATE waveform_observations SET center_hz=1e999",
        "UPDATE waveform_observations SET bandwidth_hz=200000", "UPDATE waveform_observations SET spreading_factor=11.5",
        "UPDATE waveform_observations SET contributing_subbands=33", "UPDATE waveform_observations SET up_match=-1", "UPDATE waveform_observations SET down_match=0",
        "UPDATE waveform_observations SET down_match=x'00'", "UPDATE waveform_observations SET delimiter_elapsed=NULL",
        "UPDATE waveform_observations SET association_ambiguous=2", "UPDATE waveform_observations SET first_observed_elapsed=delimiter_elapsed",
        "UPDATE waveform_observations SET receiver_lat=NULL WHERE id=1", "UPDATE waveform_observations SET receiver_lon=181 WHERE id=1",
        "UPDATE discovery_status SET enabled=0", "UPDATE discovery_status SET result_overflows=-1",
        "UPDATE discovery_status SET observations=1e30", "UPDATE discovery_status SET method='unsupported'",
        "UPDATE discovery_status SET fault=NULL", "UPDATE discovery_status SET fault=zeroblob(100)", "UPDATE discovery_status SET fault=replace(hex(zeroblob(100)),'0','X')",
        "INSERT INTO discovery_status SELECT 2,enabled,finished,failed,method,fault,accepted_input_samples,rejected_input_samples,channelized_input_samples,abandoned_input_samples,source_queue_drops,stream_resets,result_overflows,gap_overflows,observations FROM discovery_status",
        "DELETE FROM discovery_status", "UPDATE discovery_bands SET subband_index=32 WHERE subband_index=0",
        "UPDATE discovery_bands SET processed_samples=-1", "UPDATE discovery_bands SET center_hz='bad'"}) {
        const auto path=directory/("hostile-row-"+std::to_string(n++)+".sqlite");std::filesystem::copy_file(base,path);sql(path,mutation);
        ovmesh::SessionStore reader;reader.open_readonly(path.string());rejects([&]{reader.read();},"hostile typed row rejected: "+mutation);
    }
    for(const auto& mutation:std::vector<std::string>{
        "ALTER TABLE waveform_observations ADD COLUMN payload BLOB",
        "ALTER TABLE discovery_bands ADD COLUMN hidden_value AS (processed_samples+1)",
        "CREATE TRIGGER injected AFTER INSERT ON discovery_status BEGIN DELETE FROM waveform_observations; END",
        "CREATE VIEW injected AS SELECT * FROM waveform_observations", "DROP TABLE discovery_gaps", "PRAGMA user_version=6",
        "PRAGMA writable_schema=ON;UPDATE sqlite_schema SET sql=replace(sql,'center_hz REAL','center_hz TEXT') WHERE name='waveform_observations';PRAGMA writable_schema=OFF"}) {
        const auto path=directory/("hostile-schema-"+std::to_string(n++)+".sqlite");std::filesystem::copy_file(base,path);sql(path,mutation);
        ovmesh::SessionStore reader;rejects([&]{reader.open_readonly(path.string());},"hostile schema rejected: "+mutation);
    }
    for(const auto& mutation:std::vector<std::string>{"UPDATE discovery_gaps SET first_input_sample=-1","UPDATE discovery_gaps SET end_input_sample=first_input_sample","UPDATE discovery_gaps SET subband_index=32","UPDATE discovery_gaps SET reason='unknown'"}) {
        const auto path=directory/("hostile-gap-"+std::to_string(n++)+".sqlite");std::filesystem::copy_file(base,path);sql(path,mutation);
        ovmesh::SessionStore reader;reader.open_readonly(path.string());rejects([&]{reader.export_csv((directory/("hostile-gap-"+std::to_string(n)+".csv")).string(),{});},"hostile gap cannot be exported");
    }
}
void legacy(const std::filesystem::path& directory,const std::filesystem::path& base) {
    for(int version=1;version<=4;++version) {
        const auto path=directory/("legacy-"+std::to_string(version)+".sqlite");std::filesystem::copy_file(base,path);
        sql(path,"PRAGMA journal_mode=DELETE; DROP TABLE waveform_observations; DROP TABLE discovery_status; DROP TABLE discovery_bands; DROP TABLE discovery_gaps; ALTER TABLE session DROP COLUMN discover_lora;");
        if(version<4)sql(path,"DROP TABLE spectrum_tiles;DROP TABLE spectrum_events;DROP TABLE coverage_gaps;DROP TABLE survey_metrology;");
        if(version<3)sql(path,"DROP TABLE route_details;ALTER TABLE receptions DROP COLUMN evidence_port;ALTER TABLE receptions DROP COLUMN evidence_signature_present;ALTER TABLE receptions DROP COLUMN request_id;ALTER TABLE receptions DROP COLUMN reply_id;ALTER TABLE receptions DROP COLUMN signature_present;ALTER TABLE receptions DROP COLUMN routing_variant;");
        if(version<2)sql(path,"ALTER TABLE session DROP COLUMN tuning_offset_hz;");
        sql(path,"PRAGMA user_version="+std::to_string(version));const auto before=contents(path);
        {
            ovmesh::SessionStore reader;reader.open_readonly(path.string());const auto out=reader.read();
            require(!out.config.discover_lora&&!out.discovery.enabled&&out.waveforms.empty(),"legacy must not invent discovery");
            require(reader.analyze({}).waveforms.empty(),"legacy analysis empty waveform evidence");
            rejects([&]{reader.append(waveform());},"legacy waveform writes rejected");
            rejects([&]{reader.update(snapshot());},"legacy metadata writes rejected");
            const auto export_path=directory/("legacy-"+std::to_string(version)+".csv");reader.export_csv(export_path.string(),{});
            for(const auto& row:csv(export_path)) {
                require(row.at("record_type")!="discovery_status"&&row.at("record_type")!="waveform_observation","legacy export not fabricated");
                if(row.at("record_type")=="session")require(row.at("session_schema_version")==std::to_string(version),"historical export retains actual schema version");
            }
        }
        require(contents(path)==before,"legacy database bytes unchanged");
        require(!std::filesystem::exists(path.string()+"-wal")&&!std::filesystem::exists(path.string()+"-shm"),"legacy has no journal writes");
    }
}
}
int main() {
    try {
        const auto root=std::filesystem::current_path();
        const auto directory=root/"build"/("discovery-storage-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        const auto base=directory/"roundtrip.sqlite";make_base(base);
        roundtrip_and_exports(directory,base);analysis(base);bounded_history(directory);invalid_writes(directory);hostile_inputs(directory,base);legacy(directory,base);
        std::cout<<"Discovery storage: "<<checks<<" checks passed; synthetic artifacts "<<directory<<'\n';return 0;
    } catch(const std::exception& e){std::cerr<<"Discovery storage failed: "<<e.what()<<'\n';return 1;}
}
