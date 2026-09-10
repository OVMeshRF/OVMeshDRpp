// SPDX-License-Identifier: GPL-3.0-or-later
// Internal report.cpp implementation; reuses the same validated streaming
// aggregates as CSV. No network, scripts, source-file links or decoder payloads.
std::string html_escape(std::string_view value) {
    std::string out;
    for (const char c : value) {
        switch(c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: if (static_cast<unsigned char>(c) >= 32 || c == '\n' || c == '\t') out += c;
        }
    }
    return out;
}
std::string human_number(double value, unsigned places = 3) {
    if (!std::isfinite(value)) return "Unavailable";
    std::ostringstream out; out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(int(places)) << value; return out.str();
}
std::string human_percent(double busy, double observed) {
    return observed > 0 ? human_number(100 * std::clamp(busy / observed, 0., 1.)) + "%" : "Unavailable";
}
std::string human_utc(double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0 || seconds >= 253402300800. ||
        static_cast<long double>(seconds) > static_cast<long double>(std::numeric_limits<std::time_t>::max()))
        return "Unavailable";
    const auto stamp = static_cast<std::time_t>(seconds); std::tm t{};
#ifdef _WIN32
    if (gmtime_s(&t, &stamp) != 0) return "Unavailable";
#else
    if (!gmtime_r(&stamp, &t)) return "Unavailable";
#endif
    char date[40]{};
    return std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S UTC", &t) ? date : "Unavailable";
}
struct HtmlReport {
    const std::function<void(std::string_view)>& emit;
    size_t bytes = 0;
    void raw(const std::string& s) {
        if (s.size() > 16 * 1024 * 1024 - bytes)
            throw std::runtime_error("Analysis report exceeds 16 MiB; narrow the selection");
        bytes += s.size(); emit(s);
    }
    void paragraph(const std::string& s) { raw("<p>" + html_escape(s) + "</p>\n"); }
    void heading(const std::string& s) { raw("<h2>" + html_escape(s) + "</h2>\n"); }
    void row(const std::vector<std::string>& values, bool header = false) {
        raw("<tr>"); for (const auto& v : values)
            raw(std::string(header ? "<th>" : "<td>") + html_escape(v) + (header ? "</th>" : "</td>"));
        raw("</tr>\n");
    }
    void table(const std::vector<std::string>& headings) { raw("<table><thead>"); row(headings, true); raw("</thead><tbody>"); }
    void end_table() { raw("</tbody></table>\n"); }
};

void analysis_report(const SessionStore& store, const Context& c,
                     const std::function<void(std::string_view)>& emit) {
    if (c.schema < 4) throw std::runtime_error("This legacy recording lacks fine spectrum history for an analysis report");
    ReportOptions frequency_options = c.options, time_options = c.options, geo_options = c.options;
    frequency_options.kind = ReportKind::FrequencySummary;
    time_options.kind = ReportKind::TimeSummary;
    geo_options.kind = ReportKind::GeographicSummary;
    // Constructing each context validates the requested grouping before any output.
    const Context fc(frequency_options,c.summary,c.schema), tc(time_options,c.summary,c.schema);
    AggregateReport frequency(fc), time(tc);
    std::optional<Context> gc;
    std::optional<AggregateReport> geography;
    if (c.options.privacy.include_receiver_positions) {
        gc.emplace(geo_options,c.summary,c.schema); geography.emplace(*gc);
    }
    store.visit_tiles([&](const SpectrumTile& tile) {
        frequency.consume(tile); time.consume(tile); if (geography) geography->consume(tile);
    });
    uint64_t gap_count = 0;
    store.visit_gaps([&](const CoverageGap& gap) {
        if (gap.elapsed_end_seconds > c.options.query.elapsed_start && gap.elapsed_start_seconds < c.end) ++gap_count;
        frequency.gap(gap); time.gap(gap);
    });
    if (!frequency.grid || frequency.bins.empty() || frequency.latest <= frequency.earliest)
        throw std::runtime_error("No fine spectrum history intersects this report selection");
    double observed = 0, busy = 0, outside_busy = 0, missing = 0, support = 0;
    uint32_t quality = 0;
    for (const auto& [index,a] : time.times) {
        (void)index; observed += a.observed; busy += a.busy; outside_busy += a.outside_busy;
        missing += a.missing_position; support = std::max(support,a.power_support_max); quality |= a.quality;
    }
    const double from = c.options.query.elapsed_start;
    const double end = c.options.query.elapsed_end > 0 ? c.end : std::max(c.summary.elapsed_seconds,frequency.latest);
    const double duration = std::max(0.,end-from);
    size_t active_bins = 0, outside_bins = 0;
    double bin_busy = 0, bin_observed = 0;
    std::vector<size_t> ranked;
    for (size_t i = 0; i < frequency.bins.size(); ++i) {
        const auto& a = frequency.bins[i]; if (a.observed <= 0) continue;
        if (a.busy > 0) ++active_bins;
        bin_busy += a.busy; bin_observed += a.observed;
        const double center = frequency.grid_first + double(frequency.first+i)*frequency.width;
        if (std::abs(center-double(c.summary.config.center_hz)) > 2*frequency.width+1e-5) {
            ++outside_bins; ranked.push_back(i);
        }
    }
    std::stable_sort(ranked.begin(),ranked.end(),[&](size_t a,size_t b) {
        return frequency.bins[a].busy/frequency.bins[a].observed > frequency.bins[b].busy/frequency.bins[b].observed;
    });
    struct WaveGroup { uint64_t count=0,partial=0,ambiguous=0; double low=1e10,high=0; };
    std::map<std::pair<unsigned,unsigned>,WaveGroup> waves;
    uint64_t wave_count=0, receptions=0, decoded=0, bad_crc=0;
    if (c.schema >= 5) store.visit_waveforms([&](const WaveformObservation& w) {
        if (!c.selected(w.center_hz,w.bandwidth_hz,w.delimiter_elapsed,w.receiver_position)) return;
        auto& group=waves[{w.bandwidth_hz,w.spreading_factor}];
        ++group.count; group.partial+=!w.complete_in_requested_range; group.ambiguous+=w.association_ambiguous;
        group.low=std::min(group.low,w.center_hz); group.high=std::max(group.high,w.center_hz); ++wave_count;
    });
    store.visit_receptions([&](const Reception& r) {
        if (!c.selected(r.frequency_hz,r.bandwidth_hz,r.elapsed_seconds,r.receiver_position)) return;
        ++receptions;
        if (r.crc_valid && r.decoded.status==protocol::Status::decoded && r.decoded.authorized) ++decoded;
        if (r.decoded.status==protocol::Status::bad_phy_crc) ++bad_crc;
    });

    HtmlReport h{emit};
    h.raw(R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'">
<title>RF survey analysis — OVMeshDR++</title><style>
:root{font-family:system-ui,-apple-system,sans-serif;color:#172c3c;background:#eaf0f3;line-height:1.55}
body{max-width:1120px;margin:36px auto;padding:38px;background:white;border-top:8px solid #008b87}
h1{font-size:2.3rem;line-height:1.15;margin:10px 0}h2{margin-top:36px;border-bottom:1px solid #bfd1db;padding-bottom:8px}
p{max-width:95ch}.eyebrow{color:#007d79;letter-spacing:.1em;font-weight:700}.note{border-left:4px solid #bd7222;background:#fff7eb;padding:12px 18px}
table{border-collapse:collapse;width:100%;font-size:.86rem;font-variant-numeric:tabular-nums;margin:15px 0;overflow-wrap:anywhere}
th,td{padding:8px 10px;text-align:left;border-bottom:1px solid #d6e1e7}th{background:#eaf2f5}tr:nth-child(even){background:#f6f9fa}
.plot{display:block;width:100%;height:auto;background:#f3f8fa}summary{cursor:pointer;font-weight:600}footer{margin-top:40px;font-size:.85rem;color:#506476}
@media(max-width:700px){body{margin:0;padding:16px}table{font-size:.73rem}th,td{padding:5px}}
@media print{:root{background:white}body{max-width:none;margin:0;padding:0;border:0}h2{break-after:avoid}tr,svg{break-inside:avoid}thead{display:table-header-group}}
</style></head><body><div class="eyebrow">OVMeshDR++ / RF SURVEY</div><h1>Measured activity &amp; survey findings</h1>
)HTML");
    h.paragraph("Generated " + human_utc(double(std::time(nullptr))) + "; application " + Engine::version() + "; report method 1; recording schema " + std::to_string(c.schema) + ".");
    h.paragraph("Source session ID: " + c.summary.session_id);
    if (c.options.privacy.include_provenance) {
        h.paragraph("Survey: " + c.summary.config.session_title);
        h.paragraph("Antenna: " + c.summary.config.antenna_description + "; receiver: " + c.summary.config.receiver_description);
        h.paragraph("Operator notes: " + c.summary.config.survey_notes);
    }
    h.raw("<div class=\"note\">");
    h.paragraph(c.summary.config.synthetic ? "SYNTHETIC SOURCE — demonstration/test measurements, not an over-the-air survey."
        : "Experimental receiver measurements at the survey receiver. This report does not certify channel suitability, transmitter power or regulatory compliance.");
    if (c.summary.incomplete) h.paragraph("The source recording is marked incomplete. Treat this as a partial survey.");
    h.raw("</div>");
    h.heading("1. What was observed");
    h.table({"Measure","Selected-scope result"});
    h.row({"Requested frequency range",human_number(c.lower/1e6,6)+" – "+human_number(c.upper/1e6,6)+" MHz"});
    h.row({"Actual intersecting bin edges",human_number(frequency.covered_lower()/1e6,6)+" – "+human_number(frequency.covered_upper()/1e6,6)+" MHz"});
    h.row({"UTC interval (host-estimated)",human_utc(frequency.utc_anchor+from)+" through "+human_utc(frequency.utc_anchor+end)});
    h.row({"Elapsed interval",human_number(from)+" – "+human_number(end)+" s"});
    h.row({"Measured exposure",human_number(observed,6)+" s; "+human_percent(observed,duration)+" of selected elapsed time"});
    h.row({"Any-bin occupancy",human_percent(busy,observed)+"; "+human_number(busy,6)+" s busy / "+human_number(observed,6)+" s observed"});
    h.row({"Any-bin occupancy outside center guard",outside_bins?human_percent(outside_busy,observed):"Unavailable — no observed bins outside guard"});
    h.row({"Mean frequency-time occupancy",human_percent(bin_busy,bin_observed)});
    h.row({"Recorded acquisition gaps (all locations)",std::to_string(gap_count)+" records; "+human_number(frequency.explicit_gaps,6)+" s"});
    h.row({"RF exposure without a valid tile-end receiver position",human_number(missing,6)+" s; "+human_percent(missing,observed)});
    h.row({"Geographic filter",c.options.query.geographic_filter?"Applied; excludes missing and outside-area receiver fixes":"Not applied"});
    if(c.options.query.geographic_filter) h.row({"Excluded exposure with missing GPS",human_number(frequency.rejected_gps,6)+" s"});
    h.end_table();
    h.paragraph("Any-bin occupancy counts time when at least one selected bin exceeds the saved threshold; it is not the percentage of the entire band occupied at once. Mean frequency-time occupancy averages exposure across equal-width bins. Neither is a packet count, a transmitter's airtime, or Meshtastic's device-reported channel-utilization metric.");
    h.paragraph("Center guard: bin centers within two FFT bin spacings of " + human_number(double(c.summary.config.center_hz)/1e6,6) +
        " MHz. The outside-guard comparison leaves those frequencies unassessed; raw occupancy above still includes them. This is a receiver-artifact diagnostic, not proof that center activity is artificial.");
    if(observed<=0) h.paragraph("No measured exposure passed this selection. No conclusion about quiet spectrum can be drawn.");
    else {
        h.paragraph(std::to_string(active_bins)+" of "+std::to_string(frequency.bins.size())+" intersecting frequency bins showed above-threshold activity. Bins are measurement intervals, not channels or devices.");
        if(!ranked.empty()) {
            const auto best=ranked.front(); const auto& a=frequency.bins[best];
            h.paragraph("The busiest measured bin outside the center guard was centered at "+human_number((frequency.grid_first+double(frequency.first+best)*frequency.width)/1e6,6)+
                " MHz, with "+human_percent(a.busy,a.observed)+" occupancy. Its width is "+human_number(frequency.width/1000)+" kHz; this does not identify a modem bandwidth.");
        }
    }
    if(frequency.covered_lower()>c.lower+frequency.width || frequency.covered_upper()<c.upper-frequency.width)
        h.paragraph("The requested frequency interval extends beyond the recorded bin coverage. The uncovered edges have no measured occupancy.");
    if(observed+1e-6<duration) h.paragraph("Some selected elapsed time lacks included RF observations. Gaps, geographic exclusions and time outside the recorded data are not quiet time. Percentages use only measured exposure; unreported upstream loss may still exist.");

    h.heading("2. Activity by frequency");
    // Peak-preserving display columns. All bin values are retained below.
    const size_t columns=std::min<size_t>(320,frequency.bins.size());
    h.raw("<svg class=\"plot\" viewBox=\"0 0 1000 245\" role=\"img\" aria-label=\"Frequency occupancy on a linear zero to one hundred percent scale\">");
    h.raw("<text x=\"4\" y=\"18\">100%</text><text x=\"8\" y=\"210\">0%</text><path d=\"M55 15V210H985\" fill=\"none\" stroke=\"#677d8d\"/>");
    for(size_t col=0;col<columns;++col) {
        const size_t begin=col*frequency.bins.size()/columns, finish=(col+1)*frequency.bins.size()/columns;
        double maximum=0; bool any=false;
        for(size_t i=begin;i<finish;++i) {const auto& a=frequency.bins[i]; if(a.observed>0){any=true;maximum=std::max(maximum,a.busy/a.observed);}}
        if(!any)continue;
        const double height=maximum>0?std::max(1.,195*std::clamp(maximum,0.,1.)):0;
        h.raw("<rect x=\""+human_number(55+930*double(col)/double(columns))+"\" y=\""+human_number(210-height)+"\" width=\""+
            human_number(930/double(columns))+"\" height=\""+human_number(height)+"\" fill=\"#008d87\"><title>Maximum bin occupancy: "+human_percent(maximum,1)+"</title></rect>");
    }
    h.raw("<text x=\"55\" y=\"237\">"+human_number(frequency.covered_lower()/1e6,6)+" MHz</text><text x=\"985\" y=\"237\" text-anchor=\"end\">"+human_number(frequency.covered_upper()/1e6,6)+" MHz</text></svg>");
    h.paragraph("Each plotted column preserves the highest bin occupancy within its frequency span. Positive values have a one-pixel minimum for visibility; the table gives exact percentages. Plot includes the center guard. All summaries retain the original fixed detection threshold.");
    h.raw("<details><summary>Complete selected frequency table ("+std::to_string(frequency.bins.size())+" bins)</summary>");
    h.table({"Lower MHz","Upper MHz","Observed s","Busy s","Occupancy","Mean dBFS","Peak dBFS"});
    for(size_t i=0;i<frequency.bins.size();++i) {
        const auto& a=frequency.bins[i]; const double center=frequency.grid_first+double(frequency.first+i)*frequency.width;
        h.row({human_number((center-frequency.width/2)/1e6,6),human_number((center+frequency.width/2)/1e6,6),human_number(a.observed,6),
            human_number(a.busy,6),human_percent(a.busy,a.observed),a.observed>0?human_number(10*std::log10(a.mean_power/a.observed),2):"Unavailable",
            a.peak_power>0?human_number(10*std::log10(a.peak_power),2):"Unavailable"});
    }
    h.end_table(); h.raw("</details>");
    h.paragraph("A lower occupancy value identifies less detected activity under these measurement conditions, not a recommended or interference-free channel. Use the app's range controls to examine any desired channel-width interval; this report does not assume preset frequencies or snap observations to channel slots.");

    h.heading("3. When activity occurred");
    std::vector<std::pair<uint64_t,const Accumulator*>> time_rows;
    for(const auto& [index,a]:time.times)if(a.observed>0)time_rows.emplace_back(index,&a);
    std::stable_sort(time_rows.begin(),time_rows.end(),[](const auto& a,const auto& b){return a.second->busy/a.second->observed>b.second->busy/b.second->observed;});
    h.paragraph(std::to_string(time_rows.size())+" time groups contained observations. Showing up to ten groups with the highest any-bin occupancy; requested grouping is "+
        human_number(c.options.query.time_bucket_seconds)+" s. Partial groups use their actual measured exposure. This is not a rolling busy-hour estimate. Export Time summary CSV for all groups, including unobserved intervals.");
    h.table({"Elapsed start–end s","Observed s","Busy s","Any-bin occupancy","Outside guard"});
    for(size_t i=0;i<std::min<size_t>(10,time_rows.size());++i) {
        const auto& [index,a]=time_rows[i];
        h.row({human_number(time.edge(index))+" – "+human_number(std::min(end,time.edge(index+1))),human_number(a->observed,6),human_number(a->busy,6),
            human_percent(a->busy,a->observed),outside_bins?human_percent(a->outside_busy,a->observed):"Unavailable"});
    }
    h.end_table();

    h.heading("4. Receiver locations");
    if(!geography) h.paragraph("Coordinates and geographic cells were excluded by the report's privacy selection. Missing-position exposure is still reported above. Enable receiver GPS coordinates to include grouped location findings; no GPS fix is fabricated.");
    else {
        if(c.options.query.geographic_filter) h.paragraph("Selected rectangle: south "+human_number(rounded(c.options.query.south,c.options.privacy.coordinate_decimals),c.options.privacy.coordinate_decimals)+
            ", north "+human_number(rounded(c.options.query.north,c.options.privacy.coordinate_decimals),c.options.privacy.coordinate_decimals)+
            ", west "+human_number(rounded(c.options.query.west,c.options.privacy.coordinate_decimals),c.options.privacy.coordinate_decimals)+
            ", east "+human_number(rounded(c.options.query.east,c.options.privacy.coordinate_decimals),c.options.privacy.coordinate_decimals));
        struct Place { const GeoValue* value; size_t bin; double occupancy; };
        std::vector<Place> places;
        for(const auto& [key,value]:geography->cells) {
            (void)key;if(!value.cell.located()||value.common.observed<=0)continue;
            size_t bin=0;for(size_t i=1;i<value.bins.size();++i)if(value.bins[i].busy>value.bins[bin].busy)bin=i;
            places.push_back({&value,bin,value.bins[bin].busy/value.common.observed});
        }
        std::stable_sort(places.begin(),places.end(),[](const auto& a,const auto& b){return a.occupancy>b.occupancy;});
        h.paragraph(std::to_string(places.size())+" receiver cells contain measured exposure. Showing up to twenty cells ordered by their busiest bin, including center-guard bins. Approximate cell size: "+human_number(c.options.geographic_cell_m,1)+
            " m. The complete per-frequency geographic breakdown is available as Geographic summary CSV. Sparse visits are not representative of an entire region.");
        h.table({"Cell ID","South / north","West / east","Observed s","Busiest bin MHz","Bin occupancy","Cell-crossing attribution s"});
        for(size_t i=0;i<std::min<size_t>(20,places.size());++i) {
            const auto& p=places[i];const auto& cell=p.value->cell;const auto n=c.options.privacy.coordinate_decimals;
            h.row({std::to_string(cell.row)+":"+std::to_string(cell.column),human_number(rounded(cell.south,n),n)+" / "+human_number(rounded(cell.north,n),n),
                human_number(rounded(cell.west,n),n)+" / "+human_number(rounded(cell.east,n),n),human_number(p.value->common.observed,6),
                human_number((frequency.grid_first+double(frequency.first+p.bin)*frequency.width)/1e6,6),human_percent(p.occupancy,1),human_number(p.value->common.boundary_position,6)});
        }
        h.end_table();
        h.paragraph("Locations belong to the receiver, not transmitters. Attribution uses the recorded tile-end fix; cells can be affected by GPS drift, missing fixes and movement during a tile. Coordinate rounding and cell grouping are not anonymization. The original saved fixes remain unchanged.");
    }

    h.heading("5. LoRa waveform and decode evidence");
    h.paragraph(c.schema<5?"Waveform discovery records are unavailable in this legacy schema. This does not mean no LoRa was present.":
        std::to_string(wave_count)+" waveform observations intersect the frequency/time/area selection. Counts are observations, not guaranteed unique packets or device identities.");
    h.table({"Inferred BW kHz","SF","Observations","Observed center range MHz","Partial-range observations","Ambiguous associations"});
    for(const auto& [key,g]:waves) h.row({human_number(double(key.first)/1000,1),std::to_string(key.second),std::to_string(g.count),
        human_number(g.low/1e6,6)+" – "+human_number(g.high/1e6,6),std::to_string(g.partial),std::to_string(g.ambiguous)});
    h.end_table();
    h.paragraph("Selected legacy receptions: "+std::to_string(receptions)+"; eligible authorized decodes: "+std::to_string(decoded)+"; payload CRC failures: "+std::to_string(bad_crc)+
        ". CRC failures are not proof of a collision. No received message text, sender identifier, key or sender-reported position is included in this report. Authorized-content CSV remains a separate explicit export.");
    h.paragraph("A LoRa waveform does not identify Meshtastic, MeshCore, LoRaWAN, a meter or an alarm. Automatic discovered-waveform payload dispatch, MeshCore decoding and recipient PKI are incomplete or unsupported. The configured legacy decoder does not enumerate all traffic in the survey span; unsuccessful decoding does not make traffic non-mesh.");

    h.heading("6. Measurement setup and data quality");
    const auto& config=c.summary.config;const auto& d=c.summary.discovery;
    h.table({"Item","Recorded value / scope"});
    h.row({"Receiver source",receiver_source_name(config)});
    h.row({"Nominal center / Offset",human_number(double(config.center_hz)/1e6,6)+" MHz / "+std::to_string(config.tuning_offset_hz)+" Hz"});
    h.row({"Sample rate / FFT / Hann ENBW",std::to_string(config.sample_rate)+" samples/s / "+std::to_string(c.summary.spectrum_fft_size)+" / "+human_number(c.summary.spectrum_enbw_hz)+" Hz"});
    h.row({"Recorded bin spacing",human_number(frequency.width)+" Hz"});
    h.row({"Activity threshold",human_number(config.activity_threshold_dbfs,2)+" dBFS per bin"});
    if(!config.synthetic&&config.hardware_receiver==HardwareReceiver::RtlSdr)
        h.row({"RTL-SDR tuner gain",config.rtl_auto_gain?"Automatic; sensitivity varies and no fixed gain is reported":
            human_number(config.rtl_gain_tenths_db/10.0,1)+" dB (applied manual gain)"});
    else h.row({"LNA / VGA / RF amplifier",std::to_string(config.lna_gain)+" / "+std::to_string(config.vga_gain)+" dB / "+(config.amplifier?"on":"off")});
    h.row({"Maximum retained power support in selection",human_number(support,6)+" s"});
    h.row({"Selected quality flags",quality_name(quality)});
    h.row({"Input drops / clipped samples (whole session)",std::to_string(c.summary.dropped_samples)+" / "+std::to_string(c.summary.clipped_samples)});
    h.row({"Energy events (whole session; not packets)",std::to_string(c.summary.spectrum_events)});
    h.row({"Discovery state (whole session)",c.schema<5?"Unavailable":!d.enabled?"Disabled":d.failed?"Failed":d.finished?"Finished":"Unfinished"});
    h.row({"Discovery rejected / abandoned input samples (whole session)",std::to_string(d.rejected_input_samples)+" / "+std::to_string(d.abandoned_input_samples)});
    h.row({"Discovery queue drops / resets / result overflows (whole session)",std::to_string(d.source_queue_drops)+" / "+std::to_string(d.stream_resets)+" / "+std::to_string(d.result_overflows)});
    h.end_table();
    h.paragraph("Power is uncalibrated received digital power (dBFS), not antenna-port dBm, field strength or transmitter watts. Strong received signals can come from nearby low-power radios. Distance, antenna orientation/gain, obstructions, fading and receiver settings prevent identifying a one-watt radio or excessive transmit power from received strength alone. Relative comparisons require consistent setup and no overload.");
    h.paragraph("Activity uses retained FFT-frame masks; no gap is filled as quiet. Compact power blocks may span up to about one second and can cross a narrower query. Peak values need not coincide in time. The recorded threshold cannot be changed retrospectively from this data. Calibration, antenna/cable response, analog overload, sensitivity and upstream USB loss are not fully established. Zero clipped samples does not prove linear analog operation.");
    if(d.failed||d.rejected_input_samples||d.abandoned_input_samples||d.stream_resets||d.result_overflows||d.gap_overflows)
        h.paragraph("Discovery reported failure, loss or discontinuity during this session. Its coverage is separate from spectrum coverage; do not interpret missing waveform observations as absent LoRa traffic.");

    h.heading("7. Interpretation and next survey work");
    h.paragraph("Use this report to compare measured activity across the supplied range and revisit busy areas. Before proposing a regional channel, repeat visits at different times and days, use consistent receiver/antenna settings, examine the entire intended channel width and adjacent activity, and compare several representative locations. A short route samples places at different times; it cannot separate time variation from geographic variation by itself.");
    h.paragraph("Not established by this report: exhaustive emitter/protocol inventory; packet collision rate; calibrated occupied/emission bandwidth; transmitter output power or model; an uncertainty budget or statistical confidence interval; rolling busy-hour/channel-access statistics; cross-session regional coverage; interference-free operation or FCC compliance. Low observed occupancy is evidence for further investigation, not a channel recommendation.");
    h.paragraph("Method reference: ITU-R SM.2256-2 (June 2026), spectrum occupancy measurements and evaluation. This report records its own detector and coverage definitions; it does not claim conformance to that recommendation family.");
    h.raw("<footer>Local, deterministic analysis of retained measurements. No cloud or AI service was contacted. Print from your browser to save a PDF. Expand the frequency table first if you want it included in the printout.</footer></body></html>\n");
}
