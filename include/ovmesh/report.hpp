// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ovmesh/engine.hpp"

namespace ovmesh {
class SessionStore;
enum class ReportKind {
    FrequencySummary, TimeSummary, GeographicSummary,
    Waveforms, ReceiverTrack, AuthorizedContent,
    Analysis = 7 // Value 6 is reserved by the desktop's detailed-archive selector.
};
struct ReportOptions {
    ReportKind kind = ReportKind::FrequencySummary;
    SurveyQuery query;
    ExportOptions privacy;
    double geographic_cell_m = 100;
};
const char* report_kind_name(ReportKind);
// Exclusive-create, private CSV or standalone analysis HTML, consistent saved-data snapshot. This never
// changes the source survey. Limits fail explicitly instead of truncating rows.
void export_survey_report(const SessionStore&, const std::string& path,
                          const ReportOptions&);
}
