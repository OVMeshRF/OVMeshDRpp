# RTL-SDR reception

Select **RTL-SDR / USB** in the receiver selector while reception is stopped. HackRF remains a separate choice. Selecting another receiver resets Offset and the device serial; calibrations and device selection must not silently carry across receivers. The app preserves the nominal center when it fits the selected receiver's supported range.

The RTL adapter provides continuous spectrum reception at **1 or 2 MS/s**. At 2 MS/s the maximum configured spectrum span is **1.6 MHz**; the remembered/default span may be smaller. At 1 MS/s the maximum is 0.8 MHz. LoRa discovery and packet decoding are disabled in the desktop. These are application limits with guarded DSP coverage, not a calibration certificate or a claim that every RTL tuner has a flat passband. This integration does not sweep a wider requested range; frequencies outside the displayed range are unobserved.

For example, center **906.875 MHz**, rate **2 MS/s**, span **1.5 MHz** continuously observes approximately 906.125–907.625 MHz before FFT edge rounding and the receiver-center exclusion. It does not simultaneously include 908.75 MHz. Stop, retune and Resume to append a new acquisition interval; compare its recorded coverage/settings with the earlier interval.

RTL-SDR has one **Tuner gain** control, not HackRF's LNA/VGA stages or RF amplifier. Manual gain is recommended for comparing RF measurements. The driver selects the closest supported gain and the session records the applied value. Auto gain is available but its instantaneous gain is not measured, so absolute received-level comparisons are limited. Bias tee and digital AGC remain off. Received power is uncalibrated dBFS; it does not measure transmitter watts.

## Build and launch

The reviewed driver source is included and links shared libusb 1.0.30 from the reviewed local USB prefix. On native macOS/Linux, prepare it with `python3 tools/bootstrap_usb.py --download`, or supply both pinned archives for offline preparation. CMake uses `build/deps/usb-1.0.30-local` by default; use `-DOVMESH_USB_ROOT_DIR=/absolute/path/to/prefix` for another reviewed prefix. It rejects missing inputs and mixed or host-library paths; it does not use pkg-config to select a different libusb. No system package installation occurs automatically. `-DOVMESH_ENABLE_RTLSDR=OFF` omits this backend. HackRF support is independently controlled by `OVMESH_ENABLE_HACKRF`. See [build instructions](deployment.md#build) and [USB intake](../security/usb-intake.md).

On Windows, the selected dongle needs a compatible libusb/WinUSB device binding; the local dependency helper does not qualify Windows builds or packaging. On Linux, the user needs USB permissions and the DVB driver must not own the interface. The local libusb build uses its netlink backend with `--disable-udev`; this changes neither device permissions nor driver ownership. This application neither replaces Windows drivers nor edits Linux rules/blacklists. Follow the hardware vendor and distribution documentation for the exact receiver; changing unrelated USB devices is not necessary. macOS uses libusb directly. Platform packaging and model-specific hardware validation remain separate work.

For a bounded receive-only test from the build folder:

```sh
./ovmesh-cli --receive-rtlsdr --confirm-radio-access --seconds 30 --spectrum-only
```

This command measures spectrum only. Add `--session /absolute/local/path/new.sqlite` to retain survey metadata in a new file. It does not open GPS or configure keys. For the desktop executable, use `--desktop-receive-rtlsdr` with the same confirmation and duration, or launch normally and press Start. The desktop always applies the spectrum-only gate, including with earlier decoder-enabled preferences.

If Start fails, release the dongle from other SDR software, verify USB connection and permissions, and read the driver error shown in the app. A missing backend indicates missing build dependencies. A no-samples timeout or stream error marks the survey incomplete; a moving waterfall alone is not evidence of loss-free coverage. Use input, measurement, clipping and coverage-gap counters to assess the run. See the [support playbook](support-playbook.md) and [driver intake](../security/rtlsdr-intake.md).
