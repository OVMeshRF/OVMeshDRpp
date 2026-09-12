// SPDX-License-Identifier: GPL-3.0-or-later
// Included by ui_workflow.hpp after shared desktop/chart helpers.

void concentrator_settings(Engine& engine, DesktopState& ui, const Snapshot& snapshot) {
    label("RAK concentrators");
    wrapped(desktop_lora_enabled ? "Choose one or two USB/LBT boards. Each has one configured LoRa receiver and a separate swept energy scanner." :
        "Choose one or two USB/LBT boards for sampled RF energy scans across the survey range.");
    if (!ui.concentrator_inventory_loaded && !ui.passive_smoke && !snapshot.historical) ui.refresh_concentrators();
    ImGui::BeginDisabled(ui.passive_smoke || snapshot.historical || ui.operation_busy());
    if (ImGui::Button("Refresh USB candidates")) ui.refresh_concentrators();
    ImGui::EndDisabled();
    if (!ui.concentrator_devices.error.empty()) wrapped(ui.concentrator_devices.error.c_str(), red);
    else {
        ImGui::Text("USB candidates found: %zu", ui.concentrator_devices.devices.size());
        if (ui.concentrator_devices.devices.empty())
            wrapped("Connect a USB concentrator, then refresh. GPS receivers are not concentrator candidates.", amber);
    }
    wrapped("0483:5740 is a generic STM32 identity. Select only a concentrator you recognize; listing candidates does not open their ports.", amber);
    const bool session_started = !snapshot.session_id.empty();
    if (session_started && !snapshot.historical)
        wrapped(desktop_lora_enabled ? "Choose New to change concentrator boards or scan/packet settings. Start and Stop preserve the current survey setup." :
            "Choose New to change concentrator boards or scan settings. Start and Stop preserve the current survey setup.", amber);
    const bool locked = snapshot.running || snapshot.historical || session_started || ui.operation_busy();
    ImGui::BeginDisabled(locked);
    auto historical = snapshot.config.concentrators;
    auto& cfg = snapshot.historical ? historical : ui.config.concentrators;
    bool changed = false;
    int count = static_cast<int>(cfg.boards.size()) - 1;
    if (ImGui::Combo("Number of boards", &count, "One\0Two\0")) {
        const auto previous = cfg.boards.size();
        cfg.boards.resize(static_cast<size_t>(count + 1));
        if (previous < 2 && cfg.boards.size() == 2) {
            cfg.boards[1].frequency_hz = 908750000;
            cfg.boards[1].bandwidth_hz = 500000;
        }
        changed = true;
    }
    for (size_t i = 0; i < cfg.boards.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        auto& board = cfg.boards[i];
        ImGui::Separator(); ImGui::Text("Board %zu", i + 1);
        const auto used_by_other_board = [&](const ConcentratorDevice& device) {
            for (size_t j = 0; j < cfg.boards.size(); ++j)
                if (j != i && (cfg.boards[j].device_id == device.stable_id || cfg.boards[j].device_path == device.path)) return true;
            return false;
        };
        if (board.device_id.empty() && board.device_path.empty() && ui.concentrator_devices.error.empty()) {
            const ConcentratorDevice* available = nullptr;
            size_t eligible = 0;
            for (const auto& device : ui.concentrator_devices.devices)
                if (!used_by_other_board(device) && select_concentrator_device(ui.concentrator_devices.devices, device.stable_id)) {
                    available = &device; ++eligible;
                }
            if (eligible == 1) {
                const auto description = "Available: " + available->label + " / " + available->path;
                wrapped(description.c_str());
                if (ImGui::Button("Select this board")) {
                    board.device_id = available->stable_id; board.device_path = available->path; changed = true;
                }
            }
        }
        const auto selected = ui.concentrator_devices.error.empty()
            ? select_concentrator_device(ui.concentrator_devices.devices, board.device_id) : std::nullopt;
        const std::string title = snapshot.historical ? "Recorded board / USB identity not retained" : selected ? ui.concentrator_devices.devices[*selected].label + " / " + ui.concentrator_devices.devices[*selected].path :
            board.device_id.empty() ? "Choose USB concentrator..." : "Remembered board unavailable or ambiguous";
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##boardDevice", title.c_str())) {
            if (ImGui::Selectable("No board selected", board.device_id.empty())) {
                board.device_path.clear(); board.device_id.clear(); changed = true;
            }
            for (size_t d = 0; d < ui.concentrator_devices.devices.size(); ++d) {
                const auto& device = ui.concentrator_devices.devices[d];
                const bool used = used_by_other_board(device);
                const bool unique = select_concentrator_device(ui.concentrator_devices.devices, device.stable_id).has_value();
                ImGui::BeginDisabled(used || !unique || !ui.concentrator_devices.error.empty());
                const auto option = device.label + " / " + device.path + (used ? " (already selected)" : "") + "###candidate" + std::to_string(d);
                if (ImGui::Selectable(option.c_str(), selected && *selected == d)) {
                    board.device_path = device.path; board.device_id = device.stable_id; changed = true;
                }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if (desktop_lora_enabled) {
        if (ImGui::Checkbox("Receive LoRa packets", &board.packets_enabled)) changed = true;
        ImGui::BeginDisabled(!board.packets_enabled);
        const meshtastic::Preset* matching_preset = nullptr;
        for (const auto& preset : meshtastic::presets)
            if (preset.bandwidth_hz == board.bandwidth_hz && preset.spreading_factor == board.spreading_factor && board.sync_word == 0x2b) {
                matching_preset = &preset; break;
            }
        const auto preset_preview = matching_preset ? std::string(matching_preset->name) : std::string("Manual settings");
        if (ImGui::BeginCombo("Apply preset", preset_preview.c_str())) {
            for (const auto& preset : meshtastic::presets) {
                const bool supported = rak_preset_supported(preset);
                const auto title = std::string(preset.name) + (preset.historical_only ? " (historical)" : preset.deprecated ? " (deprecated)" : "") +
                    (!supported ? " (requires SDR support)" : "");
                ImGui::BeginDisabled(!supported);
                if (ImGui::Selectable(title.c_str(), matching_preset == &preset)) {
                    board.bandwidth_hz = preset.bandwidth_hz; board.spreading_factor = preset.spreading_factor;
                    board.sync_word = 0x2b; changed = true;
                }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Preset changes modulation only; frequency stays unchanged. Edit fields below for custom reception.");
        ImGui::TextDisabled("RAK: 125/250/500 kHz, SF7-12. Narrower presets require SDR support; CR comes from the packet header.");
        double frequency = board.frequency_hz / 1e6;
        if (ImGui::InputDouble("Packet frequency / MHz", &frequency, 0, 0, "%.6f") && std::isfinite(frequency)) {
            board.frequency_hz = static_cast<uint64_t>(std::clamp(frequency, 902.0, 928.0) * 1e6); changed = true;
        }
        constexpr uint32_t widths[] = {125000,250000,500000};
        int bw = board.bandwidth_hz == 125000 ? 0 : board.bandwidth_hz == 500000 ? 2 : 1;
        if (ImGui::Combo("Packet bandwidth", &bw, "125 kHz\0 250 kHz\0 500 kHz\0")) { board.bandwidth_hz = widths[bw]; changed = true; }
        int sf = static_cast<int>(board.spreading_factor);
        if (ImGui::SliderInt("Spreading factor", &sf, 7, 12, "SF%d")) { board.spreading_factor = static_cast<unsigned>(sf); changed = true; }
        constexpr unsigned words[] = {0x2b,0x12,0x34};
        int word = board.sync_word == 0x12 ? 1 : board.sync_word == 0x34 ? 2 : 0;
        if (ImGui::Combo("Sync word", &word, "0x2B / Meshtastic\0 0x12 / private LoRa\0 0x34 / public LoRaWAN\0")) { board.sync_word = words[word]; changed = true; }
        ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
    ImGui::Spacing(); ImGui::Separator();
    if (ImGui::Checkbox("Scan RF energy across the survey range", &cfg.scan_enabled)) changed = true;
    ImGui::BeginDisabled(!cfg.scan_enabled);
    int step = static_cast<int>(cfg.scan_step_hz / 1000);
    if (ImGui::InputInt("Scan step / kHz", &step)) { cfg.scan_step_hz = static_cast<uint32_t>(std::clamp(step, 25, 1000) * 1000); changed = true; }
    ImGui::TextDisabled("2,000 RSSI samples per position / approximately 234.3 kHz receive filter");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (changed) ui.persist_preferences();
    wrapped("Scanning revisits frequencies sequentially. Unobserved intervals remain unknown.");
    if (desktop_lora_enabled) {
    wrapped("Packet reception searches only the explicit frequency, bandwidth, spreading factor and sync word on each board.");
    ImGui::Text("Configured key records: %zu", ui.key_count(engine));
    if (ImGui::Button("Configure authorized keys...")) { ui.show_keys = true; ui.authorize_keys = false; erase_secret(ui.key_input); }
    wrapped("A sync word or CRC-valid packet is not authenticated device identity. Explicitly configured keys permit likely Meshtastic envelope classification. This may produce false positives; message contents are not interpreted or retained.");
    }
}

void concentrator_health_view(const Snapshot& snapshot) {
    for (size_t i = 0; i < snapshot.concentrator_health.size(); ++i) {
        const auto& h = snapshot.concentrator_health[i];
        ImGui::TextColored(h.ready ? accent : amber, "Board %zu / %s", i + 1, h.state.c_str());
        ImGui::SameLine();
        if (desktop_lora_enabled) ImGui::TextDisabled("%llu scans / %llu receptions / %llu CRC failures",
            static_cast<unsigned long long>(h.scans), static_cast<unsigned long long>(h.receptions), static_cast<unsigned long long>(h.crc_failures));
        else ImGui::TextDisabled("%llu scans", static_cast<unsigned long long>(h.scans));
    }
}

// One color cell is the latest sampled histogram at a frequency on one board.
// There is deliberately no time axis implying continuous IQ observation.
void concentrator_scan_view(const DesktopState& ui, const Snapshot& snapshot, float height, bool show_table = true) {
    const auto& cfg = snapshot.session_id.empty() ? ui.config : snapshot.config;
    const auto& scans = snapshot.recent_concentrator_scans;
    ImGui::TextUnformatted("Sampled RF energy / latest reading at each frequency");
    ImGui::TextDisabled("%llu scans / %llu RSSI samples / %zu displayed frequency readings",
        static_cast<unsigned long long>(snapshot.concentrator_scans), static_cast<unsigned long long>(snapshot.concentrator_rssi_samples), scans.size());
    wrapped("Color: samples above -87 dBm, using whole 4 dB histogram bins. Approximate 234.3 kHz receive filter; uncalibrated. These are sampled exceedance fractions, not continuous channel busy time.");
    const float width = std::max(160.f, ImGui::GetContentRegionAvail().x);
    const size_t boards = std::max<size_t>(1, cfg.concentrators.boards.size());
    const float row = 38.f, chart_h = 30.f + row * static_cast<float>(boards);
    const auto origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##sampledRfHeatmap", {width,chart_h});
    const double lower = double(cfg.center_hz) - cfg.survey_span_hz * .5;
    const double span = std::max(1.0, double(cfg.survey_span_hz));
    const float left = origin.x + 64, usable = std::max(1.f,width - 70);
    auto x = [&](double f) { return left + static_cast<float>(std::clamp((f-lower)/span,0.,1.)) * usable; };
    for (size_t b = 0; b < boards; ++b) {
        const float y = origin.y + row * static_cast<float>(b);
        const auto title = "Board " + std::to_string(b + 1);
        draw->AddText({origin.x,y+8}, IM_COL32(139,163,181,255), title.c_str());
        draw->AddRectFilled({left,y},{left+usable,y+row-5},IM_COL32(39,49,57,255));
    }
    for (const auto& scan : scans) {
        if (scan.board_index >= boards || concentrator_sample_count(scan) == 0) continue;
        const auto fraction = std::clamp(concentrator_fraction_above(scan,-87),0.,1.);
        const auto color = ImGui::ColorConvertFloat4ToU32({float(.05+.95*fraction),float(.73-.35*fraction),float(.76-.62*fraction),1});
        const float y = origin.y + row * static_cast<float>(scan.board_index);
        const double half = std::min(cfg.concentrators.scan_step_hz,scan.filter_bandwidth_hz) * .5;
        draw->AddRectFilled({x(double(scan.frequency_hz)-half),y},{x(double(scan.frequency_hz)+half),y+row-5},color);
        if (ImGui::IsItemHovered() && ImGui::GetIO().MousePos.y >= y && ImGui::GetIO().MousePos.y < y+row-5 &&
            ImGui::GetIO().MousePos.x >= x(double(scan.frequency_hz)-half) && ImGui::GetIO().MousePos.x <= x(double(scan.frequency_hz)+half))
            ImGui::SetTooltip("Board %u / %.6f MHz\n%.3f%% of %llu samples above -87 dBm\nLatest scan: %s UTC\nElapsed %.3f-%.3f s (host transaction bounds)\nCell marks a scan center; it is not measured signal bandwidth.",
                scan.board_index+1,scan.frequency_hz/1e6,100*fraction,static_cast<unsigned long long>(concentrator_sample_count(scan)),
                timestamp_text(scan.utc_end_seconds).c_str(),scan.elapsed_start_seconds,scan.elapsed_end_seconds);
    }
    const auto first = std::to_string(lower/1e6)+" MHz", last = std::to_string((lower+span)/1e6)+" MHz";
    draw->AddText({left,origin.y+chart_h-22},IM_COL32(139,163,181,255),first.c_str());
    draw->AddText({origin.x+width-ImGui::CalcTextSize(last.c_str()).x,origin.y+chart_h-22},IM_COL32(139,163,181,255),last.c_str());
    wrapped("Cyan 0% / orange 100% / gray no displayed sample. Each cell may have a different age; hover for its time. Colors do not identify transmitters.");
    if (scans.empty()) wrapped(cfg.concentrators.scan_enabled ? "Waiting for sampled RF readings." : "RF scanning is disabled for this session.", amber);
    if (!show_table) return;
    const auto flags = ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_ScrollY|ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("concentratorSamples",7,flags,{0,std::max(100.f,height-chart_h-110)})) {
        ImGui::TableSetupColumn("Sample end (UTC)",ImGuiTableColumnFlags_WidthFixed,ImGui::CalcTextSize("0000-00-00 00:00:00.000").x+10);
        ImGui::TableSetupColumn("Board",ImGuiTableColumnFlags_WidthFixed,45);
        for (const char* title : {"Center MHz","Samples","> -87 dBm %","Host interval ms","Receiver GPS"}) ImGui::TableSetupColumn(title);
        ImGui::TableSetupScrollFreeze(0,1); ImGui::TableHeadersRow();
        ImGuiListClipper clip; clip.Begin(static_cast<int>(scans.size()));
        while (clip.Step()) for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            const auto& s = scans[static_cast<size_t>(i)];
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(timestamp_text(s.utc_end_seconds).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%u",s.board_index+1);
            ImGui::TableNextColumn(); ImGui::Text("%.6f",s.frequency_hz/1e6);
            ImGui::TableNextColumn(); ImGui::Text("%llu",static_cast<unsigned long long>(concentrator_sample_count(s)));
            ImGui::TableNextColumn(); if (concentrator_sample_count(s)) ImGui::Text("%.3f",100*concentrator_fraction_above(s,-87)); else ImGui::TextUnformatted("--");
            ImGui::TableNextColumn(); ImGui::Text("%.3f",1000*(s.elapsed_end_seconds-s.elapsed_start_seconds));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.receiver_position && s.receiver_position->valid ? "Available" : "Missing");
        }
        ImGui::EndTable();
    }
}
