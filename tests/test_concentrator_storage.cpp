// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage.hpp"
#include "ovmesh/report.hpp"
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
void require(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
template<class F>void rejects(F f,const char* message){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}require(rejected,message);}
std::string contents(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
void sql(const std::filesystem::path& path,const char* command){sqlite3* db=nullptr;if(sqlite3_open(path.string().c_str(),&db)!=SQLITE_OK)throw std::runtime_error("fixture open failed");const int rc=sqlite3_exec(db,command,nullptr,nullptr,nullptr);sqlite3_close(db);if(rc!=SQLITE_OK)throw std::runtime_error("fixture SQL failed");}
ovmesh::ReceiverConfig config(){ovmesh::ReceiverConfig c;c.synthetic=false;c.hardware_receiver=ovmesh::HardwareReceiver::Rak5146;c.sample_rate=0;c.survey_span_hz=5000000;c.lanes.clear();c.discover_lora=false;
    c.concentrators.boards[0].device_path="PRIVATE-DEVICE-PATH";c.concentrators.boards[0].device_id="PRIVATE-DEVICE-ID";
    auto b=c.concentrators.boards[0];b.frequency_hz=908750000;b.bandwidth_hz=500000;b.spreading_factor=7;c.concentrators.boards.push_back(b);
    c.survey_notes="SYNTHETIC PRIVATE NOTE";c.concentrators.decode_enabled=false;return c;
}
ovmesh::PositionFix fix(){ovmesh::PositionFix f;f.valid=true;f.latitude=.1234567;f.longitude=-.7654321;f.utc_seconds=1700000000.1;f.monotonic_seconds=1000.1;f.source="Synthetic GPS";f.hdop=.8;f.satellites=9;return f;}
ovmesh::ConcentratorScan scan(uint64_t id,unsigned board,double start){ovmesh::ConcentratorScan s;s.id=id;s.board_index=board;s.frequency_hz=board?908750000:906875000;s.elapsed_start_seconds=start;s.elapsed_end_seconds=start+.2;s.utc_start_seconds=1700000000+start;s.utc_end_seconds=1700000000+s.elapsed_end_seconds;s.counts[19]=1000;s.counts[32]=1000;s.receiver_position=fix();return s;}
ovmesh::Reception packet(){ovmesh::Reception p;p.id=1;p.utc_seconds=1700000000.4;p.elapsed_seconds=.4;p.frequency_hz=906875000;p.bandwidth_hz=250000;p.spreading_factor=11;p.coding_rate=5;p.header_valid=p.crc_valid=true;p.decoded.status=ovmesh::protocol::Status::no_matching_key;p.receiver_position=fix();p.concentrator=ovmesh::ConcentratorPacketMetadata{0,-83.25,0xfffffff0U};return p;}
ovmesh::Reception classified_packet(){auto p=packet();p.id=2;p.decoded.status=ovmesh::protocol::Status::classified;p.decoded.classification="likely Meshtastic";p.decoded.evidence=ovmesh::protocol::EnvelopeEvidence{1,false};return p;}
void create(const std::filesystem::path& path){const auto c=config();ovmesh::SessionStore store;store.create(path.string(),c,"synthetic-concentrator");store.append(scan(1,0,.1));auto second=scan(2,1,.1);second.receiver_position.reset();store.append(second);auto third=scan(3,0,.5);third.counts[19]=500;third.counts[32]=1500;store.append(third);store.append(packet());
    auto bad=scan(4,0,.6);rejects([&]{store.append(bad);},"overlapping same-board scan rejected");bad=scan(4,0,.9);bad.counts[0]=1;rejects([&]{store.append(bad);},"wrong sample total rejected");bad=scan(4,0,.9);bad.filter_bandwidth_hz=250000;rejects([&]{store.append(bad);},"modem bandwidth cannot replace scan filter");
    rejects([&]{store.append(ovmesh::SpectrumTile{});},"concentrator cannot persist FFT tiles");rejects([&]{store.append(ovmesh::SpectrumEvent{});},"concentrator cannot persist FFT events");
    auto wrong=packet();wrong.id=2;wrong.bandwidth_hz=500000;rejects([&]{store.append(wrong);},"configured packet bandwidth validated");
    rejects([&]{store.append(classified_packet());},"decode opt-out enforced at persistence boundary");
    ovmesh::Snapshot s;s.config=c;s.elapsed_seconds=1;s.state="PRIVATE-DEVICE-PATH";s.concentrator_health.resize(2);for(auto& h:s.concentrator_health){h.ready=true;h.state="PRIVATE-DEVICE-ID";h.scans=1;h.rssi_samples=2000;h.receptions=1;h.crc_failures=2;h.ready_elapsed_seconds=.1;h.last_update_elapsed_seconds=.8;}
    auto invalid=s;invalid.input_seconds=.2;rejects([&]{store.update(invalid);},"continuous occupancy cannot be fabricated");store.update(s,true);
}
void roundtrip(const std::filesystem::path& file,const std::filesystem::path& directory){ovmesh::SessionStore store;store.open_readonly(file.string());const auto s=store.read();require(store.schema_version()==7,"separate schema seven");require(s.config.hardware_receiver==ovmesh::HardwareReceiver::Rak5146&&!s.config.synthetic,"source discriminator preserved");require(s.config.sample_rate==0&&s.frequencies.empty()&&s.spectrum_fft_size==0&&s.input_seconds==0&&s.measurement_seconds==0,"no SDR measurements fabricated");require(s.config.concentrators.boards.size()==2&&s.config.concentrators.boards[1].bandwidth_hz==500000&&!s.config.concentrators.decode_enabled,"board configuration and decode opt-out preserved");
    for(const auto& b:s.config.concentrators.boards)require(b.device_id.empty()&&b.device_path.empty(),"device paths and IDs never retained");require(s.concentrator_scans==3&&s.concentrator_rssi_samples==6000&&s.recent_concentrator_scans.size()==2,"complete totals and latest distinct-frequency display");require(s.recent_concentrator_scans[0].id==3,"latest scan replaces display only");require(s.concentrator_health.size()==2&&s.concentrator_health[0].crc_failures==2,"health survives historical read");require(s.receptions.size()==1&&s.receptions[0].concentrator&&s.receptions[0].concentrator->hardware_timestamp_us==0xfffffff0U&&s.receptions[0].concentrator->rssi_dbm== -83.25,"packet RSSI and wrapping clock preserved");
    unsigned n=0;store.visit_concentrator_scans([&](const auto& v){++n;require(v.counts==scan(v.id,v.board_index,v.elapsed_start_seconds).counts||v.id==3,"all histogram bins retained");if(v.id==2)require(!v.receiver_position,"missing GPS stays missing");else require(v.receiver_position&&v.receiver_position->latitude==fix().latitude&&v.receiver_position->hdop==fix().hdop,"full-precision GPS association retained");});require(n==3,"visitor retains all scans");rejects([&]{store.analyze({});},"FFT analysis explicitly rejects sampled data");rejects([&]{store.visit_tiles([](const auto&){});},"FFT visitor rejects sampled data");
    const auto raw=contents(file);require(raw.find("PRIVATE-DEVICE")==std::string::npos,"private device strings absent from all database bytes");
#ifndef _WIN32
    struct stat st{};require(::stat(file.c_str(),&st)==0&&(st.st_mode&0777)==0600,"private file permissions");
#endif
    const auto archive=directory/"archive.csv";store.export_csv(archive.string(),{});const auto csv=contents(archive);require(csv.find("concentrator_rssi_scan")!=std::string::npos&&csv.find("rssi_histogram_counts_33")!=std::string::npos,"CSV archive includes histograms");require(csv.find("hardware configured modem bandwidth; not measured signal width")!=std::string::npos&&csv.find("board-local wrapping microsecond counter")!=std::string::npos,"CSV packet measurement provenance");require(csv.find("SYNTHETIC PRIVATE NOTE")==std::string::npos&&csv.find("Synthetic GPS")==std::string::npos,"CSV privacy settings suppress notes and GPS");require(csv.find("spectrum_metrology")==std::string::npos&&csv.find("uncalibrated dBFS/bin")==std::string::npos,"CSV avoids inapplicable FFT metrology");
    ovmesh::ExportOptions privacy;privacy.include_receiver_positions=true;privacy.coordinate_decimals=7;store.export_geojson((directory/"scan.geojson").string(),privacy);const auto geo=contents(directory/"scan.geojson");require(geo.find("concentrator_rssi_scan")!=std::string::npos&&geo.find("\"geometry\":null")!=std::string::npos&&geo.find("0.1234567")!=std::string::npos,"GeoJSON retains scans with and without GPS");require(geo.find("concentrator_reception")!=std::string::npos&&geo.find("hardware_timestamp_us")!=std::string::npos,"GeoJSON includes packet receiver provenance");
    ovmesh::ReportOptions report;ovmesh::export_survey_report(store,(directory/"frequency.csv").string(),report);const auto frequency=contents(directory/"frequency.csv");require(frequency.find("sample_exceedance_pct")!=std::string::npos&&frequency.find(",37.5,")!=std::string::npos,"sample-weighted exceedance over both scans");require(frequency.find("occupancy_pct")==std::string::npos&&frequency.find("dBFS")==std::string::npos,"report never substitutes FFT occupancy");
    report.kind=ovmesh::ReportKind::TimeSummary;report.query.elapsed_start=.15;report.query.elapsed_end=.25;ovmesh::export_survey_report(store,(directory/"partial.csv").string(),report);require(contents(directory/"partial.csv").find("boundary_scans")!=std::string::npos,"partial selections preserve full histograms with boundaries");
    report.kind=ovmesh::ReportKind::Analysis;report.query={};ovmesh::export_survey_report(store,(directory/"analysis.html").string(),report);const auto html=contents(directory/"analysis.html");require(html.find("-87 dBm")!=std::string::npos&&html.find("234300 Hz")!=std::string::npos&&html.find("not continuous occupancy")!=std::string::npos,"HTML method and limits explicit");require(html.find("SYNTHETIC PRIVATE NOTE")==std::string::npos&&html.find("0.1234567")==std::string::npos,"HTML privacy respected");require(contents(file)==raw,"read/export never changes source");
    report.kind=ovmesh::ReportKind::Waveforms;rejects([&]{ovmesh::export_survey_report(store,(directory/"unsupported.csv").string(),report);},"unsupported waveform report explicit");require(!std::filesystem::exists(directory/"unsupported.csv"),"failed report removes new partial file");
}
void spectrum_only_report(const std::filesystem::path& directory) {
    const auto source=directory/"spectrum-only.sqlite";auto c=config();
    for(auto& board:c.concentrators.boards)board.packets_enabled=false;
    { ovmesh::SessionStore store;store.create(source.string(),c,"synthetic-rak-zero");
      auto sample=scan(1,0,.1);sample.counts.fill(0);sample.counts[28]=2000;sample.receiver_position.reset();store.append(sample);
      ovmesh::Snapshot snapshot;snapshot.config=c;snapshot.elapsed_seconds=1;store.update(snapshot,true); }
    ovmesh::SessionStore reader;reader.open_readonly(source.string());ovmesh::ReportOptions options;options.kind=ovmesh::ReportKind::Analysis;
    const auto output=directory/"zero-report.html";ovmesh::export_survey_report(reader,output.string(),options);const auto html=contents(output);
    require(html.find("No recorded samples reached the threshold")!=std::string::npos,"Zero exceedance explicitly distinguished from no signals");
    require(html.find("Configured packet receivers")==std::string::npos && html.find("Spectrum-only survey")!=std::string::npos,"Disabled packet profiles do not masquerade as measured signals");
    require(html.find("1 scan centers, 1 scans, 2000 RSSI samples")!=std::string::npos && html.find("cy='210.000'")!=std::string::npos,"Real samples with zero exceedance render at zero");
    require(html.find("Outermost nominal filter edges")!=std::string::npos && html.find("Scans without a receiver fix: 1")!=std::string::npos,"RAK report surfaces scan coverage and missing GPS");
}
void packet_only(const std::filesystem::path& directory) {
    auto c=config();c.center_hz=915000000;c.survey_span_hz=26000000;c.concentrators.scan_enabled=false;c.concentrators.decode_enabled=true;
    const auto path=directory/"packet-only.sqlite";
    {ovmesh::SessionStore store;store.create(path.string(),c,"synthetic-packet-only");store.append(fix());store.append(classified_packet());ovmesh::Snapshot s;s.config=c;s.elapsed_seconds=1;store.update(s,true);}
    ovmesh::SessionStore store;store.open_readonly(path.string());const auto s=store.read();require(s.config.survey_span_hz==26000000&&!s.config.concentrators.scan_enabled&&s.concentrator_scans==0,"26 MHz range and packet-only source roundtrip");require(s.receptions.size()==1&&s.receptions[0].decoded.evidence&&s.receptions[0].concentrator,"classified packet and hardware provenance remain associated");
    const auto archive=directory/"classified-packet.csv";store.export_csv(archive.string(),{});const auto metadata=contents(archive);
    require(metadata.find("packet_rssi_dbm")!=std::string::npos&&metadata.find("likely Meshtastic")!=std::string::npos,"metadata archive includes classification and hardware provenance");
    require(metadata.find("content_kind")==std::string::npos&&metadata.find("node_id")==std::string::npos,"semantic columns absent from archive");
    const auto copy=directory/"packet-only-copy.sqlite";store.save_copy(copy.string());ovmesh::SessionStore copied;copied.open_readonly(copy.string());
    require(copied.read().receptions[0].concentrator->hardware_timestamp_us==s.receptions[0].concentrator->hardware_timestamp_us,"fresh logical copy preserves concentrator timestamp");
    const auto legacy=directory/"legacy-private-v7.sqlite";std::filesystem::copy_file(path,legacy);
    sql(legacy,"DROP TABLE metadata_policy;UPDATE receptions SET text='SYNTHETIC_PRIVATE_MESSAGE',node_id='SYNTHETIC_PRIVATE_NODE',latitude=999,voltage=X'ff';INSERT INTO routes VALUES(2,999,'SYNTHETIC_PRIVATE_ROUTE');INSERT INTO route_details VALUES(2,'SYNTHETIC_PRIVATE_BACK',999,X'ff');");
    const auto legacy_before=contents(legacy);ovmesh::SessionStore old;old.open_readonly(legacy.string());
    const auto old_snapshot=old.read();require(old_snapshot.total_receptions==1&&old_snapshot.classified_receptions==1&&old_snapshot.receptions[0].concentrator->rssi_dbm== -83.25,"legacy v7 metadata/counts/provenance survive semantic omission");
    size_t count=0;old.visit_receptions([&](const auto& r){require(r.decoded.evidence->port==1&&r.receiver_position->latitude==fix().latitude,"legacy v7 metadata visitor");++count;});require(count==1,"legacy v7 visitor preserves reception");
    old.export_csv((directory/"legacy-v7.csv").string(),{});ovmesh::ExportOptions privacy;privacy.include_receiver_positions=true;
    old.export_geojson((directory/"legacy-v7.geojson").string(),privacy);
    require(contents(directory/"legacy-v7.csv").find("SYNTHETIC_PRIVATE_")==std::string::npos&&contents(directory/"legacy-v7.geojson").find("SYNTHETIC_PRIVATE_")==std::string::npos,"legacy v7 exports omit semantic contents");
    const auto refused=directory/"legacy-v7-copy.sqlite";rejects([&]{old.save_copy(refused.string());},"legacy v7 unmarked copy refused");
    require(!std::filesystem::exists(refused)&&contents(legacy)==legacy_before,"legacy v7 copy refusal and reading preserve original bytes");
    ovmesh::ReportOptions options;
    options.kind=ovmesh::ReportKind::ReceiverTrack;options.privacy.include_receiver_positions=true;ovmesh::export_survey_report(store,(directory/"track.csv").string(),options); // Packet host anchor is available with scanning off.
}
}
int main(){const auto directory=std::filesystem::current_path()/("ovmesh-concentrator-storage-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));try{std::filesystem::create_directory(directory);
#ifndef _WIN32
    ::chmod(directory.c_str(),0700);
#endif
    const auto file=directory/"survey.sqlite";create(file);roundtrip(file,directory);packet_only(directory);spectrum_only_report(directory);unsigned i=0;
    for(const auto* command:{"UPDATE concentrator_scans SET counts=X'00'","UPDATE concentrator_scans SET sample_count=1999","UPDATE concentrator_scans SET filter_hz=250000","UPDATE concentrator_scans SET rssi_offset_db=-10","UPDATE concentrator_scans SET board_index=2","UPDATE concentrator_scans SET frequency_hz=902000000","UPDATE concentrator_scans SET receiver_fix=999","UPDATE concentrator_scans SET elapsed_end=99","UPDATE concentrator_profiles SET bandwidth_hz=200000","UPDATE concentrator_packets SET hardware_timestamp_us=4294967296","DELETE FROM concentrator_packets","UPDATE concentrator_setup SET method='unsupported'","UPDATE concentrator_setup SET version=2","UPDATE session SET synthetic=0","UPDATE session SET measurement_seconds=1","UPDATE positions SET latitude=91","ALTER TABLE concentrator_scans ADD COLUMN device_path TEXT","CREATE TRIGGER unexpected AFTER INSERT ON concentrator_scans BEGIN SELECT 1; END"}) {
        const auto bad=directory/("bad-"+std::to_string(i++)+".sqlite");std::filesystem::copy_file(file,bad);sql(bad,command);rejects([&]{ovmesh::SessionStore store;store.open_readonly(bad.string());(void)store.read();store.visit_concentrator_scans([](const auto&){});store.visit_receptions([](const auto&){});},"malformed concentrator data must fail closed");
    }
    const auto malformed=directory/"bad-export.sqlite";std::filesystem::copy_file(file,malformed);sql(malformed,"UPDATE concentrator_scans SET counts=X'00' WHERE id=1");
    {ovmesh::SessionStore store;store.open_readonly(malformed.string());rejects([&]{store.export_csv((directory/"bad-export.csv").string(),{});},"old malformed row rejected before archive reservation");require(!std::filesystem::exists(directory/"bad-export.csv"),"malformed archive has no partial CSV");}
    std::filesystem::remove_all(directory);std::cout<<checks<<" concentrator storage/report checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"Concentrator storage failure: "<<e.what()<<" fixture directory="<<directory<<'\n';return 1;}}
