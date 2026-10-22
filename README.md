# Chromebook SOF Audio for Windows 11 (`cros-sofaudio`)

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%2011%20x64-0078D6.svg)](https://microsoft.com/windows)
[![Hardware](https://img.shields.io/badge/Hardware-MAX98373%20%7C%20NAU88L25-orange.svg)](#supported-hardware)

**cros-sofaudio** provides open-source Windows 11 Kernel-Mode Driver Framework (KMDF) audio drivers for Intel-based Chromebooks running **Sound Open Firmware (SOF)** audio architectures with:
* **Maxim Integrated MAX98373** Class-D Smart Power Amplifiers (internal stereo speakers)
* **Nuvoton NAU88L25** High-Performance Audio Codec (3.5mm headphone jack, TRRS headset microphone, and hardware jack detection)

---

## Architecture Overview

On modern Chromebooks running coreboot / UEFI firmware (such as MrChromebox UEFI), the audio subsystem is routed through an Intel Audio DSP (cAVS / ACE) offloading to I2S / TDM serial buses controlled via I2C (`SPB`) and ACPI GPIO interrupts:

```
                      +---------------------------------------+
                      |       Windows 11 Audio Engine         |
                      |            (WASAPI / PortCls)         |
                      +-------------------+-------------------+
                                          |
                      +-------------------+-------------------+
                      |   Intel Audio DSP / SOF WaveRT Driver  |
                      |   (e.g., csaudiointcsof / sklhdaudbus) |
                      +---------+-------------------+---------+
                                | I2S / TDM Bus     | Sidecar / CSAudio IOCTL
            +-------------------+----+        +-----+--------------------+
            |                        |        |                          |
+-----------v------------+  +--------v--------v-----+  +-----------------v---------+
| Maxim MAX98373 Amp     |  | Nuvoton NAU88L25      |  | Windows Sidecar / Bus     |
| Driver (Max98373Audio) |  | Driver (Nau8825Audio) |  | Interface Notifications   |
+-----------+------------+  +--------+--------+-----+  +---------------------------+
            | I2C (SPB)              | I2C    | GPIO IRQ
            | (Addr 0x31/0x32)       | (0x1A) | (Jack Detect)
+-----------v------------+  +--------v--------v-----+
| Hardware: MAX98373     |  | Hardware: NAU88L25    |
| Stereo Speakers        |  | 3.5mm Headphone Jack  |
+------------------------+  +-----------------------+
```

* **SklHdaudBus.sys**:
  * Intel AudioDSP / HD Audio PCI Controller Bus Driver (adapted from CoolStar).
  * Manages Intel PCH PCI controllers (Skylake through Meteor Lake).
  * Parses ACPI NHLT (Non-HD Audio Link Table) blobs and manages cyclic DMA stream allocation.
  * Exposes `GUID_ADSP_BUS_INTERFACE` on child PDO `CSAUDIO\ADSP&CTLR_VEN_8086&CTLR_DEV_%04X`.
* **CsAudioIntcsof.sys**:
  * Intel Sound Open Firmware (SOF) PortCls WaveRT Miniport Driver.
  * Connects to `SklHdaudBus` via `GUID_ADSP_BUS_INTERFACE` to stream raw audio samples.
  * Registers 4 WaveRT endpoints: Speakers (MAX98373), Headphones (NAU88L25), Internal Digital Mic Array, and Headset Mic Jack (NAU88L25).
  * Coordinates real-time hardware stream states with codec sidecars over `\CallBack\CsAudioCallbackAPI` and `GUID_DEVINTERFACE_CSAUDIO_CODEC`.
* **Max98373Audio.sys**:
  * ACPI Hardware IDs: `ACPI\MX98373`, `ACPI\MAX98373`
  * Full 16-bit register address, 8-bit data I2C transaction engine over Windows SPB.
  * Channel role routing (Left, Right, Mono Mix) based on ACPI `_UID` or slot properties.
  * Pop-free soft-mute and dynamic headroom limiter protection.
  * Power management (`D0Entry` / `D0Exit`) with low-power global shutdown standby.
* **Nau8825Audio.sys**:
  * ACPI Hardware IDs: `ACPI\10508825`, `ACPI\NVTN2000`, `ACPI\NAU8825`
  * 16-bit register, 16-bit data I2C engine.
  * FLL clock synthesis for 48kHz audio playback and capture.
  * Hardware Jack Detection via ACPI GPIO Interrupt (`DIRQL` ISR + `PASSIVE_LEVEL` WorkItem).
  * Auto-differentiation between **3-pole (TRS) stereo headphones** and **4-pole (TRRS) headset with microphone** (CTIA / OMTP detection via SAR ADC).
  * Real-time Windows Audio endpoint switching notification.

---

## Quick Installation

1. Download the latest `cros-sofaudio-v1.1.1.zip` from the [Releases](https://github.com/isaiahscape/cros-sofaudio/releases) page.
2. Extract the ZIP archive on your Chromebook.
3. Right-click **`install-drivers.cmd`** and select **Run as administrator**.
   * It will self-elevate, install the security certificate to the Windows Trusted Certificate store, and install the complete driver stack (`SklHdaudBus`, `CsAudioIntcsof`, `Max98373Audio`, `Nau8825Audio`) via `pnputil`.
4. (Optional) Run `cros_audio_test.exe` to inspect live hardware registers and monitor jack plug/unplug events.
5. **To uninstall:** Right-click **`uninstall-drivers.cmd`** and select **Run as administrator** to remove all drivers, the switcher service, and DSP firmware.

---

## Building from Source

### Prerequisites
* Windows 11 (x64)
* Visual Studio 2022 or Build Tools with C++ workload
* Windows Driver Kit (WDK) 10.0.22621+ or newer

### Build Command
Run the automated build script:
```cmd
build.cmd
```
The script will build all driver projects in `Release|x64`, sign the binaries with the certificate, and output packages into `x64\Release\`.

---

## Diagnostic Tool (`cros_audio_test.exe`)

The bundled test utility connects to both driver interfaces via IOCTLs:
* Reads silicon revision IDs (`0x21FF` for MAX98373, `0x58` for NAU88L25).
* Real-time asynchronous background thread monitoring 3.5mm jack insertion/ejection.
* Tests stream start/stop and pop-free volume ramping.

---

## Credits & Acknowledgements

This project builds heavily upon and incorporates open-source source code and reverse-engineering research by:

* **[CoolStar](https://github.com/coolstar)** ([@coolstarorg](https://github.com/coolstar))
  * **Original creator of `sklhdaudbus` and pioneer of the `csaudio` Windows audio architecture for Chromebooks.**
  * Author of [coolstar/sklhdaudbus](https://github.com/coolstar/sklhdaudbus), [coolstar/csaudioacp3x](https://github.com/coolstar/csaudioacp3x), and [coolstar/csaudiosstcatpt](https://github.com/coolstar/csaudiosstcatpt).
  * CoolStar's reverse-engineering of the Intel HD Audio / ADSP bus, ACPI NHLT parsing, and decoupled codec sidecar callback mechanism is the foundation for this driver suite. Full credit and gratitude go to CoolStar for making Windows on Chromebook audio possible!
  * See [CREDITS.md](CREDITS.md) for full details.

* **[Kier](https://github.com/3kierz)** ([@3kierz](https://github.com/3kierz)) & **Claude** for diagnosing and fixing the sound output drivers.
* **[MrChromebox](https://mrchromebox.tech)** for coreboot / UEFI firmware.
* **The Sound Open Firmware (SOF) Project & Linux ALSA Team** for kernel hardware topologies and specs.

---

## License

Licensed under the [MIT License](LICENSE).  
Portions copyright (c) CoolStar.  
Copyright (c) 2026 Leonardo (The Scape Network).
