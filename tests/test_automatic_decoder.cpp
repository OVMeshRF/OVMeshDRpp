// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/engine.hpp"
#include "ovmesh/report.hpp"
#include "storage.hpp"
#include <sqlite3.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <stdexcept>
#include <thread>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void sql(const std::filesystem::path& path,const char* command) {
    sqlite3* db=nullptr;
    if(sqlite3_open(path.string().c_str(),&db)!=SQLITE_OK)throw std::runtime_error("Test database open failed");
    const auto result=sqlite3_exec(db,command,nullptr,nullptr,nullptr);sqlite3_close(db);
    require(result==SQLITE_OK,"Test SQL failed");
}
std::string contents(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);require(input.good(),"Read generated export");
    return {std::istreambuf_iterator<char>(input),{}};
}
using StatusRow=std::array<std::uint64_t,6>;
std::vector<StatusRow> saved_decoder_rows(const std::filesystem::path& path) {
    sqlite3* db=nullptr;
    require(sqlite3_open_v2(path.string().c_str(),&db,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK,"Read generated decoder rows");
    sqlite3_stmt* statement=nullptr;
    const auto code=sqlite3_prepare_v2(db,"SELECT acquisition_id,available,enabled,finished,classified,completed FROM automatic_decoder ORDER BY acquisition_id",-1,&statement,nullptr);
    if(code!=SQLITE_OK){sqlite3_close(db);throw std::runtime_error("Prepare decoder rows");}
    std::vector<StatusRow> rows;bool valid=true;int result=SQLITE_OK;
    while((result=sqlite3_step(statement))==SQLITE_ROW) {
        StatusRow row{};
        for(int i=0;i<6;++i){valid=valid&&sqlite3_column_type(statement,i)==SQLITE_INTEGER&&sqlite3_column_int64(statement,i)>=0;row[i]=sqlite3_column_int64(statement,i);}
        rows.push_back(row);
    }
    sqlite3_finalize(statement);sqlite3_close(db);
    require(valid&&result==SQLITE_DONE,"Decoder status columns retain integer types");return rows;
}
using CsvRow=std::map<std::string,std::string>;
std::vector<CsvRow> decoder_csv_rows(const std::filesystem::path& path) {
    // Retain only the three diagnostic rows, not all frequency/event records.
    std::vector<CsvRow> result;std::vector<std::string> header,row;std::string field;bool quoted=false;
    const auto data=contents(path);
    for(size_t i=0;i<data.size();++i) {
        const char c=data[i];
        if(quoted){if(c=='"'&&i+1<data.size()&&data[i+1]=='"'){field+='"';++i;}else if(c=='"')quoted=false;else field+=c;}
        else if(c=='"')quoted=true;
        else if(c==','){row.push_back(field);field.clear();}
        else if(c=='\n') {
            row.push_back(field);field.clear();
            if(header.empty())header=row;
            else {
                require(row.size()==header.size(),"CSV diagnostic row column count");
                CsvRow record;for(size_t n=0;n<row.size();++n)record.emplace(header[n],row[n]);
                if(record.at("record_type")=="automatic_decoder_status")result.push_back(std::move(record));
            }
            row.clear();
        } else if(c!='\r')field+=c;
    }
    require(!quoted&&field.empty()&&row.empty(),"Complete CSV framing");return result;
}
void exports_and_report(const ovmesh::SessionStore& reader,const std::filesystem::path& directory,
                        const std::vector<StatusRow>& expected,const ovmesh::Snapshot& saved) {
    constexpr std::array<const char*,6> fields{"acquisition_id","decoder_available","decoder_enabled","decoder_finished","decoder_classified","decoder_completed"};
    const auto csv=directory/"mixed.csv";reader.export_csv(csv.string(),{});
    const auto rows=decoder_csv_rows(csv);require(rows.size()==expected.size(),"CSV includes every acquisition decoder status");
    for(size_t i=0;i<rows.size();++i) {
        for(size_t n=0;n<fields.size();++n)require(rows[i].at(fields[n])==std::to_string(expected[i][n]),"CSV decoder status matches numeric stored evidence");
        require(rows[i].at("acquisition_scope").find("one acquisition")!=std::string::npos,"CSV describes per-acquisition scope");
    }
    ovmesh::ExportOptions privacy;privacy.include_receiver_positions=true;
    const auto geo=directory/"mixed.geojson";reader.export_geojson(geo.string(),privacy);const auto json=contents(geo);
    const std::regex record(R"(\{[^{}]*"record_type":"automatic_decoder_status"[^{}]*\})");
    size_t count=0;
    for(auto it=std::sregex_iterator(json.begin(),json.end(),record);it!=std::sregex_iterator();++it) {
        require(count<expected.size(),"No extra GeoJSON decoder records");const auto object=it->str();
        for(size_t n=0;n<fields.size();++n) {
            const std::regex numeric("\""+std::string(fields[n])+"\":([0-9]+)([,}])");std::smatch match;
            require(std::regex_search(object,match,numeric)&&match[1].str()==std::to_string(expected[count][n]),
                "GeoJSON status is a matching JSON number, not text or a missing field");
        }
        ++count;
    }
    require(count==expected.size(),"GeoJSON preserves unlocated status for all acquisitions");
    ovmesh::ReportOptions report;report.kind=ovmesh::ReportKind::Analysis;
    report.query.elapsed_start=saved.acquisitions.back().elapsed_start_seconds;
    const auto latest=directory/"latest.html";ovmesh::export_survey_report(reader,latest.string(),report);
    require(contents(latest).find("<td>Automatic PHY decoding (latest acquisition)</td><td>Finished</td>")!=std::string::npos,
        "Analysis report labels the completed latest-acquisition decoder state");
    report.query.elapsed_start=0;report.query.elapsed_end=saved.acquisitions.front().elapsed_end_seconds;
    const auto first=directory/"first.html";ovmesh::export_survey_report(reader,first.string(),report);
    require(contents(first).find("<td>Automatic PHY decoding (latest acquisition)</td><td>Unavailable</td>")!=std::string::npos,
        "Earlier-only report does not borrow latest-acquisition decoder counters");
}
}
int main() {
    try {
        const auto directory=std::filesystem::current_path()/"automatic-decoder-test-output"/
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(directory);
        const auto path=directory/"automatic.sqlite";
        ovmesh::Engine engine(ovmesh::Engine::SyntheticPacing::ConsumerPaced);ovmesh::ReceiverConfig config;std::string error;
        config.synthetic=true;config.sample_rate=8000000;config.center_hz=906800000;
        config.survey_span_hz=1000000;config.lanes.clear();config.discover_lora=true;config.automatic_decode=true;
        config.session_path=path.string();
        require(engine.start(config,false,error),error.c_str());
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
        while(std::chrono::steady_clock::now()<deadline) {
            const auto current=engine.snapshot();require(current.error.empty(),current.error.c_str());
            require(!current.discovery.failed,current.discovery.fault.c_str());
            if(current.classified_receptions)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        engine.stop();const auto final=engine.snapshot();
        require(final.error.empty(),final.error.c_str());
        require(final.config.lanes.empty()&&final.lane_health.empty(),"Automatic path must not install fixed RF lanes");
        require(final.automatic_decoder.available&&final.automatic_decoder.enabled&&final.automatic_decoder.finished,
            "Automatic decoder mode/drain is explicit");
        require(final.automatic_decoder.crc_valid&&final.classified_receptions&&final.automatic_decoder.classified,
            "A discovered waveform must pass PHY and native classification with no configured frequency profile");
        require(engine.configured_key_count()==0,"Generated fixture keys must never enter the user keyring");
        require(final.measurement_seconds>0,"Spectrum measurement must continue independently");
        ovmesh::SessionStore reader;reader.open_readonly(path.string());const auto saved=reader.read();
        require(saved.automatic_decoder.available&&saved.automatic_decoder.enabled&&
            saved.automatic_decoder.classified==final.automatic_decoder.classified&&saved.acquisitions.back().config.automatic_decode,
            "Save/reopen must preserve automatic scope and classification counters");
        const auto copy=directory/"copy.sqlite";reader.save_copy(copy.string());
        ovmesh::SessionStore copied;copied.open_readonly(copy.string());
        require(copied.read().automatic_decoder.completed==saved.automatic_decoder.completed,"Save copy preserves decoder metadata");

        // Stopping pauses this survey. Turning automatic decoding off and back
        // on must preserve every earlier acquisition and its own diagnostics.
        auto resume=[&](bool enabled) {
            const auto before=engine.snapshot();config.automatic_decode=enabled;
            require(engine.start(config,false,error),error.c_str());
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);
            bool progressed=false;
            while(std::chrono::steady_clock::now()<end) {
                const auto current=engine.snapshot();require(current.error.empty(),current.error.c_str());
                require(!current.discovery.failed,current.discovery.fault.c_str());
                if(enabled?current.automatic_decoder.classified>0:current.input_seconds>before.input_seconds+0.25){progressed=true;break;}
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            engine.stop();const auto after=engine.snapshot();
            require(progressed&&after.error.empty(),"Resumed synthetic acquisition makes progress without error");
            require(after.session_id==before.session_id&&after.acquisitions.size()==before.acquisitions.size()+1,
                "Resume preserves session identity and appends an acquisition");
            require(after.automatic_decoder.available&&after.automatic_decoder.enabled==enabled,
                "Current diagnostics use the resumed acquisition mode");
            return after;
        };
        const auto disabled=resume(false);
        require(disabled.classified_receptions==final.classified_receptions&&disabled.automatic_decoder.started==0&&
            disabled.automatic_decoder.completed==0&&disabled.automatic_decoder.classified==0,
            "Disabled acquisition adds no decoder work or classification, preserving earlier results");
        const auto resumed=resume(true);
        require(resumed.classified_receptions==final.classified_receptions+resumed.automatic_decoder.classified,
            "Session-wide classifications accumulate while automatic diagnostics describe only the current acquisition");
        const auto mixed=reader.read();
        require(mixed.acquisitions.size()==3&&mixed.acquisitions[0].config.automatic_decode&&
            !mixed.acquisitions[1].config.automatic_decode&&mixed.acquisitions[2].config.automatic_decode,
            "Reopen preserves on/off/on decoder setup");
        require(mixed.automatic_decoder.classified==resumed.automatic_decoder.classified&&
            mixed.classified_receptions==resumed.classified_receptions,"Reopen preserves latest and cumulative counter scopes");
        const std::vector<StatusRow> expected{
            {1,1,1,1,final.automatic_decoder.classified,final.automatic_decoder.completed},
            {2,1,0,std::uint64_t(disabled.automatic_decoder.finished),0,0},
            {3,1,1,1,resumed.automatic_decoder.classified,resumed.automatic_decoder.completed}};
        require(saved_decoder_rows(path)==expected,"All acquisition diagnostic rows survive resume");
        const auto mixed_copy=directory/"mixed-copy.sqlite";reader.save_copy(mixed_copy.string());
        require(saved_decoder_rows(mixed_copy)==expected,"Save copy preserves all acquisition diagnostic rows");
        exports_and_report(reader,directory,expected,mixed);

        // Canonical extension validation must reject tampering and remain able
        // to read pre-extension metadata-only histories without claiming setup.
        const auto old=directory/"old.sqlite";reader.save_copy(old.string());
        sql(old,"DROP TABLE automatic_decoder;");
        ovmesh::SessionStore old_reader;old_reader.open_readonly(old.string());
        require(!old_reader.read().automatic_decoder.available,"Older diagnostics remain unknown, not zero activity");
        const auto old_copy=directory/"old-copy.sqlite";old_reader.save_copy(old_copy.string());
        ovmesh::SessionStore old_copied;old_copied.open_readonly(old_copy.string());
        require(!old_copied.read().automatic_decoder.available,"Copy of old diagnostics must remain unavailable");
        const auto bad=directory/"bad.sqlite";reader.save_copy(bad.string());
        sql(bad,"UPDATE automatic_decoder SET version=99;");
        bool rejected=false;try{ovmesh::SessionStore invalid;invalid.open_readonly(bad.string());(void)invalid.read();}catch(const std::exception&){rejected=true;}
        require(rejected,"Unknown decoder metadata version must fail closed");
        std::cout<<"Automatic discovery, classification, recording, copy and legacy metadata checks passed\n";
        // Test-only generated data, never operational recordings.
        std::filesystem::remove_all(directory);
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
