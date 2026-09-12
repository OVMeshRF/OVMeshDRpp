#include "ovmesh/engine.hpp"
#include "storage.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
void require(bool value,const std::string& message) { if(!value)throw std::runtime_error(message); }
bool wait_for(ovmesh::Engine& engine,const std::function<bool(const ovmesh::Snapshot&)>& predicate,double timeout) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::duration<double>(timeout);
    while(std::chrono::steady_clock::now()<deadline) {
        const auto snapshot=engine.snapshot();
        if(!snapshot.error.empty())throw std::runtime_error(snapshot.error);
        require(snapshot.measurement_seconds<=snapshot.input_seconds+1e-7,"Live measurement time cannot exceed accepted input time");
        if(predicate(snapshot))return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
double scalar(sqlite3* db,const char* query) {
    sqlite3_stmt* raw=nullptr;
    require(sqlite3_prepare_v2(db,query,-1,&raw,nullptr)==SQLITE_OK,"Prepare synthetic survey validation");
    const std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> statement(raw,sqlite3_finalize);
    require(sqlite3_step(statement.get())==SQLITE_ROW,"Read synthetic survey validation");
    return sqlite3_column_double(statement.get(),0);
}
void check_resume(const std::filesystem::path& directory,bool detailed) {
    ovmesh::Engine engine(ovmesh::Engine::SyntheticPacing::ConsumerPaced);
    ovmesh::ReceiverConfig config;config.lanes.clear();config.compact_recording=!detailed;
    engine.set_fixed_position(0,0);
    config.session_path=(directory/"resumed.sqlite").string();std::string error;
    require(engine.set_public_meshtastic_key_enabled(true,error)&&engine.public_meshtastic_key_enabled()&&
        engine.configured_key_count()==1&&engine.key_records().size()==16,"Explicit public default uses a separate slot");
    require(!engine.has_survey_key(16)&&!engine.set_survey_key(16,"Not a custom slot","AQ==",error),
        "Public default does not expand writable custom key slots");
    require(engine.start(config,false,error),error);
    require(wait_for(engine,[](const auto& s){return s.measurement_seconds>=.15;},30),"First acquisition has spectrum");
    engine.stop();const auto first=engine.snapshot();
    require(first.acquisitions.size()==1&&first.acquisitions[0].finalized,"Stop finalizes first acquisition");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(engine.start(config,false,error),error);
    require(wait_for(engine,[&](const auto& s){return s.measurement_seconds>=first.measurement_seconds+.15;},30),"Resume gathers additional observations");
    engine.stop();const auto same=engine.snapshot();
    require(same.session_id==first.session_id&&same.config.session_path==config.session_path&&same.acquisitions.size()==2&&
        same.spectrum_tiles>first.spectrum_tiles&&same.delivered_samples>first.delivered_samples,"Resume retains survey identity and cumulative measurements");
    for(const auto& bin:first.frequencies) {
        const auto found=std::find_if(same.frequencies.begin(),same.frequencies.end(),[&](const auto& v){return v.center_hz==bin.center_hz&&v.width_hz==bin.width_hz;});
        require(found!=same.frequencies.end()&&found->observed_seconds>bin.observed_seconds,"Same-grid frequency history accumulates across pause");
    }
    // Change both grid position and spacing. Former measurements retain their
    // original setup rather than being relabeled with the current receiver.
    engine.clear_position();
    config.center_hz=915000000;config.sample_rate=8000000;config.survey_span_hz=4000000;
    config.tuning_offset_hz=1200;config.lna_gain=24;config.vga_gain=20;config.activity_threshold_dbfs=-50;
    require(engine.start(config,false,error),error);
    require(wait_for(engine,[&](const auto& s){return s.measurement_seconds>=same.measurement_seconds+.15;},30),"Reconfigured resume gathers a new grid");
    engine.stop();const auto final=engine.snapshot();require(engine.save_session(error),error);
    require(final.session_id==first.session_id&&final.acquisitions.size()==3&&
        final.acquisitions[0].config.center_hz==907500000&&final.acquisitions[2].config.center_hz==915000000&&
        final.acquisitions[0].config.sample_rate==16000000&&final.acquisitions[2].config.sample_rate==8000000,
        "Each uninterrupted segment preserves its receiver settings");
    require(final.frequencies.size()>same.frequencies.size(),"Current survey retains prior and new grids");
    ovmesh::SessionStore saved;saved.open_readonly(config.session_path);const auto snapshot=saved.read();
    require(snapshot.session_id==first.session_id&&snapshot.acquisitions.size()==3&&snapshot.spectrum_tiles==final.spectrum_tiles&&
        std::abs(snapshot.measurement_seconds-final.measurement_seconds)<1e-8,"Same SQLite recording includes all acquisition segments");
    double exposure=0;uint64_t tiles=0;double prior_end=0;
    saved.visit_tiles([&](const auto& tile){++tiles;const auto& acquisition=ovmesh::acquisition_config_at(snapshot.acquisitions,snapshot.config,tile.elapsed_start_seconds);
        require(tile.elapsed_start_seconds>=prior_end-1e-8,"Global measurement timeline does not overlap");prior_end=tile.elapsed_end_seconds;
        require(std::abs(tile.bin_width_hz-double(acquisition.sample_rate)/4096)<1e-9,"Each tile is validated using its actual acquisition sample rate");
        if(acquisition.center_hz==915000000)require(!tile.receiver_start&&!tile.receiver_end,"Resumed measurements never reuse cleared GPS fixes");
        else require(tile.receiver_start&&tile.receiver_end,"Prior acquisitions retain their approved receiver positions");
        exposure+=tile.elapsed_end_seconds-tile.elapsed_start_seconds;});
    require(tiles==final.spectrum_tiles&&std::abs(exposure-final.measurement_seconds)<1e-7,"Paused time adds no measured exposure");
    unsigned pauses=0;saved.visit_gaps([&](const auto& gap){if(gap.reason=="reception_paused"){
        ++pauses;require(gap.missing_samples==0,"Pause is unobserved elapsed time, not lost source samples");
        saved.visit_tiles([&](const auto& tile){require(tile.elapsed_start_seconds>=gap.elapsed_end_seconds-1e-8||tile.elapsed_end_seconds<=gap.elapsed_start_seconds+1e-8,"No pause becomes measured RF coverage");});}});
    require(pauses==2,"Every stop/resume interval is explicitly unobserved");
    ovmesh::SurveyQuery query;const auto analyzed=saved.analyze(query);
    require(analyzed.tile_count==final.spectrum_tiles&&std::abs(analyzed.observed_seconds-final.measurement_seconds)<1e-7,
        "Full-range analysis includes observations from all acquisition grids");
    const auto copy=(directory/"resumed-copy.sqlite").string();require(engine.save_session_copy(copy,error),error);
    ovmesh::SessionStore copied;copied.open_readonly(copy);require(copied.read().acquisitions.size()==3&&copied.analyze(query).tile_count==final.spectrum_tiles,
        "Save copy retains segmented measurement provenance");
    require(engine.start(config,false,error),error);engine.stop();const auto immediate=engine.snapshot();
    require(immediate.acquisitions.size()==4&&immediate.acquisitions.back().finalized&&
        immediate.acquisitions.back().elapsed_end_seconds>=immediate.acquisitions.back().elapsed_start_seconds&&
        immediate.elapsed_seconds>=final.elapsed_seconds&&engine.save_session(error),
        "Immediate Stop after Resume preserves valid monotonic acquisition bounds even before input arrives");
    auto invalid=config;invalid.synthetic=false;
    require(!engine.start(invalid,false,error)&&engine.snapshot().session_id==first.session_id&&engine.snapshot().spectrum_tiles==immediate.spectrum_tiles,
        "Receiver type change is explicit and never discards the current survey");
    require(engine.new_session(error)&&engine.public_meshtastic_key_enabled()&&engine.snapshot().acquisitions.empty(),
        "Only New clears all acquisitions while preserving explicit public key setup");
    engine.clear_keys();require(!engine.public_meshtastic_key_enabled()&&engine.configured_key_count()==0,"Clear keys disables the public default too");
}
}
int main(int argc,char** argv) {
    try {
        bool detailed=false,realtime=false;
        for(int i=1;i<argc;++i) {
            const std::string argument=argv[i];
            if(argument=="--detailed")detailed=true;
            else if(argument=="--realtime")realtime=true;
            else throw std::runtime_error("Usage: test_engine [--detailed] [--realtime]");
        }
        ovmesh::Engine engine(realtime ? ovmesh::Engine::SyntheticPacing::Realtime :
            ovmesh::Engine::SyntheticPacing::ConsumerPaced);
        ovmesh::ReceiverConfig config; std::string error;
        std::cout<<(realtime?"Real-time throughput check":"Consumer-paced correctness fixture")
            <<" at "<<config.sample_rate<<" samples/s; "<<(detailed?"detailed":"compact")<<" recording; no hardware\n"<<std::flush;
        require(!engine.save_session(error)&&error.find("not recorded")!=std::string::npos,
            "Fresh Save refuses to imply a recording exists");
        require(engine.new_session(error)&&engine.snapshot().session_id.empty()&&!engine.snapshot().running,
            "Fresh New remains empty and never starts input");
        require(!engine.has_channel_key(0,"LongFast")&&!engine.has_channel_key(16,"LongFast") &&
            !engine.has_survey_key(0) && engine.configured_key_count()==0,"Fresh engine has no implicit public key");
        const auto empty_records=engine.key_records();
        require(empty_records.size()==16,"Keyring has 16 records independent of the one default RF lane");
        for(size_t i=0;i<empty_records.size();++i)
            require(empty_records[i].slot==i && !empty_records[i].configured &&
                !empty_records[i].restrict_channel_name && empty_records[i].channel_name.empty(),"Unused key records have empty unrestricted metadata");
        require(engine.set_channel_key(0,"LongFast","AQ==",error),"Explicit public shorthand accepted");
        require(engine.has_channel_key(0,"LongFast")&&!engine.has_channel_key(0,"longfast")&&!engine.has_channel_key(1,"LongFast"),"Named key readiness requires exact record and channel name");
        require(!engine.set_channel_key(0,"OtherChannel","AR==",error)&&engine.has_channel_key(0,"LongFast"),"Rejected key input preserves previous binding");
        require(engine.set_channel_key(1,"LongTurbo","AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=",error),"Explicit 32-byte Base64 key accepted");
        require(engine.has_channel_key(1,"LongTurbo")&&!engine.has_channel_key(1,"LongFast"),"Second key record has independent channel scope");
        require(engine.set_survey_key(15,"Community survey","202122232425262728292a2b2c2d2e2f",error),"Key-only record outside RF lane range accepted");
        require(engine.has_survey_key(15) && !engine.has_channel_key(15,"") && !engine.has_survey_key(16) &&
            engine.configured_key_count()==3,"Named and key-only readiness remain distinct");
        const auto records=engine.key_records();
        require(records[15].configured && records[15].label=="Community survey" && records[15].channel_name.empty() &&
            !records[15].restrict_channel_name && records[0].restrict_channel_name && records[0].channel_name=="LongFast", "Keyring view exposes scope metadata only");
        require(!engine.set_survey_key(15,"Replacement","AR==",error) && engine.key_records()[15].label=="Community survey" &&
            engine.has_survey_key(15),"Rejected key-only replacement preserves the record");
        require(!engine.set_survey_key(16,"Overflow","AQ==",error) &&
            !engine.set_channel_key(16,"Overflow","AQ==",error),"Key record bounds enforced");
        require(!engine.set_survey_key(15,"","AQ==",error) &&
            !engine.set_survey_key(15,std::string(81,'x'),"AQ==",error) &&
            !engine.set_channel_key(0,std::string(33,'x'),"AQ==",error),"Scope labels obey protocol and UI limits");
        require(!engine.snapshot().running&&engine.snapshot().delivered_samples==0,"Key setup never starts a receiver");
        engine.clear_keys();
        require(!engine.has_channel_key(0,"LongFast")&&!engine.has_channel_key(1,"LongTurbo") &&
            !engine.has_survey_key(15) && engine.configured_key_count()==0,"Clear removes all in-memory keys");
        require(engine.set_survey_key(0,"Key only","AQ==",error) && engine.has_survey_key(0) && !engine.has_channel_key(0,"LongFast"),"Key-only replacement removes prior channel-name scope");
        require(engine.set_channel_key(0,"LongFast","AQ==",error) && !engine.has_survey_key(0) && engine.has_channel_key(0,"LongFast"),"Named replacement restores explicit channel-name scope");
        engine.clear_keys();
        config.tuning_offset_hz=900;
        require(ovmesh::tuned_center_hz(config)==907500900,"Positive calibration raises the hardware tune command");
        config.tuning_offset_hz=-900;
        require(ovmesh::tuned_center_hz(config)==907499100,"Negative calibration lowers the hardware tune command");
        for(const auto invalid_offset:{int64_t(-100001),int64_t(100001),std::numeric_limits<int64_t>::min(),std::numeric_limits<int64_t>::max()}) {
            config.tuning_offset_hz=invalid_offset;
            require(!engine.start(config,false,error)&&error.find("offset")!=std::string::npos,"Invalid calibration rejected before source start");
        }
        config.tuning_offset_hz=-1;config.center_hz=1000000;
        require(!engine.start(config,false,error)&&error.find("Corrected")!=std::string::npos,"Corrected tune below hardware range rejected");
        config.tuning_offset_hz=1;config.center_hz=6000000000ULL;
        require(!engine.start(config,false,error)&&error.find("Corrected")!=std::string::npos,"Corrected tune above hardware range rejected");
        config=ovmesh::ReceiverConfig{};
        config.synthetic=false;
        require(!engine.start(config,false,error)&&error.find("permission")!=std::string::npos,"Hardware start must refuse before opening a device");
        config.sample_rate=0;
        require(!engine.start(config,false,error)&&error.find("sample rate")!=std::string::npos,"Invalid rate must fail before device access");
        config=ovmesh::ReceiverConfig{};
        config.lanes[0].frequency_hz=std::numeric_limits<std::uint64_t>::max();
        require(!engine.start(config,false,error),"Overflowing lane frequency rejected");
        config=ovmesh::ReceiverConfig{};
        config.synthetic=true;
        config.compact_recording=!detailed;
        config.tuning_offset_hz=900;
        // This artificial fixture key is never used for the synthetic decoder;
        // its channel binding must survive a demo run without replacement.
        require(engine.set_channel_key(0,"UserFixture","101112131415161718191a1b1c1d1e1f",error),error);
        require(engine.set_survey_key(15,"Independent survey fixture","202122232425262728292a2b2c2d2e2f",error),error);
        engine.set_fixed_position(0,0,12);
        const auto unique=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto directory=std::filesystem::current_path()/"engine-test-output"/unique;
        std::filesystem::create_directories(directory);
        config.session_path=(directory/"synthetic.sqlite").string();
        require(engine.start(config,false,error),error);
        require(!engine.set_channel_key(0,"UserFixture","101112131415161718191a1b1c1d1e1f",error),"Changing keys while running rejected");
        require(!engine.set_survey_key(15,"Other fixture","AQ==",error) && engine.has_survey_key(15),"Key-only records remain immutable during reception");
        const bool decoded=wait_for(engine,[](const auto& s){return s.classified_receptions>=1&&s.input_seconds>=5.2;},realtime?35:120);
        if(!decoded) {
            const auto s=engine.snapshot();
            std::cerr<<"Integration progress input="<<s.input_seconds<<"s frames="<<s.total_receptions<<" dropped="<<s.dropped_samples<<" load="<<s.processing_load<<'\n';
        }
        require(decoded,"Wideband synthetic IQ must pass downconversion, LoRa and key-scoped envelope classification");
        const auto before_save=engine.snapshot();
        require(engine.save_session(error),error);
        {
            ovmesh::SessionStore checkpoint;checkpoint.open_readonly(config.session_path);const auto committed=checkpoint.read();
            require(committed.session_id==before_save.session_id&&committed.delivered_samples>=before_save.delivered_samples&&
                committed.classified_receptions>=before_save.classified_receptions&&committed.incomplete,
                "Save confirms a readable live checkpoint without claiming the ongoing session is complete");
            require(engine.snapshot().running,"Explicit Save leaves reception running");
        }
        const auto live_copy=(directory/"live-copy.sqlite").string();
        require(engine.save_session_copy(live_copy,error),error);
        {
            ovmesh::SessionStore copied;copied.open_readonly(live_copy);const auto copy=copied.read();
            require(copy.session_id==before_save.session_id&&copy.delivered_samples>=before_save.delivered_samples&&copy.incomplete,
                "Live Save copy retains a consistent checkpoint and its incomplete-state marker");
        }
        require(engine.snapshot().running&&engine.snapshot().config.session_path==config.session_path,
            "Live Save copy neither stops nor changes the original recording destination");
        require(!engine.save_session_copy(config.session_path,error)&&engine.snapshot().running,
            "Save copy cannot overwrite its active source");
        require(!engine.save_session_copy((directory/"missing"/"copy.sqlite").string(),error)&&engine.snapshot().running,
            "Unavailable copy destination does not interrupt reception");
        engine.stop(); const auto stopped=engine.snapshot();
        require(engine.save_session(error),error);
        require(!stopped.running&&!stopped.recording&&stopped.error.empty(),"Clean stop state");
        require(stopped.dropped_samples==0,"No accepted synthetic input blocks lost");
        require(stopped.delivered_samples>0&&stopped.input_seconds>0&&stopped.measurement_seconds>0,"Input and RF exposure recorded");
        require(stopped.measurement_seconds<=stopped.input_seconds,"RF observation cannot exceed delivered input");
        require(stopped.input_seconds-stopped.measurement_seconds<4096.0/config.sample_rate+1e-7,
            "Every complete accepted FFT contributes exposure; only partial tail is excluded");
        std::cout<<"Fixture classified "<<stopped.classified_receptions<<" likely Meshtastic reception(s); input="
            <<stopped.input_seconds<<"s measured="<<stopped.measurement_seconds<<"s dropped="
            <<stopped.dropped_samples<<" processing_load="<<stopped.processing_load<<'\n'<<std::flush;
        require(stopped.spectrum_tiles>0 && stopped.spectrum_fft_size==4096 &&
            std::abs(stopped.spectrum_bin_width_hz-double(config.sample_rate)/4096)<1e-9 &&
            std::abs(stopped.spectrum_enbw_hz-1.5*stopped.spectrum_bin_width_hz)<1e-3,
            "Spectrum resolution and effective noise bandwidth published");
        // Physical source-rate chirps expose the legacy decoder's known CRC
        // sensitivity. Reception order is not an authorization guarantee: a
        // later failed frame may retain RF metadata, but never decoded content.
        const auto authorized=std::find_if(stopped.receptions.begin(),stopped.receptions.end(),
            [](const auto& r){return r.crc_valid&&r.decoded.status==ovmesh::protocol::Status::classified && r.decoded.evidence.has_value();});
        require(authorized!=stopped.receptions.end(),"Retain a CRC-valid authorized synthetic reception");
        require(authorized->decoded.evidence->port==1 && authorized->decoded.authentication=="not authenticated",
            "Recognized reception retains only unauthenticated envelope evidence");
        for(const auto& reception:stopped.receptions)if(!reception.crc_valid)
            require(!reception.decoded.evidence &&
                reception.decoded.status==ovmesh::protocol::Status::bad_phy_crc,
                "Failed CRC retains categorical RF metadata only");
        require(stopped.config.center_hz==907500000 && stopped.config.tuning_offset_hz==900 &&
            authorized->frequency_hz==config.lanes.front().frequency_hz,
            "Calibration preserves nominal survey and lane frequency metadata");
        require(std::abs(authorized->frequency_error_hz+900)<150,
            "Synthetic corrected tuner shifts received carrier by the explicit offset");
        require(stopped.lane_health.size()==1&&stopped.lane_health.front().phy.completed_frames==stopped.total_receptions&&
            stopped.lane_health.front().phy.headers_valid>=stopped.total_receptions&&
            stopped.lane_health.front().phy.sync_matches>=stopped.total_receptions,
            "Live acquisition diagnostics reach the engine snapshot without frame bytes");
        require(authorized->receiver_position&&authorized->receiver_position->manual,"Fixed receiver position associated");
        require(!stopped.frequencies.empty(),"Final frequency statistics published");
        ovmesh::SessionStore saved;saved.open_readonly(config.session_path);const auto recorded=saved.read();
        require(recorded.classified_receptions>=1&&!recorded.incomplete,"Finalized session reopened");
        require(recorded.receptions.size()==stopped.receptions.size(),"All reception outcomes survive readback");
        for(const auto& reception:recorded.receptions)if(!reception.crc_valid)
            require(!reception.decoded.evidence,
                "Saved CRC failures contain no decoded content or envelope evidence");
        require(recorded.config.tuning_offset_hz==900,"Applied tuning correction survives saved-session readback");
        require(recorded.frequencies.size()==stopped.frequencies.size(),"Final frequencies persisted");
        ovmesh::SurveyAnalysis measured;ovmesh::SurveyQuery survey_query;
        require(engine.analyze_survey(survey_query,measured,error),error);
        require(measured.detailed_available && measured.tile_count==stopped.spectrum_tiles &&
            std::abs(measured.observed_seconds-stopped.measurement_seconds)<1e-7,
            "Saved joint survey retains all complete FFT exposure");
        require(measured.busy_seconds<=measured.observed_seconds+1e-7 && !measured.observations.empty(),
            "Joint activity reports bounded occupied time");
        require(measured.burst_count==stopped.spectrum_bursts && measured.burst_count>0,
            "Saved full-range tile grouping reproduces live candidate count");
        {
            const auto selected_session=engine.snapshot().session_id;
            auto analyze_async=[&] {
                return std::async(std::launch::async,[&] {
                    ovmesh::SurveyAnalysis result;std::string query_error;
                    if(!engine.analyze_survey(ovmesh::SurveyQuery{},result,query_error))throw std::runtime_error(query_error);
                    return result;
                });
            };
            auto first=analyze_async(),second=analyze_async();
            auto metadata=std::async(std::launch::async,[&]{return engine.configured_key_count();});
            require(metadata.wait_for(std::chrono::seconds(5))==std::future_status::ready&&metadata.get()==2,
                "Asynchronous saved reads permit bounded concurrent key-metadata access");
            const auto a=first.get(),b=second.get();
            require(a.tile_count==stopped.spectrum_tiles&&b.tile_count==stopped.spectrum_tiles&&
                std::abs(a.observed_seconds-b.observed_seconds)<1e-9&&engine.snapshot().session_id==selected_session,
                "Concurrent independent readers preserve one saved source and do not replace the selected session");
        }
        survey_query.geographic_filter=true;survey_query.south=-.1;survey_query.north=.1;
        survey_query.west=-.1;survey_query.east=.1;
        require(engine.analyze_survey(survey_query,measured,error) && measured.tile_count==stopped.spectrum_tiles,
            "Detailed survey GPS association survives local query");
        survey_query.south=30;survey_query.north=31;
        require(engine.analyze_survey(survey_query,measured,error) && measured.observed_seconds==0,
            "Unobserved geographic area never becomes measured quiet spectrum");
        for(std::size_t i=0;i<recorded.frequencies.size();++i)
            require(std::abs(recorded.frequencies[i].observed_seconds-stopped.frequencies[i].observed_seconds)<1e-9,"Latest frequency values persisted on stop");
        sqlite3* raw_database=nullptr;
        require(sqlite3_open_v2(config.session_path.c_str(),&raw_database,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK,"Open synthetic window evidence");
        const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> database(raw_database,sqlite3_close);
        // All sample counts in this bounded fixture are below 2^53, so SQLite
        // integer sums remain exactly representable through scalar's double.
        const double complete_samples=scalar(database.get(),"SELECT coalesce(sum(end_sample-first_sample),0) FROM spectrum_tiles");
        const double partial_samples=scalar(database.get(),"SELECT coalesce(sum(missing_samples),0) FROM coverage_gaps WHERE reason='partial_fft'");
        const double application_drops=scalar(database.get(),"SELECT coalesce(sum(missing_samples),0) FROM coverage_gaps WHERE reason='application_drop'");
        require(application_drops==static_cast<double>(stopped.dropped_samples) &&
            recorded.dropped_samples==stopped.dropped_samples,"Every application drop has exactly matching saved gap accounting");
        require(complete_samples+partial_samples==static_cast<double>(stopped.delivered_samples),
            "Accepted input is exactly complete measured FFTs plus explicitly unmeasured partial FFT samples");
        require(std::abs(complete_samples/config.sample_rate-stopped.measurement_seconds)<1e-7,
            "Reported measurement exposure equals the complete FFT sample count");
        require(scalar(database.get(),"SELECT count(*) FROM coverage_gaps WHERE reason NOT IN ('application_drop','partial_fft') OR missing_samples<=0 OR elapsed_start<0 OR elapsed_end<=elapsed_start OR utc_end<=utc_start OR abs((elapsed_end-elapsed_start)*(SELECT sample_rate FROM session)-missing_samples)>0.0001 OR abs((utc_end-utc_start)-(elapsed_end-elapsed_start))>0.00001")==0,
            "Synthetic coverage gaps have known reasons, positive sample counts and consistent time intervals");
        require(scalar(database.get(),"SELECT count(*) FROM coverage_gaps a JOIN coverage_gaps b ON a.id<b.id AND a.elapsed_start<b.elapsed_end-1e-9 AND a.elapsed_end>b.elapsed_start+1e-9")==0,
            "Known coverage gaps never double-count the same missing interval");
        require(scalar(database.get(),"SELECT count(*) FROM coverage_gaps g JOIN spectrum_tiles t ON t.elapsed_start<g.elapsed_end-1e-9 AND t.elapsed_end>g.elapsed_start+1e-9")==0,
            "Missing samples never become measured quiet or busy spectrum tiles");
        const double timeline_end=scalar(database.get(),"SELECT max(elapsed_end) FROM (SELECT elapsed_end FROM spectrum_tiles UNION ALL SELECT elapsed_end FROM coverage_gaps)");
        require(std::abs(timeline_end*config.sample_rate-static_cast<double>(stopped.delivered_samples)-application_drops)<0.0001,
            "Synthetic sample timeline contains only accepted input and explicitly recorded application loss");
        if(config.compact_recording) {
            require(scalar(database.get(),"PRAGMA user_version")==6,"Default engine recording uses compact schema");
            require(scalar(database.get(),"SELECT count(*) FROM survey_windows")==0 &&
                scalar(database.get(),"SELECT count(*) FROM window_bins")==0,
                "Compact recording omits duplicate legacy windows while preserving fine measurement exposure");
            require(scalar(database.get(),"SELECT count(*) FROM power_blocks")>0 &&
                scalar(database.get(),"SELECT count(*) FROM power_blocks WHERE elapsed_end-elapsed_start>1.0000001")==0,
                "Engine compact power has bounded support");
            require(scalar(database.get(),"SELECT count(*) FROM positions")==1,
                "The unchanged manual GPS fix is stored once across all fine tiles");
        } else {
        require(scalar(database.get(),"SELECT count(*) FROM survey_windows")>=2,"Periodic and final partial survey windows persisted");
#ifndef OVMESH_TEST_SANITIZED
        require(scalar(database.get(),"SELECT count(*) FROM survey_windows WHERE elapsed_end<=elapsed_start OR utc_end<=utc_start OR elapsed_end-elapsed_start>5.02 OR associated_at!='window-end' OR receiver_manual!=1 OR receiver_latitude!=0 OR receiver_longitude!=0")==0,"Window durations and explicit fixed-position association preserved");
#else
        // A window closes at the next measured tile. Under overload that point
        // may follow explicit unobserved gaps; only those independently checked
        // gap intersections are removed from the original duration bound.
        require(scalar(database.get(),"SELECT count(*) FROM survey_windows w WHERE elapsed_end<=elapsed_start OR utc_end<=utc_start OR elapsed_end-elapsed_start-coalesce((SELECT sum(min(g.elapsed_end,w.elapsed_end)-max(g.elapsed_start,w.elapsed_start)) FROM coverage_gaps g WHERE g.elapsed_start<w.elapsed_end AND g.elapsed_end>w.elapsed_start),0)>5.02 OR associated_at!='window-end' OR receiver_manual!=1 OR receiver_latitude!=0 OR receiver_longitude!=0")==0,"Observed window durations and explicit fixed-position association preserved under accounted overload");
#endif
        require(scalar(database.get(),"SELECT count(*) FROM window_bins b JOIN survey_windows w ON b.window_id=w.id WHERE b.observed<0 OR b.active<0 OR b.active>b.observed+1e-9 OR b.observed>w.elapsed_end-w.elapsed_start+1e-9")==0,"Per-window RF exposure is bounded by observation duration");
        require(scalar(database.get(),"SELECT max(abs(f.observed-w.observed)) FROM frequencies f JOIN (SELECT center,sum(observed) AS observed FROM window_bins GROUP BY center) w ON f.center=w.center")<1e-8,"Window observations sum to final frequency exposure");
        }
        const auto uppercase_geojson=(directory/"survey.GEOJSON").string();
        ovmesh::ExportOptions export_options;
        require(!engine.export_session(uppercase_geojson,export_options,error) &&
            !std::filesystem::exists(uppercase_geojson),"Uppercase GeoJSON suffix still requires explicit GPS export consent");
        export_options.include_receiver_positions=true;
        require(engine.export_session(uppercase_geojson,export_options,error),error);
        { std::ifstream file(uppercase_geojson); char first=0;file.get(first);
          require(first=='{',"Uppercase GeoJSON output is JSON, not CSV"); }
        require(!engine.export_session(uppercase_geojson,export_options,error),"Export cannot replace an existing file");
        const auto finished_copy=(directory/"finished-copy.sqlite").string();
        require(engine.save_session_copy(finished_copy,error),error);
        require(engine.open_session(finished_copy,error),error);
        require(engine.snapshot().historical&&!engine.snapshot().incomplete,"Stopped Save copy reopens as complete historical data");
        require(!engine.save_session(error)&&error.find("read-only")!=std::string::npos,
            "Historical Save refuses to modify the source");
        require(!engine.save_session_copy(finished_copy,error),"Historical Save copy cannot overwrite itself");
        const auto historical_copy=(directory/"historical-copy.sqlite").string();
        require(engine.save_session_copy(historical_copy,error),error);
        require(engine.open_session(historical_copy,error),error);
        {
            sqlite3* changed=nullptr;
            require(sqlite3_open_v2(historical_copy.c_str(),&changed,SQLITE_OPEN_READWRITE,nullptr)==SQLITE_OK,
                "Open synthetic source replacement fixture");
            const auto rc=sqlite3_exec(changed,"UPDATE session SET id='different-synthetic-session'",nullptr,nullptr,nullptr);
            sqlite3_close(changed);require(rc==SQLITE_OK,"Replace only synthetic source identity");
            ovmesh::SurveyAnalysis rejected;
            require(!engine.analyze_survey(ovmesh::SurveyQuery{},rejected,error)&&error.find("selected session")!=std::string::npos,
                "Analysis refuses a file whose identity changed after selection");
            require(!engine.save_session_copy((directory/"wrong-source-copy.sqlite").string(),error),
                "Save copy refuses a file whose identity changed after selection");
        }
        require(engine.new_session(error),error);
        const auto fresh=engine.snapshot();
        require(fresh.session_id.empty()&&fresh.config.session_path.empty()&&!fresh.running&&!fresh.historical&&
            fresh.receptions.empty()&&fresh.frequencies.empty()&&fresh.track.empty()&&fresh.spectrum_dbfs.empty()&&
            fresh.delivered_samples==0&&fresh.config.center_hz==config.center_hz&&fresh.config.tuning_offset_hz==900,
            "New clears historical results and old path while preserving receiver configuration");
        require(engine.has_channel_key(0,"UserFixture")&&engine.has_survey_key(15)&&
            fresh.gps_status=="Manual fixed receiver position","New preserves configured keys and GPS source");
        require(!engine.analyze_survey(ovmesh::SurveyQuery{},measured,error),"New cannot silently query the previous saved session");
        require(engine.open_session(config.session_path,error)&&engine.snapshot().delivered_samples==stopped.delivered_samples,
            "Original recording remains intact after Copy, Open and New");
        require(!engine.start(config,false,error)&&error.find("read-only")!=std::string::npos,
            "Historical survey cannot be silently resumed or replaced by Start");
        require(engine.new_session(error),error);
        auto hardware=config;hardware.synthetic=false;hardware.session_path.clear();hardware.lanes[0].channel_name="UserFixture";
        require(!engine.start(hardware,false,error)&&error.find("permission")!=std::string::npos,"Demo preserves user channel-key binding");
        hardware.lanes[0].channel_name="DifferentFixture";hardware.lanes[0].label="Other RF profile";
        hardware.lanes[0].frequency_hz+=250000;
        require(!engine.start(hardware,false,error)&&error.find("permission")!=std::string::npos,"Changing RF settings does not rebind or invalidate the independent keyring");
        hardware.lanes.clear();
        require(!engine.start(hardware,false,error)&&error.find("permission")!=std::string::npos,"No RF lanes still requires hardware permission");
        require(engine.has_channel_key(0,"UserFixture") && !engine.has_channel_key(0,"DifferentFixture") &&
            engine.has_survey_key(15) && engine.key_records()[15].label=="Independent survey fixture" &&
            engine.configured_key_count()==2,"Demo, changed RF settings and failed starts preserve explicit key scopes");
        engine.clear_keys();
        require(!engine.has_channel_key(0,"UserFixture") && !engine.has_survey_key(15) &&
            engine.configured_key_count()==0,"Clear removes all readiness after synthetic lifecycle");
        require(!engine.start(hardware,false,error)&&error.find("permission")!=std::string::npos,"Cleared keys and demo keys do not survive into hardware profile");
        config.session_path.clear();
        for(unsigned attempt=0;attempt<3;++attempt) {
            if(attempt==2) require(engine.set_survey_key(15,"Clear while receiving fixture","202122232425262728292a2b2c2d2e2f",error),error);
            require(engine.start(config,false,error),error);
            require(wait_for(engine,[](const auto& s){return s.delivered_samples>0;},realtime?5:30),"Restart receives input");
            if(attempt==0) {
                const auto memory=engine.snapshot();
                require(!engine.save_session(error)&&!engine.save_session_copy((directory/"memory.sqlite").string(),error),
                    "Memory-only sessions cannot fabricate saved history");
                require(!engine.new_session(error)&&engine.snapshot().running&&engine.snapshot().session_id==memory.session_id,
                    "New refuses unrecorded-data loss without stopping a memory-only session");
            }
            if(attempt==2) {
                engine.clear_keys();
                require(engine.configured_key_count()==0,"Clearing keys stops reception before clearing the keyring");
            } else engine.stop();
            const auto ended=engine.snapshot();
            require(!ended.running&&ended.error.empty()&&ended.dropped_samples==0,"Early interruption drains accepted input cleanly");
            require(!engine.new_session(error)&&engine.snapshot().delivered_samples==ended.delivered_samples,
                "Stopped memory-only results require explicit discard confirmation");
            require(engine.new_session(error,true)&&engine.snapshot().delivered_samples==0&&engine.snapshot().session_id.empty(),
                "Explicitly confirmed New clears memory-only results");
        }
        config.lanes.clear();config.session_path=(directory/"new-while-live.sqlite").string();
        require(engine.start(config,false,error),error);
        require(wait_for(engine,[](const auto& s){return s.measurement_seconds>=.1;},realtime?5:30),"Saved New fixture has measurements");
        const auto before_new=engine.snapshot();
        require(engine.new_session(error),error);
        require(!engine.snapshot().running&&engine.snapshot().session_id.empty(),"New stops an active saved session and leaves a blank workspace");
        {
            ovmesh::SessionStore previous;previous.open_readonly(config.session_path);const auto retained=previous.read();
            require(!retained.incomplete&&retained.delivered_samples>=before_new.delivered_samples,
                "New finalizes accepted input before releasing the previous recording");
        }
        config.session_path=(directory/"unavailable-saved-file.sqlite").string();
        require(engine.start(config,false,error),error);
        require(wait_for(engine,[](const auto& s){return s.measurement_seconds>=.1;},realtime?5:30),"Unavailable-file fixture has measurements");
        engine.stop();const auto unavailable=engine.snapshot();
        const auto relocated=directory/"temporarily-relocated.sqlite";
        std::filesystem::rename(config.session_path,relocated);
        require(!engine.save_session(error)&&!engine.new_session(error,true)&&
            engine.snapshot().session_id==unavailable.session_id&&engine.snapshot().delivered_samples==unavailable.delivered_samples,
            "Even discard consent does not clear a recorded session when saved-file verification fails");
        std::filesystem::rename(relocated,config.session_path);
        require(engine.save_session(error)&&engine.new_session(error),"Restored saved file permits a verified safe reset");
        require(!engine.start(config,false,error),"Starting on an existing recording path must fail");
        const auto misleading_copy=(directory/"must-not-copy-another-session.sqlite").string();
        require(!engine.save_session_copy(misleading_copy,error)&&!std::filesystem::exists(misleading_copy),
            "Failed new-file creation cannot make Save copy copy an unrelated older recording");
        check_resume(directory,detailed);
        std::cout<<"Engine integration passed: native wideband decode, storage, key isolation, lifecycle; no hardware opened\n";
        return EXIT_SUCCESS;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return EXIT_FAILURE;}
}
