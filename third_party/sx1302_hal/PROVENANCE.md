# Semtech SX1302/SX1303 USB receive subset

Upstream: [Lora-net/sx1302_hal](https://github.com/Lora-net/sx1302_hal), release 2.1.0,
revision `4b42025d1751e04632c0b04160e0d29dbbb222a5`.

The official source archive was verified against SHA-256
`c0f9727361ed704d13b5f1521511f4ad2dcfb0a43e58f3fbd390456ee692ade6` and
the revision's individual Git blob identifiers. This establishes consistency with
the reviewed source, not a guarantee that the upstream software is defect-free.
The source manifest records original and adopted file hashes.

Only HAL C sources, needed headers, volatile radio firmware arrays and TinyMT32
are included. There is no packet forwarder, network server, MCU flash image,
flashing tool, shell installer, GPS serial reader or upstream build script.
Linux SPI/I2C implementations are omitted and replaced by fail-closed stubs.
The worker supports USB CDC on macOS and Linux. Windows is not implemented.

`LICENSE.TXT` retains the upstream BSD notices, including TinyMT32's notice.
It also retains the upstream notice for Parson; Parson itself is not included.

## Local changes

- Bounded nonblocking raw serial I/O, exclusive character-device open, no symlink
  following, explicit flow-control removal and a failed-link latch.
- Exact request/response framing, per-SPI-transaction ID/type/length validation,
  32-byte memory requests and bounded host-write pacing. Auxiliary scan tuning
  sends individually acknowledged USB transactions while retaining the HAL
  mode/flush API; it does not aggregate the eight commands into one request.
  Completed histogram banks are read in 24/24/18-byte data chunks and exposed
  only after all chunks succeed. Fixed timeout categories identify request or
  acknowledgement failure without logging device identifiers or frame bytes.
- MCU bootloader/reset commands rejected; radio transmit entry points disabled.
- Corrected auxiliary scan command and version-buffer bounds; fail-fast startup.
- Corrected unsigned timestamp assembly and wrap-counter shifts; guarded receive
  buffer extents and timestamp metric arrays before parsing variable data. FIFO
  length read failures stop immediately, reported lengths cannot exceed the fixed
  buffer, and packet counting checks complete metadata and payload extents.
- Portable `nanosleep` and platform-specific `qsort_r` calling conventions.
- Upstream diagnostic output suppressed at compilation. The worker emits only
  bounded protocol records and generic errors; packet-dump routines are disabled.

The worker separately controls the application pipe, wipes emitted frame buffers,
checks the receive sync-word register bit patterns, and interleaves packet polling
with the auxiliary scan state machine. It never writes packet data to files.

## Scanner interpretation

The SX1261 result array has 33 counts. This pinned implementation labels entries
0–31 with `-4 * index + offset` dB, using nominal offset **−11 dB**, and entry32
as underflow below the last threshold. Thus returned labels are −11, −15, …,
−135 dB; entry32 is below −135 dB. Keep all33 counts in their original order.
Observed result vectors total the configured sample count; the worker rejects
vectors that do not. Labels are uncalibrated receiver RSSI estimates, not
transmitter power. Do not cumulatively sum the array as if each entry were an
independent above-threshold counter.

The auxiliary `BW_125KHZ` argument selects a nominal **234.3 kHz FSK receive
filter** in this pinned implementation. It is not a measured LoRa bandwidth.
Worker scan timestamps bracket the host scan operation, including polling and
readout, and are not an exact RF dwell-time measurement. Packet polling can extend
that interval. Frequencies between visits remain unobserved.

Any future upstream update must preserve these receive-only, framing, logging,
privacy and portability changes, run the offline adversarial tests, and repeat
hardware acceptance before being treated as validated support.
