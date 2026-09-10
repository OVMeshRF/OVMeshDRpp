# OVMeshDR++ user guide

Use **Settings > About & licenses** to read copyright notices, the GNU GPL, third-party credits and component licenses offline. The CLI equivalent is `ovmesh-cli --licenses`; it opens no receiver or GPS. Source availability does not imply a supported binary release.

Version 0.4.0 is experimental survey software. New recordings default to Compact schema 6, with Detailed schema 5 available; existing recordings are unchanged. Start with the [build instructions](operations/deployment.md) and [current capabilities and limitations](engineering/implementation-status.md). Receiver calibration, exhaustive protocol detection and mobile survey accuracy remain unqualified.

In **Settings > Detection & decoding**, **Spectrum + LoRa** enables the configured discovery/decoding functions. New desktop settings have **Discover LoRa waveforms** on and **Decode authorized messages** armed; operator opt-outs are remembered. **Spectrum only** suppresses both while retaining their setup. Discovery searches the supplied range for 125/250/500 kHz LoRa preambles at SF7–12 without known candidate frequencies or keys. **Detected signals** displays inferred center frequency, modem bandwidth, SF and observation time. An inferred 250 kHz waveform can have a different energy-envelope width. A waveform match does not identify Meshtastic/MeshCore, decode a payload or establish packet airtime. Other widths/SFs remain outside this classifier's hypotheses.

Discovery is experimental and may miss signals or fall behind the incoming sample stream. Its processing-loss counters are separate from RF occupancy coverage; a healthy waterfall does not establish complete discovery. Supported workload depends on range, sample rate, traffic and host capacity. See [validation scope](engineering/quality-and-validation.md). Preference migration preserves an explicit stored discovery opt-out.

The **Energy groups / supporting detail** list groups repeated above-threshold activity across nearby frequencies and times. **LoRa bandwidth/SF: not identified by this energy view.** **Width kHz**, between **Upper MHz** and **Duration ms**, is this provisional group's energy span, not detected LoRa bandwidth or a preset. Groups can split one transmission or combine unrelated activity. **Duration ms** includes short grouping gaps, while **Active ms** excludes those gaps. **Tile peak upper bound dBFS** uses bin peaks from whole contributing tiles; those peaks may occur outside the group's active times or a selected partial tile. Do not sum these rows to obtain channel utilization; use the frequency occupancy analysis.

**Pause result list** freezes only the rows; acquisition and recording continue. Otherwise the list refreshes once per second. **Show raw fragments** exposes the original fine-grained energy components, including brief activity insufficient to form a candidate band. This does not discard their measurements. Center-region activity stays separate, and ambiguous/segmented groups retain visible labels. Saved surveys reconstruct groups in Analyze & export from the selected tile data; exports retain the original measurements rather than substituting candidate counts for packet counts.

OVMeshDR++ runs locally on the desktop, with no cloud account/deployment, remote receiver, network map or automatic upload. Spectrum recording and regional analysis remain the priority under [the current capability summary](engineering/implementation-status.md). Arming existing decoding does not close the automatic full-range dispatch gap. Optional legacy Meshtastic profiles remain available; MeshCore and recipient private-key reception remain disabled. Explicit CLI/demo modes retain their independent configuration.

An ordinary launch opens a fresh empty session with remembered setup; historical results are loaded only through **Open...**. RF is stopped. Recording and GPS default on; a uniquely recognized or remembered selected GPS can connect automatically when the ordinary receiver source is hardware. Missing or ambiguous GPS identity requires selection. No arbitrary serial ports are probed. The recording folder, GPS identity/baud, receiver/source settings, Offset and display/detection choices persist. Keys, coordinates, decoded content and export privacy selections do not.

## Main controls and sessions

**Live survey** contains the spectrum/waterfall plus **Detected signals**, **Decoded messages** and **Energy details**. **Analyze** contains linked **By frequency**, **Over time** and **By location** views. Receiver, center, Offset, span, sample rate, range, receiver-specific gain and Start/Stop remain on the main screen. HackRF offers LNA/VGA gain and RF amplifier controls; RTL-SDR offers tuner gain and automatic gain mode. Compact recording/GPS status opens the relevant settings; **Details** exposes health and measurement explanations.

**Settings** groups configuration into Detection & decoding, GPS & location, Recording, Display, Measurement & equipment, and About & licenses. Acquisition settings apply while stopped. Display offers system UI fonts with numeric monospace or All monospace, using local fonts and a built-in fallback without a download.

| Action | Effect |
| --- | --- |
| **New session** | Stops and finalizes the current recording, then shows an empty workspace without starting RF. Prior files remain intact. Memory-only results require explicit discard confirmation; save verification failures keep the current session available. |
| **Open...** | Chooses an existing survey and opens it read-only. The historical badge distinguishes it from live reception. Existing ordinary receiver preferences are not replaced by recorded settings. |
| **Save session** | Requests a durable recording checkpoint and confirms success after the worker commits it. It does not reconstruct memory-only history or stop an active capture. |
| **... > Save a copy...** | Chooses a new `.sqlite` file and copies a consistent saved snapshot, including retained records beyond on-screen limits. An active-session copy retains an incomplete/checkpoint marker. The original remains the recording destination. |
| **Stop reception** | Stops acquisition and retains results on screen. A later **Start new session** creates another recording; it does not resume/append to the previous file. |
| **Export report...** | Creates a separately selected, privacy-controlled report from saved measurements. It is not the same operation as saving the session. |

Save/Copy/Open and session finalization show progress and errors. Do not close or replace a session while its operation is pending. Existing files are never overwritten. New-file or disk failures require correction; an incomplete newly created copy may remain for inspection, but the original recording is preserved.

Saving must be enabled **before** Start. A deliberate memory-only session is visibly marked **Not recording**; its full history cannot later be recovered by Save, Copy or Export. Its live frequency summary still works. Requested saved starts fail when the recording folder is unavailable instead of silently disabling saving. Ordinary starts use fresh filenames in the remembered/default **Surveys** folder.

## Start with the synthetic receiver

1. Select **Synthetic / demo** in the receiver sidebar. This source generates RF samples in memory and does not open a radio or transmit. Switching to it disconnects an automatically connected GPS. To launch without any ordinary profile/GPS startup behavior, use explicit `--demo` rather than an ordinary launch.
2. In **Settings > Recording**, set the Title, recording choice and Compact/Detailed mode. **Browse recording location...** chooses an existing local folder and new filename; `.sqlite` is added if omitted. A chosen filename is used once, while its folder is remembered. Keep operational data outside the source repository.
3. Review center/span/rate/gains. **Settings > Detection & decoding** selects Spectrum + LoRa or Spectrum only. Pure spectrum measurement needs no known channel frequency, modem setting or key. Optional antenna/receiver descriptions and survey notes are in **Settings > Measurement & equipment**.
4. Choose **Start demo**. Watch the spectrum/waterfall and compact recording status. **Details** contains input availability, measurement duty, clipping, gaps and decoder/discovery health.
5. Choose **Stop reception** to retain results, or **New session** to finalize and clear them. Stop before report export or opening a historical survey.

Passive checks, explicit `--demo`, timed/managed launches and `--view-survey` do not load/change ordinary desktop preferences or automatically connect GPS. Explicit CLI GPS options remain separately authorized device actions. A command-line synthetic recording needs `--session`. Optional synthetic decoding uses an artificial fixture key only for its generated source; it never installs a default live-channel key or proves compatibility with physical transmitters.

## Choose recording detail

In **Settings > Recording**, choose **Compact** for ordinary surveys. It preserves fine frequency/activity timing and original GPS fixes while saving roughly one-second power means and peaks. **Detailed** preserves approximately 20 ms power arrays and uses more disk. Choose while stopped; the ordinary profile remembers the mode. Existing recordings are unchanged. The CLI equivalent is `--detailed-recording`; new recordings otherwise default to compact.

Compact mode preserves the information needed to compare busy time at different frequencies and places. It deliberately gives up subsecond changes in average power. Reports label that coarser power support. Neither mode records raw IQ or undecoded packets, and neither can reconstruct activity at a different threshold later.

## Configure a spectrum receiver

Select **HackRF One / USB** or **RTL-SDR / USB** in the sidebar while stopped. The driver-availability message describes build capability; it does not prove that a device is present or usable. An optional serial number selects a known receiver; no discovery scan is required. See [hardware compatibility](operations/hardware-compatibility.md) for receiver and platform qualification limits.

Set **Center frequency / MHz**, **Survey span / MHz** and **Sample rate / MS/s**. HackRF offers 8, 10, 12, 16 or 20 MS/s. RTL-SDR offers 1 or 2 MS/s, with maximum spectrum-only spans of 0.8 or 1.6 MHz respectively. RTL LoRa discovery requires 2 MS/s and a span no wider than 1.5 MHz. Complete FFT bins within the supplied usable range contribute measurements regardless of protocol. The implementation limits the spectrum span to 80% of the sample rate as a conservative configuration rule; this is **not** a measured guarantee of a clean passband. A roughly 16 MHz HackRF span remains a hardware/performance target to validate. Neither adapter sweeps frequencies outside the configured range. See [RTL-SDR setup](operations/rtl-sdr.md) for tuner gain and USB requirements.

**Offset**, with **Hz** beside its input, applies a signed receiver correction limited to ±100,000 Hz. A positive value raises the hardware tune command, matching SDR++'s manual offset convention. For example, center 907.500000 MHz with +900 Hz commands 907.500900 MHz. The spectrum and profile labels retain their nominal frequencies. Change it only while stopped; ordinary desktop launches remember the value. The default is **0 Hz**, including when reading older preferences that did not store an offset; +900 Hz is an example, not a product default. The saved value belongs to this app profile, not a per-device calibration database, so verify it when changing receivers. This is an operator-supplied correction, not an independent calibration certificate or a power calibration. It is recorded with each new survey and in exports; changing the preference does not rewrite earlier surveys.

Choose **Start reception** after reviewing the main receiver settings. This deliberate operator action opens the selected receive-only SDR and, if enabled, connects the selected GPS. Ordinary startup does not start RF. The app has no RF-transmit path. An assistant or support technician still needs applicable scoped device authorization; the existence of a Start button or product default does not grant it.

**Activity threshold** in **Settings > Measurement & equipment** is a fixed session threshold in uncalibrated dBFS per FFT bin; change it while stopped. Background estimates do not adapt the threshold. Zero busy time cannot rule out signals below it. CLI `--activity-threshold-dbfs N` accepts a finite -140 through 0 dBFS/bin in a demo or explicit hardware receive mode; the default remains -55 dBFS/bin. The applied threshold is saved, and activity masks cannot later be reinterpreted at a lower threshold to recover weak signals. Record a new session for a different threshold. Descriptions/notes are provenance, not calibration evidence.

## Optional legacy protocol reception

In **Settings > Detection & decoding**, select Spectrum + LoRa and enable **Decode authorized messages**. **Advanced / Legacy decode profiles** holds up to four profiles with frequency, 125/250/500 kHz bandwidth, SF7–SF12 and CR4/5–CR4/8. The entire profile footprint must fit the survey span/sample-rate constraints. Default arming does not imply keys are configured, every profile is supported, or discovered signals automatically feed the decoder. Analysis widths do not configure a modem.

Explicit legacy CLI modes retain their configured profiles. Add `--spectrum-only` to a headless or desktop hardware launch to pause decoding; this flag cannot be combined with decoder lanes or stdin keys. Explicit `--demo --lane ...` also retains the requested profile. The following key and protocol behavior is unchanged.

For each authorized channel:

1. Choose **Configure keys...** in Settings > Detection & decoding and select one of the 16 independent **Key records**. These slots are not RF decoder profiles.
2. Choose **Key only / any channel name** when the authorized key should apply without knowing a channel name. Supply a non-secret record label. Alternatively choose **Named channel** to narrow attempts using its exact, case-sensitive name (up to 32 bytes).
3. Enter the AES key as 32/64 hexadecimal characters or padded standard Base64 for 16/32 bytes. `AQ==` explicitly selects the published public key. No public key is tried implicitly.
4. Confirm authorization for the selected scope, then **Set / replace selected key record**. The key input is cleared; the actual key remains only in process memory.

Every completed live frame is evaluated against the entire configured keyring. A key is not tied to a frequency, preset, or RF-profile index. Changing/removing an RF profile does not erase or rebind keys. Invalid replacement input preserves the previous record. Exact duplicate eligible key material is one cryptographic candidate; if several aliases apply, retained provenance is `configured-keyring`, not an invented unique channel identity. Different keys producing competing plausible results remain ambiguous and produce no content.

Key records cannot change while reception runs. **Clear all in-memory keys** stops/drains reception before clearing keys; it does not delete already authorized saved content. There is no persistent key store, and records must be reentered after restarting. An explicit local launcher can use `--survey-key-stdin SLOT,LABEL` for one key-only record through redirected stdin; the legacy `--channel-key-stdin LANE,CHANNEL` still configures a named record. Choose one stdin option. Keys are never argument values or environment variables.

**Automatic payload-decoder dispatch is not implemented yet.** Optional waveform discovery can infer supported LoRa frequency/BW/SF without profiles, but does not pass those observations to the payload decoder. The current RF payload decoder still searches only selected frequency/BW/SF profiles. The supplied span covers RF measurements; it is not exhaustive mesh discovery coverage. The remaining requirement is tracked in [the current protocol limitations](engineering/implementation-status.md).

## Prepare a timed desktop test

The desktop option `--prepare-desktop-hackrf --spectrum-only --seconds 180` loads the requested HackRF settings and leaves the radio closed. Use `--prepare-desktop-rtlsdr` instead for RTL-SDR. Review the survey settings while the **SETUP ONLY** banner is visible. A spectrum-only survey does not require keys. No reception deadline runs during setup. **Start reception** begins reception and its independent three-minute stop timer; the window closes after the deadline. Stopping early does not reset the timer or permit a replacement session. Close the idle window to cancel without opening the radio. See [deployment](operations/deployment.md) for the two distinct launch modes.

## Keep reception and results open until stopped

Use the desktop option `--until-stopped` instead of `--seconds` when the operator will decide when testing is complete. It works with prepared HackRF or RTL-SDR setup, an explicitly authorized direct desktop start, or `--demo` for a synthetic check. It cannot be combined with `--seconds`, `--ui-smoke`, or a headless mode. Prepared setup still requires the operator to choose **Start reception** before opening the radio.

Enable **Save survey measurements and authorized content** and choose a new session path **before starting**, or supply that new path through `--session`. In this mode there is no automatic receive deadline or window close. **Stop reception** stops acquisition and leaves the window and results available for inspection. On macOS/Linux, `SIGINT` sent to this app's reported process ID also requests a stop; the GUI event loop handles that request, so it is not an independent watchdog. On Windows, use **Stop reception**. Closing the window also stops the receiver. The timed `--seconds` workflow retains its existing automatic stop-and-close behavior.

Spectrum-only sessions show the most recent 200 completed energy events. When optional legacy decoding is enabled, **Legacy receptions** keeps the most recent 512 receptions; select a row to inspect authorized decoded details. In either mode, a saved session retains the approved records beyond that display limit. Stop reception before exporting. This managed window keeps the current session selected until it closes; reopen a saved survey in a subsequent ordinary desktop launch. Saving is not retroactive, and adding a key later cannot recover discarded undecoded traffic.

## Read the live display

**Live survey** shows the spectrum, transient waterfall and result tabs. **Freeze display** affects only the displayed waterfall; acquisition/recording continue. **Energy details** contains grouped activity and optional **Show raw fragments**. A raw event joins adjacent above-threshold bins across consecutive FFT frames. Its width is a frequency envelope, not a modem bandwidth, packet or device. Overlapping signals can merge, chirps can fragment, and persistent/overflowing events can be truncated. Possible DC artifact labels identify suspicion, not confirmed external traffic.

The small **Session disk** footer shows the current database plus SQLite sidecar file sizes, refreshed about every five seconds. KiB/MiB/GiB use multiples of 1,024. Hover for the database/sidecar breakdown. It includes the write-ahead log while recording; it excludes exports, images, other sessions and filesystem allocation overhead. A checkpoint may reduce it. Memory-only sessions are labeled; an unreadable/missing file shows Unavailable instead of a misleading zero.

**Decoded messages** shows **Received at (UTC)**; **Detected signals** shows **Observed at (UTC)**. Both include the full date and time through milliseconds, including when opening a saved survey. Hover over a timestamp for elapsed session seconds and its timing meaning. Waveform time refers to observed delimiter evidence; reception time is host-estimated, not a sender's clock. Millisecond formatting does not imply GPS-synchronized accuracy. Unavailable means the stored timestamp is missing or invalid.

| Term or indicator | Meaning in spectrum surveying |
|---|---|
| INPUT AVAILABILITY | Delivered sample time divided by host elapsed time; upstream loss may remain unknown. |
| MEASUREMENT DUTY | Complete FFT-window time divided by delivered input time. Version 0.2 processes contiguous complete 4096-sample FFTs from accepted samples without deliberately skipping windows. Drops and partial FFT tails remain gaps. |
| ENERGY EVENTS | Completed connected above-threshold energy envelopes; not packets, emitters or unique transmissions. |
| CLIPPED SAMPLES | ADC-near-limit samples indicate potentially distorted measurements. No flag does not prove the analog front end was free of overload. |
| FFT spacing / window ENBW | Bin spacing is sample rate / 4096; periodic-Hann effective noise bandwidth is 1.5 times that spacing. Frequency edges have this finite resolution. |
| dBFS | Uncalibrated digital power relative to full scale, not dBm, field strength or regulatory certification. Per-bin and integrated-region powers have different bandwidths. |
| Estimated background per bin | A lower spectral percentile estimate, not a calibrated or independently measured noise floor. Broad activity can contaminate it; it does not change the fixed threshold. |

Schema 1–3 surveys used 25-percent time sampling and a different power normalization. Their original coverage and level meanings remain attached to those records; do not compare raw thresholds or interpret them as version-0.2 continuous accepted-sample measurements. Even schema 4 cannot account for unknown upstream loss, samples outside the supplied range or signals below the fixed threshold. See the [measurement definitions](design/spectrum-measurement-record.md).

## Generate a readable survey analysis

In **Analyze**, choose **Generate analysis report...**. It uses the latest completed frequency/time/area selection; **Use full session for report** resets the scope. Stop reception, choose a new `.html` destination with the file browser, review GPS/description inclusion and press **Write report**. Generation runs in the background. Open the resulting local file in a browser; use its print function to save a PDF. Expand the complete frequency table before printing if you want those rows included.

The report explains measured occupancy, per-frequency activity, busiest time groups, optional receiver-location groups, inferred LoRa widths/SF, legacy decode evidence and quality/coverage limits. It contains no message text, sender identifiers or keys. Coordinates and free-form notes/descriptions are excluded by default. A limited top-ten time/top-twenty location list is labeled; existing CSV reports provide the full respective data. No cloud service or new package is used. See the [capability/gap review](engineering/rf-survey-gap-analysis.md) for what these measurements can and cannot establish.

## Save a waveform image

Choose **Capture PNG...** beside the live plot, then select a new local `.png` destination in the existing in-app file browser. The app captures the visible spectrum/waterfall before opening the chooser and saves its axes, elapsed/UTC context and nominal tuning labels. It excludes GPS coordinates, private paths, decoded-message panels and other windows. Wait for completion or the displayed error; an existing file is never overwritten.

The PNG uses an owned uncompressed writer, so it can be larger than a compressed screenshot. Capture does not stop RF, change measurement data or save IQ; it is a visible plot image, not a session archive or replayable recording. The chooser and fonts add no external dependency. Use **Save session** or **Save a copy...** for retained survey history and **Export report...** for analysis tables.

## Optional legacy reception details

When legacy profiles are enabled, select **Decoded messages** in **Live survey**. Clicking the spectrum while stopped tunes the selected decoder profile and leaves the survey center unchanged. A running session must be stopped before tuning.

Select a reception row to open **Reception details**. The detail window separates classification, decode status, authentication, physical-layer checks, receiver position, and authorized schema fields. Supported fields can include text, node information, sender-reported position, telemetry, and routing fields. Unknown/unsupported traffic has no retained payload.

**Possible Meshtastic** means an explicitly configured channel-key attempt passed the channel/PHY checks and produced a valid Data envelope, but its application payload is unsupported. The retained evidence is limited to the envelope's port and whether a signature field was present; it includes no unsupported payload or message identifiers. **Likely Meshtastic** means a supported application payload also passed the decoder's schema checks and its authorized projection is available. Neither label authenticates the sender. A present signature is displayed as **not verified**; signature presence does not upgrade authentication.

Legacy **BW**, **SF** and **CR** describe the configured LoRa profile. **PHY CRC** checks frame integrity; it does not authenticate a sender. **Estimated SNR** is a receiver algorithm estimate. **Decoder processed time** is coverage for that profile, separate from spectrum measurement coverage and not proof of exhaustive packet detection.

Successful channel decryption and schema acceptance do **not** authenticate a Meshtastic sender. A reported origin can differ from the physical transmitter or relay. Packet counts count receptions; they are not a unique-message count or an end-to-end delivery ratio. A waterfall shape, frequency, or short packet alone cannot uniquely identify an alarm sensor, meter, LoRaWAN installation, or mesh device.

Routing and traceroute details show **request ID** and **reply ID** separately from the packet ID. These are reported correlation fields, not proof that a request was delivered. A Routing wrapper is identified as a request, reply, or error; a direct traceroute payload has no wrapper variant. An empty route request does not imply an acknowledgment, and a missing routing result must not be interpreted as success.

Forward and return routes are separate from forward and return SNR lists. Each list retains its own order and count; the app does not pair mismatched entries or fill missing ones. SNR is sender-reported, with wire values divided by four to display dB. Raw `-128` is the firmware's unknown-SNR marker, and route node `0xffffffff` represents an unknown hop. These markers remain unchanged in saved data. The displayed SNR list is separate from this receiver's estimated packet SNR.

## Fixed and mobile receiver position

In **Settings > GPS & location**, **Fixed position** accepts latitude/longitude and optional altitude. Apply a fixed receiver position only when stationary; it is labeled manually configured. Coordinates are not restored from ordinary preferences.

For a mobile receiver, use **Serial GPS** in Settings > GPS & location with automatic GPS enabled. On ordinary hardware-oriented startup, the app inventories OS device metadata and connects only the remembered selected device or a unique recognized GPS. Automatic recognition currently covers u-blox USB GNSS generations 5–8/M8; other known serial GPS receivers can be explicitly selected from the device list. Labels show names/paths without requiring manual path typing. Refresh after a USB change; inventory does not probe arbitrary serial devices for traffic.

The app rechecks GPS identity on real-survey Start. A missing or ambiguous selection blocks a GPS-enabled start until the operator selects the intended receiver or turns GPS off to survey without positions. A remembered identity never silently substitutes another device. Devices without a serial number may be identified by USB location, so moving hubs can require reselection. **Connect GPS now** supports manual reconnection while stopped; no RF reception is started by connecting GPS.

Supported baud rates are 4800, 9600, 19200, 38400, 57600, and 115200; the default is 9600. Status distinguishes disconnected, connected/waiting for a fix, valid, stale and read-error conditions. A serial connection alone is not a valid position. The parser accepts checksummed NMEA RMC/GGA sentences and rejects invalid or stale fixes. No GPSD or network location service is used. **Disconnect GPS** stops the current serial reader; it does not disable automatic connection on a later ordinary startup or real-survey start. **Clear receiver position** disconnects and clears positioning for future observations. Applying a fixed position disables automatic GPS; fixed coordinates are not restored after restarting the app.

The receiver-position plots in **Settings > GPS & location** and **Analyze > By location** are offline coordinate plots, not maps with downloaded basemaps. **Stationary / position cloud** shows reported fixes without route lines and a neutral mean-position reference; **Mobile / connected fixes** connects nearby observations in time. This is an operator display choice, not automatic motion detection. Missing/time-discontinuous fixes break lines; a line is an estimated path, not verified travel or a transmitter location. Sender-reported coordinates inside decoded messages are distinct.

Both plots use equal physical distance per pixel horizontally and vertically, account for latitude and longitude wrapping at the dateline, and show a distance bar in metric units and feet. The shorter displayed dimension covers at least 100 metres, so small position fluctuations do not fill the window. A stationary receiver can still report a cloud of different positions. Its spread and the mean reference are neither a confidence radius nor proof of movement or an accurate surveyed location. Hover a fix for available satellite count and HDOP; HDOP describes satellite geometry without units, not error in metres, and a valid fix does not establish positional accuracy.

Changing the view affects presentation only. It does not smooth or snap fixes, alter stored positions or their RF associations, or change queries, exports or retention.

Schema-4 measurement tiles save receiver fixes at their start and end when available. Current analysis associates a tile with its **end** fix and each displayed time bucket with its last recorded fix. A merged bucket may span movement; that one point does not describe every intervening location. Geographic filtering excludes measurements with no valid receiver position. Coverage gaps have no position and are omitted from geographic queries with an explicit applicability warning. For driving surveys, configure the application before travel and perform screen interaction while stationary or with a passenger.

An explicit local launcher may use `--gps-device ABSOLUTE_LOCAL_SERIAL_PATH --confirm-gps-access --gps-baud 9600` with a reception mode. The default baud is 9600. This opens only the specified GPS before starting reception; it does not scan ports. Help, saved analysis and export do not open a GPS or radio. GPS access is independent of SDR authorization.

## Analyze and export reports

**Analyze > By frequency** shows live full-range occupancy, including for memory-only sessions. Expand **Frequency measurements** for lower/upper edges, bin width, observed/busy seconds, busy percentage and mean/peak dBFS. These are finite-resolution measurement bins, not mesh channels or packet counts. Zero observed time is unavailable. Historical views use recorded summaries; time/location analysis requires saved history.

Frequency occupancy charts default to **Reveal low activity**, a nonlinear percentage scale that makes short, infrequent activity visible alongside continuously busy bins. Read the labeled ticks: 0%, 0.001%, 0.01%, 0.1%, 1%, 10% and 100%; equal vertical distances do not represent equal percentage changes. The linear 0–100% option remains available. Exact zero draws no active bar, an amber marker identifies unobserved data, and positive activity too small to resolve vertically receives a minimum visible marker. Hover to read the measured percentage and observed/busy durations; a marker's height is not a substitute for those values.

When multiple frequency bins share a chart column, its height represents the **maximum single-bin occupancy** in that column. Hover identifies the represented frequency range and the strongest bin's values. This preserves narrow activity when the display has fewer pixels than measurement bins; it does not combine their busy intervals or give a packet count. Changing the chart scale changes only the display, not the activity threshold, recorded measurements or exports. A center-frequency spike can be a receiver artifact and does not by itself identify an external transmitter.

To open a saved survey directly, launch the desktop executable with `--view-survey ABS_PATH`. It opens the database read-only, selects **Analyze** and loads one full-range analysis automatically. No radio or GPS is opened and no reception timer starts. Use **Apply range** or **Apply time and area** after editing the main controls; **Run analysis** remains in Measurement details. A frequency-graph selection runs the query on release. This desktop-only mode cannot be combined with receiver modes/settings, keys, GPS, duration, CLI analysis filters or export options; use the UI to select another analysis or export.

Use toolbar **Open...** to choose a saved session while stopped; older filename extensions remain supported. The main Analyze controls apply frequency bounds and a click/selection width, with **Apply range**, **Full range**, and **Width from lower edge**. **Time interval and geographic filters** contains elapsed bounds/detail and receiver-area filters. Use **Apply time and area** to apply changes. Successful graph selection updates the shared query, and saved live results refresh periodically. Reads run asynchronously against an independent saved snapshot; captions retain the prior applied selection while pending, and stale results are not applied to another session. **Measurement details...** opens the full advanced analysis controls and quality/coverage explanations.

**Select frequencies directly:** choose a width of 62.5, 125, 250 or 500 kHz, or Custom, then click to center that width or drag horizontally to select intersecting bins. Escape cancels a drag. The blue outline marks applied bounds. Select **Over time** or **By location** to inspect that same frequency interval. Full range restores the supplied span while retaining time/area filters. These queries never retune the receiver. Memory-only sessions retain the full-range chart but cannot provide retrospective time/location selection.

The new query runs once after release, with an updating notice. Until it finishes, captions and outlines still describe the prior applied result. Manual inputs remain available for exact frequency limits; changing an input alone does not relabel an existing result.

For the full technical controls, open **Measurement details...**. The following names refer to that advanced view:

1. Set **Lower MHz** and **Upper MHz** to arbitrary frequency edges. **View width** offers 62.5, 125, 250 and 500 kHz or **Custom** with **Width kHz**. **Apply width from lower edge** fills the upper edge. These are analysis views, not channel or modem presets. **Full recorded frequency range** sets both edges to zero.
2. Set **Start elapsed / s**, **End elapsed / s** and **Requested time detail / s**. End zero includes the remaining saved time. **Full recorded time** clears the interval.
3. Optionally enable **Filter by receiver geographic rectangle** and enter **South latitude**, **North latitude**, **West longitude** and **East longitude**. Bounds do not wrap across the antimeridian.
4. Select **Run analysis** (a frequency-chart selection also runs the query). Read **Analyzed frequencies**, **Analyzed time**, observed and busy seconds, quality flags and **Recorded coverage gaps** before interpreting occupancy.

The **Survey file** label identifies the saved source, including after reception stops. Receiver-position coverage reports the proportion of selected observed time with a stored endpoint fix. Missing tile-start/end durations can overlap; neither means the RF measurement itself is missing. Current GPS status does not repair a past missing fix. Hover a Quality cell or expand **What do these quality flags mean?** for explanations. Continuous energy is split into approximately two-second events, and overlapping event durations must not be summed to calculate occupancy.

**Full-interval overview** reports the requested and displayed time detail. The app widens the displayed buckets when necessary to cover the entire selected interval within 2,000 buckets; later history is not dropped to keep the requested detail. All observed time contributes to the overview. A bucket may combine partial coverage and use its last recorded receiver fix, so its GPS point does not represent every intervening location. Select a shorter elapsed interval and run analysis again for finer time and GPS detail.

The selected frequency region includes every recorded bin intersecting its requested edges, so the actual bin footprint can be slightly wider than a requested view or clipped to the recorded range. **Recorded any-bin busy** is the **union** of active FFT intervals across the selected bins; simultaneous activity is counted once. It means at least one frequency was active, not that the whole bandwidth was occupied. It is not a packet count or the sum or average of bin occupancy percentages. A selection with no observed time is unavailable, not zero occupancy. No report ranks or recommends channels.

When the selection intersects the receiver center, **Exclude receiver-center region from time/GPS view** starts enabled. This diagnostic guard covers up to five FFT bins around the nominal receiver center, where an internal DC artifact may occur. The displayed guard edges and bin count identify what is excluded. An artifact is possible, not confirmed: real RF signals may also be excluded, and the guarded region remains unassessed. **Outside-center busy** shows the time union for the remaining selected bins. Clear the checkbox to compare the original all-bin result. A selection entirely inside the guard has no outside-center result and is shown as unavailable, not zero. The detailed per-frequency chart retains the original measurements and shades the guard; neither the recording nor exports are center-filtered by this display choice.

**CHANNEL OCCUPANCY OVER TIME** repeats the actual **Frequency range** and **Width** directly above the graph, followed by any excluded receiver-center interval. The same caption accompanies the geographic plot. Each bar is the occupied fraction of observed time across those included frequencies; it does not locate activity at one particular frequency. The adjacent explanation identifies the effective bucket duration and both axes. The time axis covers the full analyzed interval, including intervals without observations. Gray marks below the axis show observed time; measured zero has no activity fill. Recorded gaps are striped and are not quiet measurements. Hover for the displayed busy fraction, original all-bin busy duration and observed time. Power values in the tooltip still describe the whole selected frequency region, including the center. The **peak envelope** sums bin maxima that can occur at different times; it is not a simultaneous measured channel peak. Partial-tile power selections use tile-average powers with boundary flags; busy-time masks retain complete-FFT resolution.

**GEOGRAPHIC RF VIEW / OFFLINE RECEIVER POSITIONS** uses the same guarded or all-bin comparison as the time chart, from cyan (0% busy) to orange (100% busy). Gray means the displayed fraction is unavailable, including an all-guard selection or zero observed time. Color describes RF busy time, not GPS accuracy; the mean-position reference stays neutral. It uses the same position-cloud/mobile display choice and distance scale as Settings > GPS & location. Click a time bucket or geographic point to copy its interval into the controls, then select **Run analysis**. **Set area to selected position +/- 0.001 degrees** fills an optional local rectangle; it also requires a new query. Dateline wrapping is a display feature; the geographic query rectangle still does not cross the antimeridian. Energy-event rows remain capped at 200 with a truncation notice; complete-query event totals remain separate from that list.

**Recorded coverage gaps** distinguishes explicit missing intervals from valid observations and corresponds to the striped time-plot intervals. A `source_stall` has unknown missing sample count, not zero loss. Unknown upstream loss may remain even when no gap rows appear. Geographic queries omit gaps because their receiver positions are unknown. Legacy schema-1–3 sessions remain readable, but the UI explicitly reports that detailed timing, geographic joint occupancy and energy-event analysis were not recorded; it shows their original aggregate frequency summaries instead. No raw IQ or waterfall replay is available.

For example, a local CLI report of a saved 62.5 kHz view is:

```sh
build/native/ovmesh-cli --analyze-saved /absolute/local/survey.sqlite \
  --frequency-range 907000000,907062500 --time-range 10,30
```

`--bounds SOUTH,NORTH,WEST,EAST` adds a receiver rectangle. `--headless-demo --spectrum-only --seconds 2 --session ABSOLUTE_NEW_PATH` creates a bounded synthetic survey without a radio. `--antenna-description`, `--receiver-description` and `--survey-notes` record optional acquisition context for a new survey.

Select **Export report...** in Analyze, then **Browse export location...** after stopping reception. Navigate to a folder, choose **CSV** or **GeoJSON** under **File type**, enter a new filename and press **Choose file**. Review the displayed destination and privacy choices, then press **Write export**. Choosing a file or canceling the chooser writes nothing. A missing extension is added to match the selected format; a conflicting extension must be corrected. The default **Frequency summary** uses the last completed analysis selection shown in the dialog, or the full survey when no analysis is loaded. Apply any changed filters and wait for completed analysis before opening the export dialog. Choose **Detailed archive** explicitly for the original whole-session CSV/GeoJSON; its scope is not the selection. Numeric measurement method, settings, observed durations and quality metadata accompany the export. Detailed-archive CSV also preserves measurement tiles, activity runs and energy events; detailed GeoJSON associates RF measurements with receiver positions when selected.

The built-in file browser uses the same controls on macOS, Windows and Linux: **Home**, **Up**, **Local disk**, folder rows, **Refresh** and **Show hidden**. Windows lists local fixed drives. Click a folder once to enter it. The selected full path is displayed read-only and is never silently shortened. Folder listings are cached and capped at 2,000 inspected entries; a notice identifies incomplete lists. Use a smaller folder or a known filename when needed. Recording/Open/report browsing is used while stopped; Save-copy and PNG destinations may be chosen during reception without changing its active path. Existing files are not replaced, and symlinks, Windows reparse points and unsupported mounted/network paths remain excluded. The app creates its own private default **Surveys** folder; create any other new folder in your operating system's file manager. These controls use existing C++/UI libraries and do not invoke a shell or an external file-dialog package.

Each new export dialog starts with three independent choices off:

- **Include authorized decoded content**: legacy authorized message fields.
- **Include receiver GPS coordinates**: receiver positions, with **Coordinate decimal places** when enabled.
- **Include antenna/receiver descriptions and survey notes**: free-form provenance that may contain private operational details.

CLI equivalents are `--content`, `--positions` and `--provenance`. Including descriptions does not require including decoded content or GPS. None is uploaded automatically. Coordinate rounding reduces numeric precision but does not guarantee anonymity; location details can still appear in selected message text or notes.

### Compact reports

Choose a report before selecting its new `.csv` destination:

- **Frequency summary:** busy/observed time, occupancy and power for each measured frequency bin.
- **Frequency interval over time:** joint busy time for the displayed frequency interval, in 60-second groups by default. Simultaneous signals count once.
- **Frequency by geographic area:** per-bin occupancy in approximately 100-metre receiver-location cells. Original fixes remain unchanged. Unlocated observations and cell-boundary attribution are explicit.
- **Waveform observations:** inferred LoRa settings and uncertainty, without claiming packet identity or packet airtime.
- **Receiver GPS track:** original fix time/quality and coordinates at the chosen export precision; frequency selection describes survey context, not a filter on GPS fixes.
- **Authorized decoded content:** only retained, eligible content, with an explicit content opt-in.

GPS reports require receiver-coordinate inclusion. Content, coordinates and free-form notes retain independent privacy controls. Default export coordinate rounding may make adjacent 100-metre cell bounds coincide; cell IDs distinguish groups. Increase export decimal places explicitly when more precision is appropriate. This does not change stored GPS precision, and a grid ID still reveals location.

Reports fail clearly rather than truncate at one million rows, 200,000 time groups or 200,000 geographic cells. For a long route across the full band, choose a narrower frequency selection, split the time interval, or increase cells to 500 metres or more. For example, 2,559 bins across 400 cells exceeds the row limit; a 500-metre grid reduces the number of location groups. The database retains the complete survey regardless of export grouping. Detailed archive remains available and can still be gigabytes; it is not intended as the default spreadsheet view.

### Optional legacy decoded-content export

Detailed-archive metadata exports can include `evidence_port` and `evidence_signature_present` when qualifying envelope evidence exists. **Include authorized decoded content** additionally permits request/reply IDs, routing variant, signature presence, and both routes/SNR lists for supported decoded content. Route nodes are semicolon-separated unsigned integers. `snr_towards_db_x4` and `snr_back_db_x4` in detailed archives are JSON integer lists (semicolon-separated lists in the compact content report) containing the original values, including unknown markers; divide non-sentinel values by four for dB. Unsupported payloads remain excluded even when content export is enabled.

Including content can disclose message text, node identifiers, sender-reported positions, telemetry, and routing information. Excluding receiver GPS does not remove locations mentioned in message text. Coordinate rounding reduces numeric precision; it is not anonymization. GeoJSON export requires explicit receiver-position inclusion in the UI or CLI; see the CLI's `--help` output. No export is uploaded automatically.

### Storage and compatibility

Session files and exports are **plaintext local files**, created with private file permissions where the platform supports the implemented controls. They are not encrypted databases. The application retains approved metadata and eligible decoded content; it has no raw-IQ, ciphertext, or undecoded-payload export. Decoding requires transient process memory, and the application cannot promise that the operating system never pages memory or creates a crash dump. No key added later can recover discarded undecoded traffic.

New compact sessions use storage schema 6; Detailed mode uses schema 5. Schema 6 shares up-to-one-second power blocks and exact GPS references while retaining fine joint activity. Historical files are unchanged. Schema 5 retains schema-4 spectrum tiles, joint activity masks, generic energy events, coverage gaps and measurement provenance, and adds waveform observations plus discovery status, subband progress and gap records. CSV/GeoJSON include inferred BW/SF and reception-time evidence separately from occupancy and payloads. Receiver-location export controls apply to waveform records. CSV session rows retain `tuning_offset_hz` and derived `tuner_command_hz` alongside nominal frequency. Schema-1 through schema-5 surveys open read-only without migration or rewriting; schema 1 implies zero tuning correction. Missing historical waveform records cannot be reconstructed from phase-free power tiles. Newly introduced fields remain blank where they were not recorded.

See the [support playbook](operations/support-playbook.md) for safe local troubleshooting, including unavailable receivers, no decodes, missing GPS, interrupted sessions, and export errors.
