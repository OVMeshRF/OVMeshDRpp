// SPDX-License-Identifier: GPL-3.0-or-later
// Desktop composition. Included inside ui.cpp's private namespace after the
// shared charts and dialogs; it has no receiver or rendering dependencies of its own.

class NumericFont {
public:
    explicit NumericFont(const DesktopState& ui, float size = 0) : active_(ui.mono_font != nullptr) {
        if (active_) ImGui::PushFont(ui.mono_font, size);
    }
    ~NumericFont() { if (active_) ImGui::PopFont(); }
private:
    bool active_;
};

bool has_session_data(const Snapshot& snapshot) {
    return snapshot.input_seconds > 0 || snapshot.total_receptions > 0 ||
        !snapshot.frequencies.empty() || snapshot.historical;
}

void request_new_session(Engine& engine, DesktopState& ui, bool discard = false) {
    ui.begin_operation("Finishing current session...", [&engine, discard] {
        std::string error;
        const bool ok = engine.new_session(error, discard);
        return DesktopState::OperationResult{ok, ok ? "New session ready. Previous recordings are preserved." : error};
    }, [&engine, &ui] {
        ui.clear_views(); ui.config.session_path.clear(); ui.recording_path_used = true;
        ui.prepare_recording_file(); ui.new_requested = false;
        if (ui.preferences_active && ui.gps_enabled && ui.source == 1 && !ui.passive_smoke && !ui.prepared_run)
            ui.connect_selected_gps(engine);
        if (ui.restart_after_new) { ui.restart_after_new = false; ui.start(engine, ui.source == 1); }
    });
}

void request_save_session(Engine& engine, DesktopState& ui) {
    ui.begin_operation("Saving session checkpoint...", [&engine] {
        std::string error;
        const bool ok = engine.save_session(error);
        return DesktopState::OperationResult{ok, ok ? "Session checkpoint saved to disk." : error};
    });
}

void request_open_session(Engine& engine, DesktopState& ui, const std::string& path) {
    ui.begin_operation("Opening saved session...", [&engine, path] {
        std::string error;
        const auto current = engine.snapshot();
        if (!current.historical && has_session_data(current) && !current.config.session_path.empty() &&
            !engine.save_session(error)) return DesktopState::OperationResult{false, error};
        const bool ok = engine.open_session(path, error);
        return DesktopState::OperationResult{ok, ok ? "Saved session opened read-only." : error};
    }, [&engine, &ui] {
        engine.disconnect_gps(); ui.clear_views(); ui.focus_analysis = true; ui.focus_live = false;
        ui.show_export = false; ui.last_analysis_refresh = -1;
    });
}

void open_session_chooser(Engine& engine, DesktopState& ui, const Snapshot& snapshot) {
    begin_file_picker(ui, FilePickerPurpose::OpenSurvey, ui.reopen_path, snapshot.config.session_path);
    ui.file_chosen = [&engine, &ui](const std::string& path) {
        ui.reopen_path = path;
        const auto current = engine.snapshot();
        if (has_session_data(current) && current.config.session_path.empty()) ui.pending_open_path = path;
        else request_open_session(engine, ui, path);
    };
}

void session_toolbar(Engine& engine, DesktopState& ui, const Snapshot& snapshot) {
    if (ImGui::BeginTable("sessionToolbar", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Session", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 468 * ui.ui_scale);
        ImGui::TableNextRow(); ImGui::TableNextColumn();
        ImGui::TextColored(accent, "OVMeshDR++"); ImGui::SameLine(0, 18);
        const auto& title = snapshot.session_id.empty() ? std::string(ui.session_title.data()) : snapshot.config.session_title;
        ImGui::TextUnformatted(title.c_str());
        if (snapshot.historical) ImGui::TextColored(secondary, "Saved session / Read only");
        else if (snapshot.session_id.empty()) ImGui::TextDisabled("New survey / No previous results loaded");
        else ImGui::TextDisabled(snapshot.running ? "Current survey" : "Stopped / Results retained");
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(ui.operation_busy() || ui.managed_started || ui.passive_smoke);
        if (ImGui::Button("New session")) {
            if (has_session_data(snapshot) && snapshot.config.session_path.empty()) ui.new_requested = true;
            else request_new_session(engine, ui);
        }
        ImGui::SameLine(); ImGui::BeginDisabled(snapshot.running);
        if (ImGui::Button("Open...")) open_session_chooser(engine, ui, snapshot);
        ImGui::EndDisabled(); ImGui::SameLine();
        ImGui::BeginDisabled(snapshot.session_id.empty() || snapshot.historical || snapshot.config.session_path.empty());
        if (ImGui::Button("Save session")) request_save_session(engine, ui);
        ImGui::EndDisabled(); ImGui::SameLine();
        if (ImGui::Button("...##sessionActions")) ImGui::OpenPopup("Session actions");
        if (ImGui::BeginPopup("Session actions")) {
            ImGui::BeginDisabled(snapshot.config.session_path.empty() || snapshot.session_id.empty());
            if (ImGui::MenuItem("Save a copy...")) {
                begin_file_picker(ui, FilePickerPurpose::SaveCopy, ui.copy_path, snapshot.config.session_path);
                ui.file_chosen = [&engine, &ui](const std::string& path) {
                    ui.copy_path = path;
                    ui.begin_operation("Saving session copy...", [&engine, path] {
                        std::string error; const bool ok = engine.save_session_copy(path, error);
                        return DesktopState::OperationResult{ok, ok ? "Session copy saved. Current recording is unchanged." : error};
                    });
                };
            }
            ImGui::EndDisabled(); ImGui::EndPopup();
        }
        ImGui::EndDisabled(); ImGui::SameLine();
        if (ImGui::Button("Settings")) ui.show_settings = true;
        ImGui::EndTable();
    }
}

void receiver_controls(Engine& engine, DesktopState& ui, const Snapshot& snapshot) {
    label(snapshot.historical ? "Recorded receiver" : has_session_data(snapshot) && !snapshot.running ? "Receiver / next session" : "Receiver");
    const bool locked = snapshot.running || snapshot.historical || ui.operation_busy();
    auto& cfg = ui.config;
    const auto& displayed = snapshot.historical ? snapshot.config : cfg;
    bool changed = false;
    ImGui::BeginDisabled(locked);
    int source = snapshot.historical ? (snapshot.config.synthetic ? 0 : 1) : ui.source;
    ImGui::SetNextItemWidth(-1); ImGui::BeginDisabled(ui.prepared_run);
    if (ImGui::Combo("##source", &source, "Synthetic / demo\0HackRF One / USB\0")) {
        ui.source = source; changed = true;
        if (source == 0) engine.disconnect_gps();
        else if (ui.preferences_active && ui.gps_enabled && !ui.passive_smoke) ui.connect_selected_gps(engine);
    }
    ImGui::EndDisabled();
    ImGui::Spacing(); ImGui::TextDisabled("Center frequency / MHz");
    double center = displayed.center_hz / 1e6;
    {
        NumericFont font(ui, ui.mono_font ? 21 : 0);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputDouble("##center", &center, 0, 0, "%.6f") && std::isfinite(center)) {
            cfg.center_hz = static_cast<uint64_t>(std::clamp(center, 1.0, 6000.0) * 1e6); changed = true;
        }
    }
    ImGui::Spacing(); ImGui::TextDisabled("Offset / Hz");
    int offset = static_cast<int>(displayed.tuning_offset_hz);
    ImGui::SetNextItemWidth(-1);
    { NumericFont font(ui); if (ImGui::InputInt("##tuningOffset", &offset, 0, 0)) { cfg.tuning_offset_hz = std::clamp(offset, -100000, 100000); changed = true; } }
    ImGui::Spacing(); ImGui::TextDisabled("Survey span / MHz");
    double span = displayed.survey_span_hz / 1e6;
    ImGui::SetNextItemWidth(-1);
    { NumericFont font(ui); if (ImGui::InputDouble("##span", &span, 0, 0, "%.2f") && std::isfinite(span)) {
        cfg.survey_span_hz = static_cast<uint32_t>(std::clamp(span, .5, std::min(16.0, cfg.sample_rate * .8e-6)) * 1e6); changed = true;
    } }
    ImGui::Spacing(); ImGui::TextDisabled("Sample rate");
    constexpr std::array<uint32_t, 5> rates{8000000,10000000,12000000,16000000,20000000};
    int rate = 3;
    for (size_t i=0; i<rates.size(); ++i) if (displayed.sample_rate == rates[i]) rate = static_cast<int>(i);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##sample", &rate, "8 MS/s\0 10 MS/s\0 12 MS/s\0 16 MS/s\0 20 MS/s\0")) {
        cfg.sample_rate = rates[static_cast<size_t>(rate)];
        cfg.survey_span_hz = std::min(cfg.survey_span_hz, static_cast<uint32_t>(cfg.sample_rate * .8)); changed = true;
    }
    ImGui::Spacing(); ImGui::TextDisabled("Survey range");
    { NumericFont font(ui); ImGui::Text("%.3f - %.3f MHz", (double(displayed.center_hz)-displayed.survey_span_hz*.5)/1e6,
        (double(displayed.center_hz)+displayed.survey_span_hz*.5)/1e6); }
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    int lna = static_cast<int>(displayed.lna_gain), vga = static_cast<int>(displayed.vga_gain);
    ImGui::TextDisabled("LNA gain"); ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##lnaGain", &lna, 0, 40, "%d dB")) { cfg.lna_gain = static_cast<unsigned>((lna+4)/8*8); changed = true; }
    ImGui::Spacing(); ImGui::TextDisabled("VGA gain"); ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##vgaGain", &vga, 0, 62, "%d dB")) { cfg.vga_gain = static_cast<unsigned>((vga+1)/2*2); changed = true; }
    bool amp = displayed.amplifier;
    ImGui::Spacing(); if (ImGui::Checkbox("RF amplifier", &amp)) { cfg.amplifier = amp; changed = true; }
    ImGui::EndDisabled();
    if (changed) ui.persist_preferences();
    ImGui::Spacing(); ImGui::Spacing();
    ImGui::BeginDisabled(ui.operation_busy() || ui.passive_smoke || (!snapshot.running && ui.managed_started));
    if (snapshot.running) {
        ImGui::PushStyleColor(ImGuiCol_Button, {0.26f,.13f,.17f,1});
        ImGui::PushStyleColor(ImGuiCol_Text, {1,.73f,.76f,1});
        if (ImGui::Button("Stop reception", {-1,40})) {
            ui.begin_operation("Stopping reception...", [&engine] {
                engine.stop(); const auto final = engine.snapshot();
                return DesktopState::OperationResult{final.error.empty(), final.error.empty() ? "Reception stopped. Results remain open." : final.error};
            }, [&ui] { ui.prepare_recording_file(); });
        }
        ImGui::PopStyleColor(2);
    } else {
        const bool unavailable = ui.source == 1 && !snapshot.hardware_available;
        ImGui::BeginDisabled(unavailable || (ui.save_session && ui.session_path.empty()));
        ImGui::PushStyleColor(ImGuiCol_Button, accent); ImGui::PushStyleColor(ImGuiCol_Text, {0.03f,.10f,.11f,1});
        const char* action = has_session_data(snapshot) ? "Start new session" : ui.source == 0 ? "Start demo" : "Start reception";
        if (ImGui::Button(action, {-1,40})) {
            if (has_session_data(snapshot) && snapshot.config.session_path.empty()) { ui.new_requested = true; ui.restart_after_new = true; }
            else if (has_session_data(snapshot)) { ui.restart_after_new = true; request_new_session(engine, ui); }
            else ui.start(engine, ui.source == 1);
        }
        ImGui::PopStyleColor(2); ImGui::EndDisabled();
        if (unavailable) wrapped("HackRF support is unavailable in this build.", amber);
        if (ui.save_session && ui.session_path.empty()) {
            wrapped("Choose a recording folder to start.", amber);
            if (ImGui::SmallButton("Recording settings")) { ui.settings_page = 2; ui.show_settings = true; }
        }
    }
    ImGui::EndDisabled(); ImGui::Spacing();
    wrapped(ui.source == 0 ? "Synthetic source / no radio" : "Receive only / local to this computer");
}

void detection_settings(Engine& engine, DesktopState& ui, const Snapshot& snapshot) {
    label("Detection & decoding");
    wrapped("Choose what the survey looks for across the supplied range.");
    ImGui::BeginDisabled(snapshot.running || ui.operation_busy());
    int mode = ui.spectrum_only ? 1 : 0;
    if (ImGui::RadioButton("Spectrum + LoRa", &mode, 0)) { ui.spectrum_only = false; ui.persist_preferences(); }
    if (ImGui::RadioButton("Spectrum only", &mode, 1)) { ui.spectrum_only = true; ui.persist_preferences(); }
    ImGui::Spacing(); ImGui::BeginDisabled(ui.spectrum_only);
    if (ImGui::Checkbox("Discover LoRa waveforms", &ui.config.discover_lora)) ui.persist_preferences();
    wrapped("Estimate center frequency, modem bandwidth and spreading factor.");
    if (ImGui::Checkbox("Decode authorized messages", &ui.decode_enabled)) ui.persist_preferences();
    wrapped("Use only configured keys and supported receive profiles.");
    ImGui::EndDisabled(); ImGui::EndDisabled();
    ImGui::Spacing(); ImGui::Separator();
    ImGui::Text("Configured key records: %zu", ui.key_count(engine));
    if (ImGui::Button("Configure keys...")) { ui.show_keys = true; ui.authorize_keys = false; erase_secret(ui.key_input); }
    if (ui.config.lanes.empty() && !ui.spectrum_only) {
        wrapped("No receive profiles configured. Waveform discovery can still run.", amber);
        ImGui::BeginDisabled(snapshot.running);
        if (ImGui::Button("Add receive profile")) ui.config.lanes.push_back(LaneConfig{});
        ImGui::EndDisabled();
    }
    if (ImGui::CollapsingHeader("Advanced / Legacy decode profiles")) legacy_profile_settings(engine, ui, snapshot);
    const double lower = double(ui.config.center_hz) - ui.config.survey_span_hz * .5;
    const double upper = double(ui.config.center_hz) + ui.config.survey_span_hz * .5;
    const auto outside = std::count_if(ui.config.lanes.begin(), ui.config.lanes.end(), [&](const LaneConfig& lane) {
        return double(lane.frequency_hz) - lane.bandwidth_hz * .5 < lower || double(lane.frequency_hz) + lane.bandwidth_hz * .5 > upper;
    });
    if (outside) ImGui::TextWrapped("%zu legacy profile(s) are outside this survey range and will be skipped. Their settings are preserved.", static_cast<size_t>(outside));
    ImGui::Spacing();
    wrapped("Waveform discovery does not identify a mesh network. Automatic decoding of discovered signals is still in development.", secondary);
    if (ImGui::CollapsingHeader("Supported decoding and measurement limits")) {
        wrapped("Discovery searches supported 125/250/500 kHz, SF7-12 waveforms throughout the supplied range. A match is an estimate, not a unique packet or authenticated sender. Detection is experimental and may miss signals.");
        wrapped("Payload decoding currently uses explicit Meshtastic profiles. MeshCore and recipient private-key decoding are not enabled. Spectrum only suppresses both discovery and payload decoding without erasing their settings.");
    }
}

void settings_window(Engine& engine, DesktopState& ui, const Snapshot& snapshot) {
    if (!ui.show_settings) return;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize({std::min(900.f*ui.ui_scale, viewport->WorkSize.x-30), std::min(730.f*ui.ui_scale,viewport->WorkSize.y-30)}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, {.5f,.5f});
    if (ImGui::Begin("Settings", &ui.show_settings, ImGuiWindowFlags_NoCollapse)) {
        const char* categories[] = {"Detection & decoding","GPS & location","Recording","Display","Measurement & equipment","About & licenses"};
        const float footer = ImGui::GetFrameHeightWithSpacing() + 8;
        ImGui::BeginChild("settingsNavigation", {210*ui.ui_scale,-footer}, false);
        for (int i=0;i<6;++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(categories[i], ui.settings_page==i,0,{0,39*ui.ui_scale})) ui.settings_page=i;
            ImGui::PopID();
        }
        ImGui::EndChild(); ImGui::SameLine();
        ImGui::BeginChild("settingsPage", {0,-footer}, false);
        ImGui::BeginDisabled(ui.operation_busy());
        if (ui.settings_page == 0) detection_settings(engine,ui,snapshot);
        else if (ui.settings_page == 1) {
            ImGui::BeginDisabled(ui.passive_smoke || snapshot.historical || snapshot.running);
            gps_settings(engine,ui,snapshot);
            ImGui::EndDisabled();
            if (snapshot.running) wrapped("Stop reception to change the GPS or fixed survey location.");
            if (snapshot.historical) wrapped("Saved positions are read-only. New session returns to receiver setup.");
        } else if (ui.settings_page == 2) recording_settings(ui,snapshot);
        else if (ui.settings_page == 3) {
            label("Display");
            int fonts = ui.mixed_fonts ? 0 : 1;
            if (ImGui::Combo("Typography", &fonts, "System UI + numeric monospace\0All monospace\0")) { ui.mixed_fonts=fonts==0; ui.persist_preferences(); }
            wrapped("Uses locally installed system fonts, with a built-in fallback.");
            ImGui::SliderFloat("UI scale", &ui.ui_scale,.85f,1.5f,"%.2f");
            ImGui::SliderFloat("Display floor", &ui.display_floor,-130,-30,"%.0f dBFS");
            ImGui::SliderFloat("Display ceiling", &ui.display_ceiling,-60,0,"%.0f dBFS");
            if (ui.display_ceiling <= ui.display_floor+5) ui.display_ceiling=ui.display_floor+5;
            wrapped("Display settings do not change stored measurements.");
        } else if (ui.settings_page == 4) {
            label("Measurement & equipment"); ImGui::BeginDisabled(snapshot.running);
            ImGui::SliderFloat("Activity threshold", &ui.config.activity_threshold_dbfs,-110,-10,"%.0f dBFS");
            ImGui::InputText("Optional receiver serial",ui.device_serial.data(),ui.device_serial.size());
            ImGui::InputText("Antenna description",ui.antenna_description.data(),ui.antenna_description.size());
            ImGui::InputText("Receiver description",ui.receiver_description.data(),ui.receiver_description.size());
            ImGui::InputTextMultiline("Survey notes",ui.survey_notes.data(),ui.survey_notes.size(),{-1,100});
            ImGui::EndDisabled();
            wrapped("Power is relative dBFS. No calibration is implied. Descriptions and notes are recorded with the survey.");
            if (ImGui::Button("Measurement and receiver details")) ui.show_diagnostics=true;
            if (ImGui::CollapsingHeader("Offset convention")) wrapped("Offset is in hertz. Positive values raise the hardware tuning command; the spectrum axis stays nominal. Verify this receiver-specific setting when changing equipment.");
        } else {
            ImGui::Text("OVMeshDR++ %s",Engine::version().c_str());
            wrapped("Local RF survey / receive only / open source");
            if (ImGui::Button("Open-source licenses & notices")) ui.show_licenses=true;
            if (ImGui::CollapsingHeader("Help / session workflow",ImGuiTreeNodeFlags_DefaultOpen)) {
                wrapped("New clears the workspace after preserving the prior recording. Open views a saved session read-only. Save writes a checkpoint; Save a copy creates a separate database. Export creates a report.");
                wrapped("GPS connects automatically on ordinary startup when enabled and uniquely recognized. Keys stay in this process and must be configured for authorized decoding.");
            }
        }
        ImGui::EndDisabled(); ImGui::EndChild();
        ImGui::Separator();
        ImGui::TextDisabled(snapshot.running ? "Stop reception before changing acquisition settings." : "Receiver and recording settings apply to the next session.");
        ImGui::SameLine(); if (ImGui::Button("Done")) ui.show_settings=false;
    }
    ImGui::End();
}

void compact_signal_table(DesktopState& ui, const Snapshot& snapshot, float height) {
    const double now = ImGui::GetTime();
    if (ui.waveform_session != snapshot.session_id || ui.waveform_refresh < 0 || now-ui.waveform_refresh >= 1 || !snapshot.running) {
        ui.waveform_rows = snapshot.waveforms; ui.waveform_session=snapshot.session_id; ui.waveform_refresh=now;
        std::sort(ui.waveform_rows.begin(),ui.waveform_rows.end(),[](const auto& a,const auto& b){return a.delimiter_elapsed>b.delimiter_elapsed;});
    }
    if (ui.waveform_rows.empty()) {
        wrapped(has_session_data(snapshot) ? "No LoRa waveform observations yet. Other RF activity remains visible in Analyze." : "Signals will appear after reception starts.");
        return;
    }
    NumericFont font(ui);
    if (ImGui::BeginTable("detectedSignals",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY|ImGuiTableFlags_Resizable,{0,std::max(90.f,height)})) {
        ImGui::TableSetupColumn("Observed at (UTC)", ImGuiTableColumnFlags_WidthFixed,
            ImGui::CalcTextSize("0000-00-00 00:00:00.000").x + 12);
        for (const char* h : {"Center MHz","Inferred BW kHz","SF","Evidence"}) ImGui::TableSetupColumn(h);
        ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();
        ImGuiListClipper clip; clip.Begin(static_cast<int>(ui.waveform_rows.size()));
        while(clip.Step())for(int i=clip.DisplayStart;i<clip.DisplayEnd;++i){
            const auto& o=ui.waveform_rows[static_cast<size_t>(i)];
            ImGui::TableNextRow(); ImGui::TableNextColumn();ImGui::TextUnformatted(timestamp_text(o.delimiter_utc).c_str());
            reception_time_tooltip(o.delimiter_utc, o.delimiter_elapsed, true);
            ImGui::TableNextColumn();ImGui::Text("%.6f",o.center_hz/1e6);
            ImGui::TableNextColumn();ImGui::Text("%u",o.bandwidth_hz/1000);
            ImGui::TableNextColumn();ImGui::Text("%u",o.spreading_factor);
            ImGui::TableNextColumn();ImGui::TextUnformatted(o.association_ambiguous?"Ambiguous association":o.complete_in_requested_range?"LoRa waveform":"Range edge / partial");
        }
        ImGui::EndTable();
    }
}

void compact_analysis_tab(Engine& engine, DesktopState& ui, const Snapshot& snapshot) {
    ImGui::BeginChild("analysisWorkspace",{0,0},false);
    const bool saved=!snapshot.session_id.empty()&&!snapshot.config.session_path.empty();
    if (ui.analysis_session != snapshot.session_id) { ui.analysis_loaded=false;ui.selected_observation.reset(); }
    if (!has_session_data(snapshot)) {
        wrapped("No measurements yet. Start a survey or open a saved session.");
        if(ImGui::Button("Open saved session..."))open_session_chooser(engine,ui,snapshot);
        ImGui::EndChild();return;
    }
    if (saved && !ui.analysis_busy() && !ui.operation_busy() && (ui.last_analysis_refresh<0 ||
        (!snapshot.running && ui.last_analysis_requested_while_running) || (snapshot.running && ImGui::GetTime()-ui.last_analysis_refresh>=5))) {
        queue_analysis(ui,snapshot,false);ui.last_analysis_refresh=ImGui::GetTime();
    }
    ImGui::TextUnformatted("Frequency activity"); ImGui::SameLine();
    ImGui::BeginDisabled(!saved || ui.operation_busy());
    if(ImGui::Button("Generate analysis report...")) {prepare_report_export(ui,snapshot);ui.export_kind=7;}
    ImGui::SameLine();
    if(ImGui::Button("Export report..."))prepare_report_export(ui,snapshot);
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::BeginDisabled(!saved || ui.analysis_busy());
    if(ImGui::BeginTable("rangeSelection",3,ImGuiTableFlags_SizingStretchSame)){
        ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextDisabled("From / MHz");
        ImGui::SetNextItemWidth(-1);ImGui::InputDouble("##analysisLowerMHz",&ui.query_lower_mhz,0,0,"%.6f");
        ImGui::TableNextColumn();ImGui::TextDisabled("To / MHz");
        ImGui::SetNextItemWidth(-1);ImGui::InputDouble("##analysisUpperMHz",&ui.query_upper_mhz,0,0,"%.6f");
        ImGui::TableNextColumn();ImGui::TextDisabled("Click / selection width");ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##selectionWidth",&ui.width_choice,"62.5 kHz\0 125 kHz\0 250 kHz\0 500 kHz\0 Custom\0");
        ImGui::EndTable();
    }
    if(ui.width_choice<4){constexpr double widths[]={62.5,125,250,500};ui.query_width_khz=widths[ui.width_choice];}
    else {ImGui::SetNextItemWidth(130);ImGui::InputDouble("Custom width / kHz",&ui.query_width_khz,0,0,"%.3f");}
    if(ImGui::Button("Apply range"))queue_analysis(ui,snapshot,false);
    ImGui::SameLine();if(ImGui::Button("Full range"))select_analysis_frequencies(ui,snapshot,{});
    ImGui::SameLine();if(ImGui::Button("Width from lower edge")){
        if(ui.query_lower_mhz==0)ui.query_lower_mhz=(double(snapshot.config.center_hz)-snapshot.config.survey_span_hz*.5)/1e6;
        ui.query_upper_mhz=ui.query_lower_mhz+ui.query_width_khz/1000;queue_analysis(ui,snapshot,false);
    }
    ImGui::EndDisabled();
    if(ui.analysis_busy())ImGui::TextColored(secondary,"Updating saved measurements...");
    if(ui.analysis_loaded){
        const auto& a=ui.analysis;
        wrapped(analysis_frequency_text(a).c_str(),secondary);
        ImGui::TextDisabled("Elapsed %.1f - %.1f s / saved measurements",a.resolved_elapsed_start,a.resolved_elapsed_end);
        if(a.observed_seconds>0){
            const bool guarded=guarded_view(ui);const double busy=guarded?a.outside_center_busy_seconds:a.busy_seconds;
            const bool measured=!guarded||a.outside_center_bin_count>0;
            if(ImGui::BeginTable("selectionMetrics",3,ImGuiTableFlags_SizingStretchSame)){
                ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextDisabled("Busy time / selected range");
                if(measured)ImGui::TextColored(accent,"%.3f%%",100*ratio(busy,a.observed_seconds));else ImGui::TextUnformatted("Unavailable");
                ImGui::TableNextColumn();ImGui::TextDisabled("Above threshold / observed");
                if(measured)ImGui::Text("%.3f / %.3f s",busy,a.observed_seconds);else ImGui::TextUnformatted("No unguarded bins");
                ImGui::TableNextColumn();ImGui::TextDisabled("Observed / selected elapsed");
                const double elapsed=a.resolved_elapsed_end-a.resolved_elapsed_start;
                ImGui::Text("%.2f%%",100*ratio(a.observed_seconds,elapsed));ImGui::EndTable();
            }
        }else wrapped("No observed time in this selection; occupancy is unavailable.",amber);
        if(a.center_guard_bin_count){ImGui::Checkbox("Exclude receiver-center artifact region",&ui.use_center_guard);ImGui::SameLine();ImGui::TextDisabled("Excluded frequencies remain unassessed");}
    }else if(!saved)wrapped("Live frequency measurements / not recording. Saved history is required for time and location views.",amber);
    ImGui::Spacing();
    const char* views[]={"By frequency","Over time","By location"};
    for(int i=0;i<3;++i){if(i)ImGui::SameLine();ImGui::BeginDisabled(i>0&&!ui.analysis_loaded);
        const bool selected=ui.analysis_view==i;
        if(selected)ImGui::PushStyleColor(ImGuiCol_Button,{.12f,.23f,.34f,1});
        if(ImGui::Button(views[i]))ui.analysis_view=i;
        if(selected)ImGui::PopStyleColor();
        ImGui::EndDisabled();
    }
    ImGui::Spacing();
    if(ui.analysis_view==0){
        ImGui::TextDisabled("Full-session frequency overview / all recorded times and positions");
        ui.summary_selection.click_width_hz=ui.query_width_khz*1000;
        ui.summary_selection.highlighted=ui.analysis_loaded&&!ui.analysis.bins.empty()?std::optional<FrequencyRange>{{ui.analysis.covered_lower_hz,ui.analysis.covered_upper_hz}}:std::nullopt;
        {NumericFont font(ui);occupancy_chart(snapshot.frequencies,std::max(180.f,ImGui::GetContentRegionAvail().y*.52f),ui.occupancy_scale,0,0,saved&&!ui.analysis_busy()?&ui.summary_selection:nullptr);}
        if(ui.summary_selection.requested){select_analysis_frequencies(ui,snapshot,*ui.summary_selection.requested);ui.summary_selection.requested.reset();}
        wrapped("Busy = observed time above threshold. Drag the chart to select frequencies; click to use the chosen width.");
        bool low=ui.occupancy_scale==OccupancyScale::RevealLowActivity;
        if(ImGui::Checkbox("Reveal low activity (log scale)",&low))ui.occupancy_scale=low?OccupancyScale::RevealLowActivity:OccupancyScale::Linear;
        if(ImGui::CollapsingHeader("Frequency measurements"))frequency_summary(snapshot,ui.occupancy_scale);
    }else if(ui.analysis_view==1&&ui.analysis_loaded){
        {NumericFont font(ui);busy_time_chart(ui,std::max(170.f,ImGui::GetContentRegionAvail().y*.52f));}
        if(ImGui::BeginTable("timeSummary",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY,{0,160})){
            for(const char* h:{"Elapsed interval / s","Observed s","Busy s","Busy %","Receiver position"})ImGui::TableSetupColumn(h);
            ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();
            ImGuiListClipper clip;clip.Begin(static_cast<int>(ui.analysis.observations.size()));
            while(clip.Step())for(int i=clip.DisplayStart;i<clip.DisplayEnd;++i){const auto& o=ui.analysis.observations[static_cast<size_t>(i)];
                const auto busy=displayed_busy_ratio(ui,o);ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::Text("%.1f - %.1f",o.elapsed_start,o.elapsed_end);
                ImGui::TableNextColumn();ImGui::Text("%.3f",o.observed_seconds);ImGui::TableNextColumn();if(busy)ImGui::Text("%.3f",*busy*o.observed_seconds);else ImGui::TextUnformatted("--");
                ImGui::TableNextColumn();if(busy)ImGui::Text("%.3f",*busy*100);else ImGui::TextUnformatted("--");
                ImGui::TableNextColumn();ImGui::TextUnformatted(o.receiver_position?"Available":"Missing");
            }ImGui::EndTable();
        }
    }else if(ui.analysis_loaded)geographic_rf_view(ui,std::max(200.f,ImGui::GetContentRegionAvail().y-130));
    if(ImGui::CollapsingHeader("Time interval and geographic filters")){
        ImGui::BeginDisabled(!saved||ui.analysis_busy());
        ImGui::InputDouble("Start elapsed / s",&ui.survey_query.elapsed_start,0,0,"%.3f");
        ImGui::InputDouble("End elapsed / s (0 = latest)",&ui.survey_query.elapsed_end,0,0,"%.3f");
        ImGui::InputDouble("Time detail / s",&ui.survey_query.time_bucket_seconds,0,0,"%.3f");
        ImGui::Checkbox("Filter receiver area",&ui.survey_query.geographic_filter);
        if(ui.survey_query.geographic_filter){ImGui::InputDouble("South",&ui.survey_query.south);ImGui::InputDouble("North",&ui.survey_query.north);ImGui::InputDouble("West",&ui.survey_query.west);ImGui::InputDouble("East",&ui.survey_query.east);}
        if(ImGui::Button("Apply time and area"))queue_analysis(ui,snapshot,false);
        ImGui::EndDisabled();
    }
    if(ImGui::Button("Measurement details..."))ui.show_analysis_details=true;
    ImGui::EndChild();
}

void status_strip(Engine& engine,DesktopState& ui,const Snapshot& snapshot){
    ImGui::TextColored(snapshot.running?accent:muted,"%s",snapshot.historical?"Read only":snapshot.running?"Receiving":has_session_data(snapshot)?"Stopped":"Ready");
    ImGui::SameLine(0,18);{NumericFont font(ui);ImGui::TextUnformatted(duration_text(snapshot.elapsed_seconds).c_str());}
    ImGui::SameLine(0,18);
    ImGui::TextColored(snapshot.running&&!snapshot.recording?amber:muted,"%s",snapshot.historical?"Saved recording":snapshot.recording?"Recording to disk":snapshot.running?"Not recording":ui.save_session?"Auto-save on":"Memory only");
    ImGui::SameLine(0,18);
    const auto gps=engine.gps_connection_status();
    const char* gps_label=snapshot.historical?"Recorded GPS":gps.state==GpsConnectionState::ValidFix?"GPS fix":gps.state==GpsConnectionState::WaitingForFix?"GPS acquiring":gps.state==GpsConnectionState::StaleFix?"GPS stale":gps.state==GpsConnectionState::ReadError?"GPS error":ui.gps_enabled?"GPS unavailable":"GPS off";
    if(ImGui::SmallButton(gps_label)){ui.settings_page=1;ui.show_settings=true;}
    ImGui::SameLine(0,18);
    if(ImGui::SmallButton("Details"))ui.show_diagnostics=true;
    if(snapshot.discovery.failed)wrapped("LoRa discovery stopped. Spectrum measurements have separate coverage; see Details.",red);
    else if(snapshot.discovery.rejected_input_samples||snapshot.discovery.abandoned_input_samples||snapshot.discovery.result_overflows)
        wrapped("LoRa discovery has coverage loss. See Details; detected signals may be incomplete.",amber);
    if(snapshot.dropped_samples)wrapped("RF sample loss recorded. Check receiver details before comparing occupancy.",amber);
    if(snapshot.clipped_samples)wrapped("Receiver clipping detected. Stop and reduce gain.",amber);
}

void session_disk_footer(DesktopState& ui,const Snapshot& snapshot) {
    const auto path=snapshot.session_id.empty()?std::string():snapshot.config.session_path;
    ui.session_disk.poll(path,ImGui::GetTime());
    std::string text="Session disk: ";
    if(snapshot.session_id.empty())text+="No session";
    else if(path.empty())text+="Memory only";
    else if(!ui.session_disk.value.available)text+=ui.session_disk.pending.valid()?"Checking...":"Unavailable";
    else {
        const auto& size=ui.session_disk.value;
        double bytes=double(size.database_bytes+size.journal_bytes);unsigned unit=0;
        const char* units[]={"B","KiB","MiB","GiB","TiB"};
        while(bytes>=1024&&unit<4){bytes/=1024;++unit;}
        char amount[64]{};std::snprintf(amount,sizeof(amount),"%.1f %s",bytes,units[unit]);text+=amount;
    }
    const float width=ImGui::CalcTextSize(text.c_str()).x;
    const float right=ImGui::GetWindowContentRegionMax().x-width;
    const float used=ImGui::GetItemRectMax().x-ImGui::GetWindowPos().x;
    if(right>used+20)ImGui::SameLine(right);
    ImGui::TextDisabled("%s",text.c_str());
    if(ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Approximate current file sizes, refreshed every five seconds in the background.");
        ImGui::TextUnformatted("Includes this database plus SQLite WAL, shared-memory and rollback-journal files.");
        ImGui::TextUnformatted("Excludes exports, screenshots, other sessions and filesystem allocation overhead.");
        if(ui.session_disk.value.available)ImGui::Text("Database: %llu bytes | SQLite sidecars: %llu bytes",
            static_cast<unsigned long long>(ui.session_disk.value.database_bytes),static_cast<unsigned long long>(ui.session_disk.value.journal_bytes));
        ImGui::EndTooltip();
    }
}

void render(Engine& engine, DesktopState& ui, const Snapshot& snapshot) {
    auto& io=ImGui::GetIO();io.FontGlobalScale=ui.ui_scale;
    ImFont* font=ui.mixed_fonts?ui.sans_font:ui.mono_font;
    if(font)ImGui::PushFont(font,14);
    const auto* viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("OVMeshDR++",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBringToFrontOnFocus);
    session_toolbar(engine,ui,snapshot);ImGui::Separator();
    if(ui.timed_run){const double remaining=std::chrono::duration<double>(ui.deadline-std::chrono::steady_clock::now()).count();ImGui::TextColored(amber,"Test auto-close in %s",duration_text(std::ceil(std::max(0.,remaining))).c_str());}
    if(!ui.preferences_error.empty())wrapped(ui.preferences_error.c_str(),red);
    const float footer=ui.notice.empty()&&snapshot.error.empty()&&!ui.operation_busy()?35.f:62.f;
    const float content=std::max(250.f,ImGui::GetContentRegionAvail().y-footer*ui.ui_scale);
    ImGui::BeginChild("receiverSidebar",{225*ui.ui_scale,content},false);
    receiver_controls(engine,ui,snapshot);ImGui::EndChild();ImGui::SameLine();
    ImGui::BeginChild("workspace",{0,content},false);
    status_strip(engine,ui,snapshot);ImGui::Spacing();
    if(ImGui::BeginTabBar("workspaces")){
        if(ImGui::BeginTabItem("Live survey",nullptr,ui.focus_live?ImGuiTabItemFlags_SetSelected:ImGuiTabItemFlags_None)){
            ui.focus_live=false;
            const float available=ImGui::GetContentRegionAvail().y;
            {NumericFont numeric(ui);spectrum_view(ui,snapshot,std::max(215.f,available*.64f));}
            ImGui::Spacing();
            ImGui::BeginChild("liveResults",{0,0},false);
            if(ImGui::BeginTabBar("eventKinds")){
                if(ImGui::BeginTabItem("Detected signals")){compact_signal_table(ui,snapshot,std::max(90.f,ImGui::GetContentRegionAvail().y-8));ImGui::EndTabItem();}
                if(ImGui::BeginTabItem("Decoded messages")){packet_table(ui,snapshot,std::max(90.f,ImGui::GetContentRegionAvail().y-44));ImGui::EndTabItem();}
                if(ImGui::BeginTabItem("Energy details")){live_burst_table(ui,snapshot,ImGui::GetContentRegionAvail().y-50);ImGui::EndTabItem();}
                ImGui::EndTabBar();
            }
            ImGui::EndChild();ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Analyze",nullptr,ui.focus_analysis?ImGuiTabItemFlags_SetSelected:ImGuiTabItemFlags_None)){
            ui.focus_analysis=false;compact_analysis_tab(engine,ui,snapshot);ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();ImGui::Separator();
    if(ui.operation_busy())ImGui::TextColored(secondary,"%s",ui.operation_label.c_str());
    else if(!snapshot.error.empty())wrapped(snapshot.error.c_str(),red);
    else if(!ui.notice.empty()){
        wrapped(ui.notice.c_str(),ui.notice_error?red:muted);
        if(ImGui::IsItemClicked()&&!ui.notice_error)ui.notice.clear();
    }
    const auto& active = snapshot.session_id.empty() ? ui.config : snapshot.config;
    const bool discovery = active.discover_lora && (!snapshot.session_id.empty() || !ui.spectrum_only);
    const bool profiles = std::any_of(active.lanes.begin(), active.lanes.end(), [](const auto& lane) { return lane.enabled; }) &&
        (!snapshot.session_id.empty() || (!ui.spectrum_only && ui.decode_enabled));
    ImGui::TextDisabled("%s", discovery ? "Spectrum + LoRa / waveform discovery" : profiles ? "Spectrum + configured-profile decoding" : "Spectrum only / energy measurements");
    if(discovery || profiles){ImGui::SameLine(0,18);if(ImGui::SmallButton(ui.key_count(engine)?"Decoding settings":"Decoding needs setup")){ui.settings_page=0;ui.show_settings=true;}}
    session_disk_footer(ui,snapshot);
    if(ui.new_requested||ui.close_requested||!ui.pending_open_path.empty())ImGui::OpenPopup("Unrecorded session");
    if(ImGui::BeginPopupModal("Unrecorded session",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        ImGui::TextUnformatted("This session was not recorded. Its measurements will be lost.");
        ImGui::TextUnformatted("Save cannot recover the full history of a memory-only session.");
        if(ImGui::Button(ui.close_requested?"Discard and close":!ui.pending_open_path.empty()?"Discard and open":"Discard and create new")){
            if(ui.close_requested){ui.close_approved=true;ui.close_requested=false;}
            else if(!ui.pending_open_path.empty()){const auto path=std::move(ui.pending_open_path);ui.pending_open_path.clear();request_open_session(engine,ui,path);}
            else request_new_session(engine,ui,true);
            ui.new_requested=false;ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();if(ImGui::Button("Keep session")){ui.new_requested=false;ui.close_requested=false;ui.restart_after_new=false;ui.pending_open_path.clear();ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
    ImGui::End();
    settings_window(engine,ui,snapshot);
    if(ui.show_diagnostics){ImGui::SetNextWindowSize({850,660},ImGuiCond_Appearing);
        if(ImGui::Begin("Measurement and receiver details",&ui.show_diagnostics)){
            health_strip(snapshot);lane_health_table(snapshot);receiver_diagnostics(snapshot,true);
            waveform_panel(snapshot,160,ui.config.discover_lora);
            ImGui::TextWrapped("Recording file: %s",snapshot.config.session_path.empty()?"Not recording":snapshot.config.session_path.c_str());
            ImGui::TextWrapped("GPS: %s",snapshot.gps_status.c_str());
            if(snapshot.incomplete)wrapped("Recording is incomplete. Preserve it and review the errors.",red);
            wrapped("Uncalibrated relative power. Unknown upstream sample loss is not counted as known good coverage.");
        }ImGui::End();}
    if(ui.show_analysis_details){ImGui::SetNextWindowSize({960,760},ImGuiCond_Appearing);
        if(ImGui::Begin("Advanced analysis and measurement details",&ui.show_analysis_details))advanced_analysis_tab(engine,ui,snapshot);
        ImGui::End();}
    dialogs(engine,ui,snapshot);file_picker_dialog(ui,snapshot);
    if(font)ImGui::PopFont();
}
