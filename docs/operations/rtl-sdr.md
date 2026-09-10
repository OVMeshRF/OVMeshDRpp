# RTL-SDR reception

Select **RTL-SDR / USB** in the receiver selector while reception is stopped. HackRF remains a separate choice. Selecting another receiver resets Offset and the device serial; calibrations and device selection must not silently carry across receivers. The app preserves the nominal center when it fits the selected receiver's supported range.

The initial RTL adapter provides continuous reception at **1 or 2 MS/s**. At 2 MS/s, use a **1.5 MHz span** with LoRa discovery; spectrum-only acquisition may use up to 1.6 MHz. At 1 MS/s, the maximum spectrum span is 0.8 MHz and discovery must be disabled. These are application limits with guarded DSP coverage, not a calibration certificate or a claim that every RTL tuner has a flat passband. This integration does not sweep a wider requested range. Frequencies outside the displayed range are unobserved.

For example, center **906.875 MHz**, rate **2 MS/s**, span **1.5 MHz** continuously observes approximately 906.125–907.625 MHz before FFT edge rounding and the receiver-center exclusion. It does not simultaneously include 908.75 MHz. Retuning starts another session; compare surveys with their own recorded coverage and receiver settings.

RTL-SDR has one **Tuner gain** control, not HackRF's LNA/VGA stages or RF amplifier. Manual gain is recommended for comparing RF measurements. The driver selects the closest supported gain and the session records the applied value. Auto gain is available but its instantaneous gain is not measured, so absolute received-level comparisons are limited. Bias tee and digital AGC remain off. Received power is uncalibrated dBFS; it does not measure transmitter watts.

## Build and launch

The reviewed driver source is included. Provide libusb development headers/library using the platform's trusted package source or an explicitly configured local build. CMake discovers it through pkg-config or `RTLUSB_INCLUDE_DIR` and `RTLUSB_LIBRARY`. No system package installation occurs automatically. `-DOVMESH_ENABLE_RTLSDR=OFF` omits this backend. HackRF support is independently controlled by `OVMESH_ENABLE_HACKRF`.

On Windows, the selected dongle needs a compatible libusb/WinUSB device binding. On Linux, the user needs USB permissions and the DVB driver must not own the interface. This application neither replaces Windows drivers nor edits Linux rules/blacklists. Follow the hardware vendor and distribution documentation for the exact receiver; changing unrelated USB devices is not necessary. macOS uses libusb directly. Platform packaging and model-specific hardware validation remain separate work.

For a bounded receive-only test from the build folder:

```sh
./ovmesh-cli --receive-rtlsdr --confirm-radio-access --seconds 30 --spectrum-only --discover-lora
```

`--spectrum-only` here disables payload decoding; `--discover-lora` still enables waveform observations. Omit the latter for spectrum measurements alone. Add `--session /absolute/local/path/new.sqlite` to retain approved survey metadata in a new file. Keys and GPS are not opened or configured by this command. For the desktop executable, use `--desktop-receive-rtlsdr` with the same confirmation and duration, or launch normally and press Start. Other receiver arguments are listed by `--help`.

If Start fails, release the dongle from other SDR software, verify USB connection and permissions, and read the driver error shown in the app. A missing backend indicates missing build dependencies. A no-samples timeout or stream error marks the survey incomplete; a moving waterfall alone is not evidence of loss-free coverage. Use input, measurement and discovery counters to assess the run. See the [support playbook](support-playbook.md) and [driver intake](../security/rtlsdr-intake.md).
