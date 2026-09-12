# Concentrator measurements and saved reports

RAK5146 USB/LBT reception uses a separate measurement path from HackRF and RTL-SDR. The SX1261 scanner supplies sampled RSSI histograms. The service modem supplies packet records at explicitly configured receive frequencies, bandwidths and spreading factors. Neither path supplies SDR IQ or FFT bins.

## What a completed scan means

Each scan retains 2,000 histogram samples at one nominal center frequency, using a nominal 234,300 Hz receiver filter. The vendor RSSI offset is -11 dB. Reported dBm is **uncalibrated**. The scanner does not identify which protocol produced the energy, and its filter bandwidth is not a measured transmitter bandwidth.

The 33 counters retain HAL histogram order. Bin 0 is the upper tail starting at -11 dBm. Bins 1 through 31 have lower edges `-11 - 4 * bin_index` dBm and four-decibel intervals. Bin 32 is the underflow below -135 dBm. These limits use the recorded vendor offset; they are not field calibration results.

The initial display/report threshold is **-87 dBm**, an exact histogram boundary. Sample exceedance is the sum of bins whose lower edge is at least -87 dBm, divided by all 2,000 samples. The frequency summary combines those counts across scans, weighted by sample count. It does not average scan percentages without their denominators. A nominal threshold such as -90 dBm cannot be resolved inside one four-decibel bin; reporting -87 makes the quantization explicit.

Sample exceedance is not continuous channel occupancy, packet airtime, collision probability or a channel-utilization reading from a Meshtastic device. Scan transactions cover different frequencies at different times. Adjacent filter passbands can overlap, and their sample counts must not be added as independent band coverage. Frequencies and times without scans remain unassessed.

## Time and receiver position

The host records UTC and session-elapsed start/end bounds for the scan transaction. Those bounds include control/transport latency and do not establish exact RF sample dwell or the timestamp of each histogram sample. Completed scans on the same board must have non-overlapping host intervals. Different boards may scan simultaneously.

A valid receiver fix is associated at host scan completion. Original coordinate precision, UTC, monotonic time, source description, satellite count and optional HDOP/altitude are preserved. A missing fix stays missing. The fix locates the receiver at that endpoint; it is not a transmitter location, interpolated route or a claim that the receiver remained stationary through all scans.

Packet records retain host reception time separately from the board's wrapping microsecond timestamp. Board clocks are not synchronized to each other or to GPS. Packet RSSI uses the vendor's nominal uncalibrated scale. Packet bandwidth is explicitly the **configured service-modem bandwidth**, not a measured signal width. Only RF/GPS metadata and optional envelope classification evidence are retained; semantic message contents, node identities, packet IDs and routes are excluded. No channel key, undecoded payload or ciphertext is saved.

## Schema and privacy boundary

Concentrator recordings use application schema **7**, session source discriminator **3**, and concentrator extension version **1**. Existing SDR recordings keep schemas 5 (detailed) or 6 (compact); historical schemas 1–6 are read without migration. Earlier readers reject the new format instead of interpreting a concentrator as an SDR.

The extension consists of:

| Table | Retained information |
| --- | --- |
| `concentrator_setup` | Extension version, scan enable/step/target count, authorized decode enable, fixed method/coverage descriptions, optional operator provenance |
| `concentrator_profiles` | Board index, packet enable, frequency, configured bandwidth, SF and sync word |
| `concentrator_scans` | Increasing scan ID, board index, nominal frequency/filter, offset, host intervals, sample total, all 33 counters and optional receiver-fix reference |
| `concentrator_packets` | Reception reference, board index, nominal RSSI and board-local timestamp |
| `concentrator_health` | Board index, fixed ready-state vocabulary, scan/sample/reception/CRC counters and readiness/update times |

Histogram storage is exactly 132 bytes: 33 unsigned 32-bit counters, little-endian, with no compression or omitted bins. The counter sum must equal both the retained sample total and the configured count of 2,000. USB paths, USB identities, subprocess error text and device serial numbers are excluded from these tables. Receiver fixes use the shared normalized positions table; repeated identical fixes are stored once.

Schema 7 keeps the common database layout for archive compatibility, but FFT tile/power/event/window tables and FFT metrology must be empty. SDR sample rate is zero; SDR input, measurement and delivered-sample counters stay zero. FFT analysis rejects this source with an explanation. It does not manufacture spectrum bins, busy seconds, a waterfall or quiet intervals.

Reads validate the extension's exact table definitions, configuration bounds, histogram shape/counts, board references, packet-profile agreement, supported RSSI method, intervals and GPS references. Streaming export validates all selected record types beyond bounded UI lists. The historical UI shows only the latest scan per board/frequency, bounded at 2,080 entries, while the saved scan history and streaming exports retain all scans.

## Reports and selection semantics

- **Frequency summary:** sample-count-weighted histogram/exceedance per board and scan center. A shown position is the last included receiver fix, not a location for the entire summary.
- **Time and geographic CSV:** one row per selected completed scan, preserving its histogram and actual host interval. This initial pathway does not interpolate scan time buckets or spatial cells. GPS inclusion is required for geographic reports.
- **Analysis HTML:** a local standalone frequency summary with configured packet profiles, sample counts, host intervals, optional last receiver fixes, and explicit measurement limits. No external map, scripts or network resources are loaded.
- **Detailed CSV / GeoJSON:** fixed method/configuration, health and complete scan histograms; packet records include receiver RSSI, configured-bandwidth and hardware-clock provenance. Unlocated scans are retained as GeoJSON features with null geometry.
- **Receiver track CSV:** the shared export privacy controls apply. Software waveform-discovery reports are unavailable for this source.

Time/frequency selections that intersect only part of a scan retain the **whole** histogram and mark it as a boundary scan. No fraction of a histogram is invented for a narrower frequency or time interval. Geographic selection uses the associated receiver endpoint. Reports with no matching scans describe absence of measurements, not proven quiet airwaves. Notes and receiver coordinates are exported only when their respective controls are enabled.

`tests/test_concentrator_storage.cpp` uses synthetic measurements to check roundtrips, missing/full-precision GPS, private identifier exclusion, malformed-record rejection, truthful report labels and legacy-path separation. These tests do not establish field RF calibration or continuous survey coverage.
