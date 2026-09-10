# Selected LoRa codec logic

`lora_codec.hpp` and the symbol transformation in `src/phy_codec.cpp` adapt the
header checksum, Gray mapping, diagonal interleaving, codeword tables, whitening,
and payload CRC logic from SDRangel at revision
`866ef1656af7e0923554581afc5c6dd97cbafbaf`:

- `plugins/channelrx/demodmeshtastic/meshtasticdemoddecoderlora.h`
- `plugins/channelrx/demodmeshtastic/meshtasticdemoddecoderlora.cpp`

Original copyright: 2020 Edouard Griffiths, F4EXB. The original files identify
`myriadrf/LoRa-SDR` as inspiration and describe compatibility with `gr-lora_sdr`.
License: GPL-3.0-or-later; the accompanying `LICENSE` is the reviewed upstream
GPL version 3 text. Preserve these notices with derived distributions.

Local changes remove Qt and logging, bound lengths and parameter domains,
initialize every status, distinguish absent CRC from verified CRC, use a bounded
nearest-codeword decoder that rejects ambiguous corrections, and generate the
upstream whitening sequence from its LFSR. No plugin, device control, key handling,
networking, recording, or transmitter code was imported. Synthetic modulation is
new in-memory test code, with no RF output path. The streaming synchronizer and
radix-2 FFT in `src/phy.cpp` are project implementations informed by LoRa's
published chirp structure and the reviewed SDRangel synchronization approach.

The original review-file SHA-256 values remain in
`docs/research/sdrangel-source-manifest-2026-09-09.json`. These adaptations are
not a claim of RF interoperability or completion of upstream's soft-decision and
sample-clock-offset compensation behavior.
