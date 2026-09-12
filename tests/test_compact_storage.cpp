// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic measurement fixtures only; no radio, raw IQ, keys or actual routes.
#include "storage.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifndef _WIN32
#include <sys/stat.h>
#endif
namespace {
unsigned checks=0;
void require(bool value,const char* text){++checks;if(!value)throw std::runtime_error(text);}
template<class F> void rejects(F action,const char* text){bool caught=false;try{action();}catch(const std::exception&){caught=true;}require(caught,text);}
std::string contents(const std::filesystem::path& p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void sql(const std::filesystem::path& p,const std::string& text){sqlite3* db=nullptr;require(sqlite3_open(p.string().c_str(),&db)==SQLITE_OK,"open SQL fixture");const int rc=sqlite3_exec(db,text.c_str(),nullptr,nullptr,nullptr);sqlite3_close(db);require(rc==SQLITE_OK,"execute SQL fixture");}
int64_t scalar(const std::filesystem::path& p,const char* text){sqlite3* db=nullptr;sqlite3_stmt* q=nullptr;require(sqlite3_open_v2(p.string().c_str(),&db,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK,"open scalar");require(sqlite3_prepare_v2(db,text,-1,&q,nullptr)==SQLITE_OK,"prepare scalar");require(sqlite3_step(q)==SQLITE_ROW,"read scalar");const auto out=sqlite3_column_int64(q,0);sqlite3_finalize(q);sqlite3_close(db);return out;}
ovmesh::ReceiverConfig config(bool compact=true){ovmesh::ReceiverConfig c;c.compact_recording=compact;c.sample_rate=16000000;c.center_hz=907500000;c.survey_span_hz=10000000;c.lanes.clear();c.discover_lora=true;return c;}
ovmesh::PositionFix position(unsigned group){ovmesh::PositionFix f;f.valid=true;f.latitude=.123456789012345+double(group)/10;f.longitude=-.765432109876543;f.utc_seconds=1700000000.+group;f.monotonic_seconds=100.+group;f.altitude_m=42.123456789;f.hdop=.9;f.satellites=11;f.source="synthetic compact GPS";return f;}
ovmesh::SpectrumTile tile(unsigned i){ovmesh::SpectrumTile t;t.id=i+1;t.first_sample=uint64_t(i)*80*4096+(i>=100?2000000:0);t.end_sample=t.first_sample+80*4096;t.elapsed_start_seconds=double(t.first_sample)/16000000;t.elapsed_end_seconds=double(t.end_sample)/16000000;t.utc_start_seconds=1700000000.+t.elapsed_start_seconds;t.utc_end_seconds=1700000000.+t.elapsed_end_seconds;t.first_center_hz=902503906.25;t.bin_width_hz=3906.25;t.frame_count=80;t.background_dbfs=-89.123f;t.mean_dbfs.resize(2559);t.peak_dbfs.resize(2559);const size_t stride=(t.mean_dbfs.size()+7)/8;t.activity.resize(stride*t.frame_count);
    for(size_t b=0;b<t.mean_dbfs.size();++b){t.mean_dbfs[b]=-83.f+float((i+b)%9);t.peak_dbfs[b]=t.mean_dbfs[b]+float(3+i%3);}
    // Distinct and simultaneous activity: summing per-bin busy time would fail.
    for(size_t f=0;f<t.frame_count;++f){if(f%4<2)t.activity[f*stride+20/8]|=uint8_t(1u<<(20%8));if(f%4==1||f%4==2)t.activity[f*stride+80/8]|=uint8_t(1u<<(80%8));}
    if(i%13!=0)t.receiver_start=position(i/50);if(i%17!=0)t.receiver_end=position(i/50);return t;
}
ovmesh::Snapshot snapshot(double elapsed){ovmesh::Snapshot s;s.config=config();s.elapsed_seconds=elapsed;s.input_seconds=s.measurement_seconds=elapsed-(elapsed>2.0480001?.125:0.);s.delivered_samples=uint64_t(std::llround(s.input_seconds*16000000));s.dropped_samples=elapsed>2.0480001?2000000:0;s.discovery.enabled=true;return s;}
void make(const std::filesystem::path& p,bool compact){ovmesh::SessionStore writer;writer.create(p.string(),config(compact),"synthetic compact storage");
    for(unsigned i=0;i<210;++i){if(i==100){ovmesh::CoverageGap gap;gap.id=1;gap.missing_samples=2000000;gap.elapsed_start_seconds=double(i)*80*4096/16000000;gap.elapsed_end_seconds=gap.elapsed_start_seconds+.125;gap.utc_start_seconds=1700000000.+gap.elapsed_start_seconds;gap.utc_end_seconds=1700000000.+gap.elapsed_end_seconds;gap.reason="synthetic deliberate gap";writer.append(gap);}
        if(i%50==0)writer.append(position(i/50));writer.append(tile(i));
        // Many checkpoints must not accidentally create one power block per tile.
        writer.update(snapshot(tile(i).elapsed_end_seconds));
    }
    ovmesh::SurveyWindow window;window.id=1;window.elapsed_start_seconds=0;window.elapsed_end_seconds=4;window.utc_start_seconds=1700000000;window.utc_end_seconds=1700000004;window.receiver_position=position(4);for(unsigned b=0;b<2559;++b)window.frequencies.push_back({uint64_t(902503906.25+b*3906.25),3906,-80,-70,4,1});writer.append(window);
    ovmesh::WaveformObservation wave;wave.id=1;wave.center_hz=906875000;wave.bandwidth_hz=250000;wave.spreading_factor=11;wave.first_observed_elapsed=.1;wave.delimiter_elapsed=.2;wave.delimiter_utc=1700000000.2;wave.up_match=.8;wave.down_match=.9;wave.contributing_subbands=1;wave.complete_in_requested_range=true;wave.receiver_position=position(0);writer.append(wave);
    ovmesh::Reception reception;reception.id=1;reception.frequency_hz=906875000;reception.bandwidth_hz=250000;reception.spreading_factor=11;reception.coding_rate=5;reception.utc_seconds=1700000001;reception.elapsed_seconds=1;reception.decoded.status=ovmesh::protocol::Status::no_matching_key;writer.append(reception);
    reception.id=2;reception.header_valid=reception.crc_valid=true;reception.decoded.status=ovmesh::protocol::Status::classified;reception.decoded.classification="likely Meshtastic";
    reception.decoded.evidence=ovmesh::protocol::EnvelopeEvidence{1,false};writer.append(reception);
    ovmesh::SpectrumEvent event;event.id=1;event.first_sample=1600000;event.end_sample=4800000;event.elapsed_start_seconds=.1;event.elapsed_end_seconds=.3;event.utc_start_seconds=1700000000.1;event.utc_end_seconds=1700000000.3;event.lower_hz=906750000;event.upper_hz=907000000;event.active_seconds=.125;event.mean_dbfs=-66.25f;event.peak_dbfs=-42.75f;event.receiver_end=position(0);writer.append(event);
    auto final=snapshot(tile(209).elapsed_end_seconds);final.discovery.observations=1;final.discovery.finished=true;writer.update(final,true);
}
void long_fix_reuse(const std::filesystem::path& path) {
    ovmesh::SessionStore writer;writer.create(path.string(),config(),"synthetic GPS reuse");
    auto original=position(0);original.altitude_m.reset();original.hdop.reset();writer.append(original);
    for(unsigned i=1;i<=160;++i)writer.append(position(i));
    writer.append(original); // Outside the bounded cache, indexed exact lookup.
    auto distinct=original;distinct.altitude_m=0;writer.append(distinct);
    writer.update(snapshot(0),true);
    require(scalar(path,"SELECT count(*) FROM positions")==162,"dedup beyond cache and null versus zero precision");
}
void stable_fix_identity(const std::filesystem::path& path) {
    {ovmesh::SessionStore writer;writer.create(path.string(),config(),"synthetic stable GPS identity");
        for(unsigned i=0;i<160;++i)writer.append(position(i));
        auto t=tile(0);t.receiver_start=t.receiver_end=position(159);writer.append(t);writer.update(snapshot(t.elapsed_end_seconds),true);}
    // Remove only unrelated synthetic fixes to create holes, then exercise a
    // normal SQLite compaction. Explicit INTEGER PRIMARY KEY ids must survive.
    sql(path,"DELETE FROM positions WHERE id<100; VACUUM;");
    ovmesh::SessionStore reader;reader.open_readonly(path.string());unsigned count=0;
    reader.visit_tiles([&](const auto& t){require(t.receiver_start&&t.receiver_end&&t.receiver_end->latitude==position(159).latitude,"GPS references survive SQLite VACUUM");++count;});
    require(count==1,"vacuum retains referenced measurement");
}
void compare(const std::filesystem::path& compact,const std::filesystem::path& detailed){ovmesh::SessionStore c,d;c.open_readonly(compact.string());d.open_readonly(detailed.string());require(c.read().config.compact_recording&&!d.read().config.compact_recording,"recording mode readback");
    unsigned n=0;c.visit_tiles([&](const auto& saved){const auto original=tile(n++);require(saved.activity==original.activity,"joint activity bytes remain exact");require(saved.first_sample==original.first_sample&&saved.end_sample==original.end_sample&&saved.frame_count==original.frame_count,"fine timing exact");require(saved.quality&ovmesh::SurveyPowerAggregated,"coarse power flagged");require(saved.power_elapsed_start<=saved.elapsed_start_seconds&&saved.power_elapsed_end>=saved.elapsed_end_seconds,"explicit power support covers fine tile");require(saved.power_elapsed_end-saved.power_elapsed_start<=1.0000001,"bounded aggregate support");require(saved.receiver_start.has_value()==original.receiver_start.has_value()&&saved.receiver_end.has_value()==original.receiver_end.has_value(),"missing GPS preserved independently");if(saved.receiver_end)require(saved.receiver_end->latitude==original.receiver_end->latitude&&saved.receiver_end->longitude==original.receiver_end->longitude&&saved.receiver_end->hdop==original.receiver_end->hdop,"GPS precision and quality exact");});require(n==210,"all fine tiles streamed");
    const auto fullc=c.analyze({}),fulld=d.analyze({});require(fullc.gaps.size()==1&&fullc.gaps[0].missing_samples==2000000,"gap retained");require(fullc.events.size()==1&&fullc.events[0].mean_dbfs==-66.25f&&fullc.events[0].peak_dbfs==-42.75f,"individual energy event statistics retained");require(std::abs(fullc.observed_seconds-210*80*4096./16000000)<1e-10,"known observed denominator");require(std::abs(fullc.busy_seconds-fullc.observed_seconds*.75)<1e-10,"joint union occupancy independently known");require(fullc.busy_seconds==fulld.busy_seconds&&fullc.observed_seconds==fulld.observed_seconds,"full activity unchanged");require(fullc.missing_start_position_seconds==fulld.missing_start_position_seconds&&fullc.missing_end_position_seconds==fulld.missing_end_position_seconds,"GPS coverage unchanged");
    for(size_t b=0;b<fullc.bins.size();++b){require(fullc.bins[b].active_seconds==fulld.bins[b].active_seconds,"every bin busy unchanged");require(std::abs(fullc.bins[b].mean_dbfs-fulld.bins[b].mean_dbfs)<=.011,"linear power aggregation respects centidB precision");require(fullc.bins[b].peak_dbfs==fulld.bins[b].peak_dbfs,"session peak exact");}
    {ovmesh::SurveyQuery q;q.elapsed_start=tile(10).elapsed_start_seconds;q.elapsed_end=tile(19).elapsed_end_seconds;
        const auto a=c.analyze(q),b=d.analyze(q);require((a.quality&ovmesh::SurveyBoundary)&&!(b.quality&ovmesh::SurveyBoundary),"query aligned to fine tiles still marks partial coarse power");
        q={};q.geographic_filter=true;q.south=.12;q.north=.125;q.west=-.8;q.east=-.7;require(c.analyze(q).quality&ovmesh::SurveyBoundary,"geographic power subset boundary explicit");
        q={};q.time_bucket_seconds=tile(19).elapsed_end_seconds;const auto buckets=c.analyze(q);
        require(!buckets.observations.empty()&&(buckets.observations.front().quality&ovmesh::SurveyBoundary),"fine-aligned display bucket marks partial coarse power");}
    for(unsigned i=0;i<35;++i){ovmesh::SurveyQuery q;q.lower_hz=902503906.25+(i%25)*3906.25;q.upper_hz=q.lower_hz+(1+i%3)*250000;q.elapsed_start=.0137+i*.07;q.elapsed_end=q.elapsed_start+.81837;if(i%2){q.geographic_filter=true;q.south=.12;q.north=.325;q.west=-.8;q.east=-.7;}const auto a=c.analyze(q),b=d.analyze(q);require(a.busy_seconds==b.busy_seconds&&a.observed_seconds==b.observed_seconds,"time/frequency/GPS partial occupancy unchanged");for(size_t k=0;k<a.bins.size();++k)require(a.bins[k].active_seconds==b.bins[k].active_seconds,"partial per-bin activity exact");}
    n=0;c.visit_positions([&](const auto& fix){require(fix.latitude==position(n).latitude&&fix.utc_seconds==position(n).utc_seconds,"normalized GPS order and precision");++n;});require(n==5,"fix stored once including repeated tile references");n=0;c.visit_waveforms([&](const auto& w){require(w.bandwidth_hz==250000&&w.receiver_position->latitude==position(0).latitude,"waveform evidence unchanged");++n;});require(n==1,"waveform retained");n=0;c.visit_receptions([&](const auto& r){require(r.id==n+1,"reception retained");if(r.id==2)require(r.decoded.evidence&&r.decoded.evidence->port==1&&r.decoded.status==ovmesh::protocol::Status::classified,"classified reception metadata retained");else require(!r.decoded.evidence,"unknown packet metadata does not gain envelope evidence");++n;});require(n==2,"all receptions visited");
    for(const auto bounds:{std::pair{1000000.,2000000.},std::pair{5000000000.,5001000000.}}) {
        ovmesh::SurveyQuery q;q.lower_hz=bounds.first;q.upper_hz=bounds.second;const auto empty=c.analyze(q);
        require(empty.bins.empty()&&empty.observed_seconds==0&&empty.busy_seconds==0,"nonintersecting frequency query is unobserved, not quiet");
    }
    ovmesh::SurveyWindow window;window.id=1;window.utc_start_seconds=1700000000;window.utc_end_seconds=1700000001;window.elapsed_end_seconds=1;window.frequencies.push_back({906875000,250000,-80,-70,1,0});
    rejects([&]{c.append(window);},"compact readonly no-op append still rejected");
}
}
int main(){const auto directory=std::filesystem::current_path()/("ovmesh-compact-storage-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));try{std::filesystem::create_directory(directory);
#ifndef _WIN32
    ::chmod(directory.c_str(),0700);
#endif
    const auto compact=directory/"compact.sqlite",detailed=directory/"detailed.sqlite";make(compact,true);make(detailed,false);long_fix_reuse(directory/"fix-reuse.sqlite");stable_fix_identity(directory/"stable-fix.sqlite");require(scalar(compact,"PRAGMA user_version")==6&&scalar(detailed,"PRAGMA user_version")==5,"new compact and detailed schema versions");require(scalar(compact,"SELECT count(*) FROM power_blocks")==6,"one-second blocks unaffected by 210 checkpoints and split at gap");require(scalar(compact,"SELECT count(*) FROM window_bins")==0&&scalar(detailed,"SELECT count(*) FROM window_bins")==2559,"duplicate five-second summaries omitted only compact");compare(compact,detailed);
#ifndef _WIN32
    struct stat info{};require(::stat(compact.c_str(),&info)==0&&(info.st_mode&0777)==0600,"private database permissions");
#endif
    const auto before=contents(compact);{ovmesh::SessionStore reader;reader.open_readonly(compact.string());reader.export_csv((directory/"archive.csv").string(),{});const auto text=contents(directory/"archive.csv");require(text.find("power_elapsed_start_seconds")!=std::string::npos&&text.find("coarse block power")!=std::string::npos,"archive coarse power support explicit");reader.write_report_file((directory/"report.csv").string(),[](const auto& emit){emit("frequency,busy\n");});require(contents(directory/"report.csv")=="frequency,busy\n","private report callback");rejects([&]{reader.write_report_file((directory/"report.csv").string(),[](const auto&){});},"never overwrite report");rejects([&]{reader.write_report_file((directory/"failed.csv").string(),[](const auto& emit){emit("partial\n");throw std::runtime_error("synthetic failure");});},"failed report surfaced");require(!std::filesystem::exists(directory/"failed.csv"),"failed new report removed");}
    require(contents(compact)==before,"readonly analysis/export never alters original");
    const auto copied=directory/"metadata-copy.sqlite";
    {ovmesh::SessionStore reader;reader.open_readonly(compact.string());reader.save_copy(copied.string());}
    compare(copied,detailed);
    require(contents(compact)==before,"logical copy preserves original compact recording bytes");
    require(scalar(copied,"SELECT count(*) FROM receptions WHERE profile IS NOT NULL OR origin IS NOT NULL OR text IS NOT NULL")==0&&
        scalar(copied,"SELECT count(*) FROM routes")==0&&scalar(copied,"SELECT count(*) FROM route_details")==0,"new compact records and copies retain no semantic contents");
    unsigned invalid=0;for(const auto& command:{"UPDATE spectrum_tiles SET power_id=999 WHERE id=1","UPDATE spectrum_tiles SET start_fix=999 WHERE id=1","UPDATE power_blocks SET elapsed_end=9 WHERE id=1","UPDATE power_blocks SET frame_count=-1 WHERE id=1","UPDATE power_blocks SET bin_count=4097 WHERE id=1","UPDATE power_blocks SET mean_cdb=X'00' WHERE id=1","UPDATE spectrum_tiles SET activity=X'01FFFF01' WHERE id=1","UPDATE positions SET latitude=91 WHERE rowid=1","UPDATE spectrum_tiles SET quality=1024 WHERE id=1"}){const auto p=directory/("bad-"+std::to_string(invalid++)+".sqlite");std::filesystem::copy_file(compact,p);sql(p,command);ovmesh::SessionStore reader;reader.open_readonly(p.string());rejects([&]{reader.visit_tiles([](const auto&){});},"malformed compact data rejected");}
    const auto compact_size=std::filesystem::file_size(compact),detailed_size=std::filesystem::file_size(detailed);require(compact_size*4<detailed_size,"meaningful measured compact size reduction");std::cout<<checks<<" compact storage checks passed; detailed_bytes="<<detailed_size<<" compact_bytes="<<compact_size<<" reduction_percent="<<100.*(1.-double(compact_size)/double(detailed_size))<<'\n';std::filesystem::remove_all(directory);return 0;
}catch(const std::exception& e){std::cerr<<"Compact storage test failed: "<<e.what()<<" fixtures="<<directory<<'\n';return 1;}}
