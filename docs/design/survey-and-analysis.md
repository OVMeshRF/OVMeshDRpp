# Survey methodology and analysis

Status: methodology for the implemented spectrum measurement and retrospective reporting workflow. The FFT occupancy sections apply to HackRF/RTL-SDR. RAK5146 follows the separate [sampled concentrator measurement contract](concentrator-measurements.md); the shared planning, provenance and privacy principles still apply. Later recommendations and protocol discovery remain separate.

## Session preparation

Record purpose, receiver location or GPS source, antenna type/placement/orientation, feedline/filter/attenuator configuration, gain, frequency span, sample rate, decoder profiles, and method versions. Make configuration changes visible as new observation intervals.

A stationary survey observes temporal patterns at one receiver location. A mobile survey links observations to valid receiver positions along a route. Neither alone proves transmitter location or overall network coverage.

## Coverage-aware statistics

Maintain valid observation time per frequency bin and separately per active decoder/profile. Retunes, missing samples, unsupported modes, and processing overload are gaps. Avoid recording gaps as zero power or zero activity.

An occupancy statistic is the duration satisfying a defined activity criterion divided by the valid observed duration for the same frequency/time region. Store the criterion, threshold, resolution, aggregation method, and coverage with the value.

Keep raw any-bin occupancy visible. A continuously active center bin can make a wide selection 100% busy without making each frequency occupied. The optional receiver-center comparison separately unions activity outside bins centered within ±2 bin widths of the nominal center, exposing the actual excluded bin edges. Time/GPS views default to this comparison when the guard intersects the selection. This heuristic can exclude real signals; it does not prove an internal artifact, assess the guarded frequencies or declare them quiet. If every selected bin is guarded, outside-center occupancy is unavailable. The frequency chart and saved/exported measurements retain the original values.

Report detected-event rates separately from decoded-packet rates and unique-message counts. Overlapping transmissions can share time/frequency area. Do not sum category airtimes into a supposed total without accounting for overlap.

## Recorded range analysis and later candidate comparisons

First inspect the recorded range by arbitrary frequency bounds, time and geographic area, without needing to nominate a candidate. Observed energy envelopes describe generic activity, not assumed modem bandwidth or device identity. After collection, compare candidate center-frequency and bandwidth combinations over their full spectral footprint. Include adjacent activity and required edge margins, not just the center bin.

Proposed report dimensions:

- Total observed activity and background/noise distribution.
- Authorized decoded mesh activity and likely mesh activity without decoded content.
- Supported other-protocol evidence and unknown activity.
- Burst durations, recurrence patterns, and busy periods.
- Valid listening time, decoder coverage, measurement settings, and uncertainty.
- Variation across locations and matched time windows.

Avoid a single unexplained best-channel score. Any future ranking must publish its inputs, weights, exclusions, and insufficient-evidence conditions. A low observed occupancy is not a promise of future quiet conditions.

No passive packet-delivery-ratio claim is possible without a known transmission denominator. No observed signal at the survey receiver does not prove that another site will have no interference. Network migration suitability also needs separately authorized link and device/configuration assessment.

## Timing and GPS

Preserve a monotonic time basis for durations and UTC mapping for reports, including uncertainty and clock adjustments. USB arrival time is not assumed to be an exact RF timestamp. A GPS position/time feed does not automatically discipline the SDR sample clock.

Associate observations with fixes using a documented matching/interpolation policy, bounded fix age, and validity/accuracy information. Never silently extrapolate a stale fix. Manual stationary coordinates must be labeled as manual.

Mobile comparisons need enough valid observation per location/time area; do not compare a brief drive-through directly to hours at a fixed site without exposing that difference.

Each retrospective query uses a stable read snapshot. Resolve its elapsed interval against recorded tile/gap bounds and cover that entire interval with at most the configured number of displayed time buckets, increasing the requested bucket width when necessary. Report the effective width; do not silently show only the first 2000 buckets. Aggregated busy/observed durations retain all matching measurements and do not count gaps as quiet. Coarser position buckets show their last associated fix and may span movement; full interval coverage is not continuous route reconstruction. The event table has a separate row limit.

The Survey & GPS and Analyze geographic plots share a display-only choice. **Stationary / position cloud** is the default: draw the individual reported fixes without route lines and a neutral mean-position reference. This does not infer stationary operation, improve the fixes, establish a surveyed reference, or turn the cloud spread into a confidence radius. **Mobile / connected fixes** adds estimated path connections, broken at missing or time-discontinuous observations; these are not verified travel. Valid fixes and satellite count do not establish accuracy. Available HDOP is shown as dimensionless satellite geometry, not error in metres.

Use equal physical metre scaling on both axes, latitude-aware longitude distances, dateline wrapping and a metric/feet distance bar. Keep the shorter displayed dimension at least 100 metres to prevent minor position variation from filling the chart. Dateline wrapping applies to rendering only; current geographic filter rectangles do not wrap. Analyze point colors represent the selected raw or outside-center RF busy-time metric, separate from position quality; unavailable comparisons are labeled accordingly. Plot modes and reference markers do not smooth or snap observations, modify stored fixes or RF association, or change export/retention behavior. Synthetic scale and rendering validation is recorded separately from any field positioning accuracy evidence.

## Sampled scanning and future extensions

Continuous reception remains the SDR baseline. The RAK auxiliary scanner now visits configured frequencies sequentially and retains all histogram counters, host operation intervals and eligible receiver-fix associations. Its sample-exceedance fraction cannot be substituted for observed/busy time, and the host interval is not exact RF dwell. Two boards split scan centers while their service modems receive only their configured packet profiles. Missing visits and intervals between visits remain unknown.

Future wider SDR scanning or concentrator profile scheduling must retain visit/dwell timing and frequency gaps. Packet-decoding dwell must allow the targeted frame duration; a rapid energy sweep cannot promise complete packets. Consider sampling bias from periodic visits and periodic emitters. Do not pool uncalibrated RAK RSSI and SDR dBFS into one receiver-independent congestion score.

## RF planning and regulatory context

Nominal modem BW, estimated occupied BW, and a regulatory measurement such as 6 dB BW are different fields/methods.

The app produces observations for planning. Device authorization, operating mode, power, antenna, emissions, and applicable rules require their own assessment. It must not mark a preset FCC compliant based on bandwidth alone.
