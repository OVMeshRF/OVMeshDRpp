# Meshtastic preset catalog

The receiver catalog covers all 17 `ModemPreset` IDs in the project's pinned
Meshtastic protobuf v2.8.0, including the ten IDs in v2.7.19. These are sub-GHz
LoRa parameter bundles. They do not specify a survey frequency, channel key,
sender identity, or permission to transmit.

Implementation: [meshtastic_presets.hpp](../../include/ovmesh/meshtastic_presets.hpp).
The current official sources were checked at the immutable revisions below;
no firmware or additional runtime dependency was adopted for this catalog.

| Reference | Exact revision | Purpose |
|---|---|---|
| [Protobuf v2.7.19 config.proto](https://github.com/meshtastic/protobufs/blob/e1a6b3a868d735da72cd6c94c574d655129d390a/meshtastic/config.proto) | `e1a6b3a868d735da72cd6c94c574d655129d390a` | IDs 0–9 and deprecation flags |
| [Protobuf v2.8.0 config.proto](https://github.com/meshtastic/protobufs/blob/7b2464c9b8c1521f93852261e4123826e5b25e11/meshtastic/config.proto) | `7b2464c9b8c1521f93852261e4123826e5b25e11` | IDs 0–16; vendored application schema |
| [Current official config.proto](https://github.com/meshtastic/protobufs/blob/3b3df2a5e54a6f4599ab37ac819da597607a4a27/meshtastic/config.proto) | `3b3df2a5e54a6f4599ab37ac819da597607a4a27` | Same 17 IDs and deprecations as the pinned v2.8.0 schema |
| [Original firmware MeshRadio.h](https://github.com/meshtastic/firmware/blob/6d41e279f1f51bd59f687b9d441c1bf47b1594fc/src/mesh/MeshRadio.h) | `6d41e279f1f51bd59f687b9d441c1bf47b1594fc` | Existing nine active RF mappings; matches the project's original wire baseline |
| [Current firmware MeshRadio.h](https://github.com/meshtastic/firmware/blob/dcdfa32250282cb01745f774cd8aa410e6e44f8e/src/mesh/MeshRadio.h) | `dcdfa32250282cb01745f774cd8aa410e6e44f8e` | All 16 current RF mappings, including Lite, Narrow, Tiny and MediumTurbo |
| [Historical firmware RadioInterface.cpp](https://github.com/meshtastic/firmware/blob/v2.4.2.5b45303/src/mesh/RadioInterface.cpp) | Release tag `v2.4.2.5b45303` | Removed VeryLongSlow RF parameters |

The historical file's measured SHA-256 is
`da33ae9190f35cfdcca5cf63b85977bc43f88b1105be8dd28b3f4905393fe83d`.
The current `MeshRadio.h` file's measured SHA-256 is
`f0e32287b665cabc08b9fb63719f3b3c56fac15e7bf037ed2021ad94cf4b279e`.
These hashes identify reviewed public source files; they are not release signatures.

## Sub-GHz RF parameters

Coding rate is written as 4/denominator. Names below are human-readable catalog
labels, not required primary channel names.

| Wire ID | Preset | Bandwidth kHz | SF | Coding rate | Baseline/status |
|---:|---|---:|---:|---|---|
| 0 | LongFast | 250 | 11 | 4/5 | v2.7.19 and v2.8.0 |
| 1 | LongSlow | 125 | 12 | 4/8 | Both; deprecated upstream in 2.7 |
| 2 | VeryLongSlow | 62.5 | 12 | 4/8 | Both retain the enum; historical RF profile, deprecated in 2.5 |
| 3 | MediumSlow | 250 | 10 | 4/5 | Both |
| 4 | MediumFast | 250 | 9 | 4/5 | Both |
| 5 | ShortSlow | 250 | 8 | 4/5 | Both |
| 6 | ShortFast | 250 | 7 | 4/5 | Both |
| 7 | LongModerate | 125 | 11 | 4/8 | Both; not deprecated |
| 8 | ShortTurbo | 500 | 7 | 4/5 | Both |
| 9 | LongTurbo | 500 | 11 | 4/8 | Both |
| 10 | LiteFast | 125 | 9 | 4/5 | Added between the reviewed schema baselines |
| 11 | LiteSlow | 125 | 10 | 4/5 | Added between the reviewed schema baselines |
| 12 | NarrowFast | 62.5 | 7 | 4/6 | Added between the reviewed schema baselines |
| 13 | NarrowSlow | 62.5 | 8 | 4/6 | Added between the reviewed schema baselines |
| 14 | TinyFast | 15.625 | 7 | 4/5 | Added between the reviewed schema baselines |
| 15 | TinySlow | 15.625 | 8 | 4/6 | Added between the reviewed schema baselines |
| 16 | MediumTurbo | 500 | 9 | 4/5 | Added between the reviewed schema baselines |

VeryLongSlow no longer has a case in either reviewed modern firmware parameter
switch. Its enum would fall through to LongFast there. The catalog preserves the
historical 62.5 kHz waveform for receiving older traffic and explicitly labels it;
it does not suggest that selecting that enum configures modern radios this way.

Tiny uses the nominal 15.6 kHz LoRa modem setting, represented as 15,625 Hz in
the SDR sample-rate model. The upstream schema's 20 kHz description refers to
the regional channel allocation: the current firmware adds 2.2 kHz padding on
each side of its nominal 15.6 kHz setting. It is not a 20 kHz modem waveform.
See [regional profiles and parameter application](https://github.com/meshtastic/firmware/blob/dcdfa32250282cb01745f774cd8aa410e6e44f8e/src/mesh/RadioInterface.cpp).

The native SDR PHY accepts every parameter bundle in this table. That means
implemented parameter support, not guaranteed discovery or successful reception
under noise, clock error, collisions, clipping, missing samples, or processing
overload. The RAK concentrator has separate modem bandwidth and scheduling
limits. The upstream 2.4 GHz wide-LoRa variants (203.125/406.25/812.5/1625 kHz)
are outside this sub-GHz catalog and are not advertised as supported.

## One public key, independent frequencies

`AQ==` selects the same published AES key for every preset; the RF preset does
not change its expansion. A channel name affects the short header hash, not the
expanded key. The explicit public survey-key option therefore applies across
the range and all supported presets, without a required channel-name filter.
Private channel keys remain separately supplied in memory. See the
[key-index expansion and hash definitions](https://github.com/meshtastic/firmware/blob/6d41e279f1f51bd59f687b9d441c1bf47b1594fc/src/mesh/Channels.cpp)
and the [native wire baseline](meshtastic-wire-baseline.md).

Selecting a preset for a manual receive profile changes its BW/SF/CR bundle,
while leaving the actual selected frequency unchanged. There is intentionally
no automatic US frequency helper here: primary names, explicit frequency slots,
frequency overrides, region configuration and firmware revisions can alter the
center frequency. Discovery must continue to search anywhere inside the supplied
usable range, including non-default centers. Regional transmit restrictions
remain a separate subject from receive-side recognition.

## Validation and maintenance

`test_meshtastic_presets` checks catalog completeness, stable IDs, names and
deprecation against both already-vendored schemas, checks the frozen RF parameter
vectors, rejects an unknown ID, and exercises clean synthetic PHY reception for
all 17 bundles. It uses generated in-memory bytes only; no device, payload log,
GPS, key file, schema generator, or network is required.

On a future schema or firmware update, compare both the enum and the actual
firmware parameter switch. Preserve deprecated wire IDs and distinguish removed
RF mappings from newly supported profiles. Upstream descriptions can lag the
code: for example, the ShortTurbo comment still calls it the only 500 kHz
preset although LongTurbo and MediumTurbo also use 500 kHz.
