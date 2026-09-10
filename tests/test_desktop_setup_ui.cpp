// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual desktop setup/start methods, with explicit test-only
// preference folders. No GLFW window, OS discovery, USB device or user profile.
#include "../src/ui.cpp"
#include <imgui_internal.h>
#include <fstream>
#include <iostream>

namespace {
namespace fs=std::filesystem;
using namespace ovmesh;
void require(bool value,const std::string& message) {if(!value)throw std::runtime_error(message);}
fs::path file_path(const std::string& text) {return fs::path(std::u8string(text.begin(),text.end()));}
std::string contents(const std::string& filename) {
    std::ifstream file(file_path(filename),std::ios::binary);
    require(file.good(),"Read synthetic fixture file");
    return {std::istreambuf_iterator<char>(file),{}};
}
struct Fixture {
    fs::path directory;
    Fixture() {
        directory=fs::current_path()/("desktop-setup-fixture-"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(fs::create_directory(directory),"Create isolated desktop fixture");
    }
    ~Fixture() {std::error_code error;fs::remove_all(directory,error);}
    std::string path(const char* name) const {return path_utf8(directory/name);}
};
void configure_synthetic(DesktopState& ui) {
    ui.source=0;ui.config.sample_rate=8000000;ui.config.survey_span_hz=5000000;
    // Keep the operator's armed profile intact while bounding this storage-only
    // fixture to energy measurements. No waveform discovery worker is needed.
    ui.spectrum_only=true;ui.config.discover_lora=false;
}
void wait_for_survey(Engine& engine) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<end) {
        const auto snapshot=engine.snapshot();
        require(snapshot.error.empty(),snapshot.error);
        if(snapshot.input_seconds>.15&&snapshot.spectrum_tiles>0)return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("Synthetic desktop start did not produce bounded survey measurements");
}
void defaults_folder_and_optouts(const Fixture& fixture) {
    const auto profile=fixture.path("remembered-profile");
    DesktopState first;first.initialize_preferences(profile,false);
    require(first.preferences_active&&first.preferences_ready&&first.preferences_error.empty(),
        "Fresh desktop setup initializes explicit local settings");
    require(first.gps_enabled&&first.save_session&&first.focus_serial_gps,
        "Ordinary desktop defaults enable GPS and recording and expose serial GPS setup");
    require(first.config.discover_lora&&first.decode_enabled&&!first.spectrum_only&&first.config.lanes.size()==1,
        "Fresh ordinary setup enables discovery and arms a profile without implicitly adding a key");
    require(first.gps_devices.devices.empty()&&!first.selected_gps,
        "Tests explicitly skip OS device discovery");
    require(!first.session_path.empty()&&file_path(first.session_path).extension()==".sqlite"&&
        !fs::exists(file_path(first.session_path)),"Initialization selects a new SQLite filename without recording");
    require(first.config.session_path.empty(),"Setup alone does not activate a recording session");
    const auto initial=first.session_path;
    const auto folder=file_path(fixture.path("chosen-surveys"));
    require(fs::create_directory(folder),"Create alternate local recording folder");
    first.preferences.recording_directory=path_utf8(folder);
    first.prepare_recording_file();first.persist_preferences();
    require(first.preferences_error.empty()&&file_path(first.session_path).parent_path()==folder,
        "Changing the recording folder prepares its next unused filename");
    DesktopState second;second.initialize_preferences(profile,false);
    require(second.preferences_ready&&second.preferences.recording_directory==path_utf8(folder)&&
        file_path(second.session_path).parent_path()==folder,"Recording folder survives a fresh desktop instance");
    require(second.session_path!=first.session_path&&second.session_path!=initial&&
        !fs::exists(file_path(second.session_path)),"Reopening chooses another unused filename without creating a survey");
    second.gps_enabled=false;second.save_session=false;second.persist_preferences();
    require(second.preferences_error.empty(),"Explicit GPS and recording opt-outs save successfully");
    DesktopState third;third.initialize_preferences(profile,false);
    require(third.preferences_ready&&!third.gps_enabled&&!third.save_session&&third.session_path.empty(),
        "Saved opt-outs survive restart rather than re-enabling recording or GPS");
    require(third.preferences.recording_directory==path_utf8(folder),"Opt-out preserves chosen folder for later reuse");
    third.gps_enabled=true;third.save_session=true;third.prepare_recording_file();third.persist_preferences();
    DesktopState fourth;fourth.initialize_preferences(profile,false);
    require(fourth.gps_enabled&&fourth.save_session&&!fourth.session_path.empty(),
        "Re-enabled GPS and recording settings survive restart");
}
void receiver_offset_survives_restart(const Fixture& fixture) {
    const auto profile=fixture.path("receiver-offset-profile");
    DesktopState first;first.initialize_preferences(profile,false);
    require(first.preferences_ready&&first.config.tuning_offset_hz==0,
        "Fresh desktop receiver correction defaults to zero Hz");
    first.config.tuning_offset_hz=900;first.persist_preferences();
    require(first.preferences_error.empty(),"Positive receiver correction saves successfully");
    DesktopState positive;positive.initialize_preferences(profile,false);
    require(positive.preferences_ready&&positive.config.tuning_offset_hz==900&&
        positive.preferences.tuning_offset_hz==900,
        "A fresh desktop instance restores the operator's +900 Hz correction with its sign and units");
    require(positive.config.session_path.empty()&&!fs::exists(file_path(positive.session_path))&&
        positive.gps_devices.devices.empty()&&!positive.selected_gps,
        "Restoring correction alone neither starts recording nor discovers or selects a device");
    positive.config.tuning_offset_hz=-900;positive.persist_preferences();
    require(positive.preferences_error.empty(),"Negative receiver correction saves successfully");
    DesktopState negative;negative.initialize_preferences(profile,false);
    require(negative.preferences_ready&&negative.config.tuning_offset_hz==-900&&
        negative.preferences.tuning_offset_hz==-900,
        "A fresh desktop instance preserves a negative receiver correction rather than its magnitude");
    negative.config.tuning_offset_hz=0;negative.persist_preferences();
    require(negative.preferences_error.empty(),"Explicitly clearing receiver correction saves successfully");
    DesktopState cleared;cleared.initialize_preferences(profile,false);
    require(cleared.preferences_ready&&cleared.config.tuning_offset_hz==0&&
        cleared.preferences.tuning_offset_hz==0,
        "Clearing correction persists zero rather than restoring an earlier nonzero offset");

    const auto settings=cleared.preference_locations.settings_file;
    const auto original_bytes=contents(settings);
    // Passive, explicit test and saved-view startup paths leave preferences
    // uninitialized. Exercise that persistence guard without opening a window.
    DesktopState isolated;
    isolated.preference_locations=cleared.preference_locations;
    isolated.config.tuning_offset_hz=1700;
    require(!isolated.preferences_active&&!isolated.preferences_ready,
        "An isolated desktop state has not activated ordinary profile preferences");
    isolated.persist_preferences();
    require(contents(settings)==original_bytes,
        "Changing an offset without ordinary preference initialization leaves the existing profile byte-identical");
    isolated.passive_smoke=true;isolated.config.tuning_offset_hz=-1700;isolated.persist_preferences();
    require(contents(settings)==original_bytes,
        "Passive test state cannot overwrite the ordinary receiver correction");
}
void consecutive_recordings(const Fixture& fixture) {
    DesktopState ui;ui.initialize_preferences(fixture.path("recording-profile"),false);
    configure_synthetic(ui);Engine engine;
    const auto first_path=ui.session_path;
    ui.start(engine,false);
    require(!ui.notice_error&&engine.snapshot().running&&engine.snapshot().recording,
        "Normal desktop synthetic start saves by default");
    require(engine.gps_connection_status().state==GpsConnectionState::Disconnected,
        "Synthetic start does not connect GPS even when the convenience preference is enabled");
    wait_for_survey(engine);engine.stop();
    require(!engine.snapshot().incomplete&&fs::is_regular_file(file_path(first_path)),
        "First bounded synthetic recording is complete");
    const auto first_bytes=contents(first_path);
    require(!first_bytes.empty()&&ui.recording_path_used,"Completed start marks the chosen recording path used");
    ui.start(engine,false);
    require(!ui.notice_error&&engine.snapshot().running&&engine.snapshot().recording&&ui.session_path!=first_path,
        "Second desktop start automatically uses a fresh file");
    const auto second_path=ui.session_path;
    wait_for_survey(engine);engine.stop();
    require(!engine.snapshot().incomplete&&fs::is_regular_file(file_path(second_path))&&
        contents(first_path)==first_bytes,"Second recording completes without modifying the first survey");
    const auto second_bytes=contents(second_path);
    ui.config.sample_rate=0;ui.start(engine,false);
    const auto failed_path=ui.session_path;
    require(ui.notice_error&&!engine.snapshot().running&&failed_path!=first_path&&failed_path!=second_path&&
        !fs::exists(file_path(failed_path)),"Invalid synthetic configuration stops before recording to a newly allocated path");
    configure_synthetic(ui);ui.start(engine,false);
    require(!ui.notice_error&&engine.snapshot().running&&ui.session_path!=failed_path,
        "Retry after failed startup allocates another fresh destination");
    wait_for_survey(engine);engine.stop();
    require(!engine.snapshot().incomplete&&contents(first_path)==first_bytes&&contents(second_path)==second_bytes,
        "Failed start and successful retry preserve both earlier recordings byte for byte");
    std::string error;Engine reopened;
    require(reopened.open_session(first_path,error)&&reopened.snapshot().historical&&
        reopened.snapshot().spectrum_tiles>0&&!reopened.snapshot().incomplete,
        "First recording remains a readable completed measurement survey");
    require(reopened.open_session(second_path,error)&&reopened.snapshot().historical&&
        reopened.snapshot().spectrum_tiles>0&&!reopened.snapshot().incomplete,
        "Second recording is independently readable");
}
void unavailable_settings_or_folder(const Fixture& fixture) {
    const auto malformed_profile=fixture.path("malformed-profile");
    DesktopState original;original.initialize_preferences(malformed_profile,false);
    require(original.preferences_ready,"Initialize settings file before controlled corruption");
    const auto settings=original.preference_locations.settings_file;
    const std::string malformed="version=unknown\nrecording_enabled=0\n";
    {
        std::ofstream file(file_path(settings),std::ios::binary|std::ios::trunc);file<<malformed;
        file.close();require(file.good(),"Write malformed test settings");
    }
    DesktopState invalid;invalid.initialize_preferences(malformed_profile,false);
    require(!invalid.preferences_ready&&!invalid.preferences_error.empty()&&invalid.save_session&&
        invalid.session_path.empty(),"Malformed preferences remain visible and cannot silently disable saving");
    configure_synthetic(invalid);Engine engine;invalid.start(engine,false);
    require(invalid.notice_error&&!engine.snapshot().running&&contents(settings)==malformed,
        "Malformed settings stop an unconfigured recording and are never overwritten");

    const auto unavailable_profile=fixture.path("unavailable-folder-profile");
    DesktopState writer;writer.initialize_preferences(unavailable_profile,false);
    const auto missing=fixture.path("removed-recording-folder");
    require(!fs::exists(file_path(missing)),"Missing recording folder fixture is absent");
    writer.preferences.recording_directory=missing;writer.persist_preferences();
    require(writer.preferences_error.empty(),"Remembered folder may become unavailable between sessions");
    DesktopState reader;reader.initialize_preferences(unavailable_profile,false);
    require(reader.preferences_ready&&reader.save_session&&reader.session_path.empty()&&reader.notice_error,
        "Unavailable remembered folder reports an error without silently switching off recording");
    configure_synthetic(reader);reader.start(engine,false);
    require(reader.notice_error&&!engine.snapshot().running&&!fs::exists(file_path(missing)),
        "Start does not invent a folder or fall back to an unrecorded survey");
}

class SummaryUi {
public:
    SummaryUi() {
        ImGui::CreateContext();auto& io=ImGui::GetIO();
        io.IniFilename=nullptr;io.LogFilename=nullptr;io.DeltaTime=1.0f/60.0f;
        unsigned char* pixels=nullptr;int width=0,height=0;
        io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
        require(pixels&&width>0&&height>0,"Create summary test font atlas in memory");
    }
    ~SummaryUi(){ImGui::DestroyContext();}
    std::string frame(const Snapshot& snapshot,ImVec2 size={1100,650},float scale=1,bool capture=false) {
        auto& io=ImGui::GetIO();io.DisplaySize=size;io.FontGlobalScale=scale;
        ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(size);
        ImGui::Begin("Summary fixture",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoCollapse);
        // Captures rendered labels in memory only; never a log file or clipboard.
        if(capture)ImGui::LogToBuffer();
        frequency_summary(snapshot);
        std::string text;
        if(capture){text=ImGui::GetCurrentContext()->LogBuffer.c_str();ImGui::LogFinish();}
        ImGui::End();ImGui::Render();
        const auto* context=ImGui::GetCurrentContext();
        require(context->CurrentWindowStack.empty()&&context->BeginPopupStack.empty()&&context->CurrentTable==nullptr,
            "Frequency summary balances window, popup and table scopes");
        const auto* draw=ImGui::GetDrawData();
        require(draw&&draw->Valid&&draw->TotalVtxCount>0,"Frequency summary generates a valid rendered frame");
        for(int list=0;list<draw->CmdListsCount;++list)for(const auto& vertex:draw->CmdLists[list]->VtxBuffer)
            require(std::isfinite(vertex.pos.x)&&std::isfinite(vertex.pos.y),"Summary geometry remains finite");
        return text;
    }
};
void frequency_summary_rendering() {
    SummaryUi ui;Snapshot snapshot;
    ui.frame(snapshot);
    const auto empty=ui.frame(snapshot,{1100,650},1,true);
    require(empty.find("Frequency measurements appear here after reception starts.")!=std::string::npos,
        "Empty live summary explains that measurements have not started");
    snapshot.measurement_seconds=10;snapshot.spectrum_events=3;
    snapshot.frequencies={{906875000,250000,-72,-32,10,2.5},{908750000,500000,-180,-180,0,0}};
    ui.frame(snapshot);
    const auto populated=ui.frame(snapshot,{1100,650},1,true);
    require(populated.find("25.00")!=std::string::npos&&populated.find("N/A")!=std::string::npos,
        "Observed busy percentage and unobserved N/A are distinct in rendered table text");
    snapshot.frequencies.erase(snapshot.frequencies.begin());snapshot.historical=true;
    const auto unobserved=ui.frame(snapshot,{1100,650},1,true);
    require(unobserved.find("RECORDED FREQUENCY SUMMARY")!=std::string::npos&&
        unobserved.find("N/A")!=std::string::npos&&unobserved.find("-180.0")==std::string::npos,
        "Unobserved saved bins do not render sentinel levels as measured quiet power");
    snapshot.frequencies.clear();snapshot.historical=false;
    for(uint64_t i=0;i<4096;++i)snapshot.frequencies.push_back({905000000+i*2500,2500,-75,-35,
        i%3?10.0:0.0,i%3?static_cast<double>(i%10):0.0});
    ui.frame(snapshot);ui.frame(snapshot);
    ui.frame(snapshot,{320,420},1.6f);ui.frame(snapshot,{320,420},1.6f);
    require(snapshot.frequencies.size()==4096,"Rendering does not change the full frequency-bin data");
}
}
int main() {
    try {
        Fixture fixture;defaults_folder_and_optouts(fixture);receiver_offset_survives_restart(fixture);consecutive_recordings(fixture);
        unavailable_settings_or_folder(fixture);frequency_summary_rendering();
        std::cout<<"Desktop setup and recording integration passed; explicit fixtures only, no windows or USB opened\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"Desktop setup integration failed: "<<error.what()<<'\n';return 1;}
}
