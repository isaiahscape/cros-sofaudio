# Credits and Acknowledgements

This project owes immense gratitude and credit to **CoolStar** ([@coolstarorg](https://github.com/coolstar)), whose pioneering reverse-engineering work and open-source codebase made native Windows audio possible on Chromebooks.

## Foundational Projects & Components

### 1. `SklHdaudBus` (Intel AudioDSP / HD Audio Bus Driver)
- **Original Author & Reverse Engineering:** **CoolStar** ([coolstar/sklhdaudbus](https://github.com/coolstar/sklhdaudbus))
- CoolStar reversed and implemented the Intel PCI HD Audio controller driver with NHLT ACPI parsing, AudioDSP child PDO enumeration (`CSAUDIO\ADSP`), and stream DMA allocation for Intel Skylake, Kaby Lake, Whiskey Lake, Comet Lake, Jasper Lake, Tiger Lake, and Alder Lake.
- The `SklHdaudBus` implementation in this repository is directly adapted and extended from CoolStar's open-source driver.

### 2. `CsAudioIntcsof` (Intel Sound Open Firmware PortCls WaveRT Audio Driver)
- **Original Architecture & Concept:** **CoolStar** ([coolstar/csaudioacp3x](https://github.com/coolstar/csaudioacp3x), [coolstar/csaudiosstcatpt](https://github.com/coolstar/csaudiosstcatpt))
- CoolStar created the `csaudio` architecture, separating PortCls WaveRT streaming from codec-specific I2C sidecars via `GUID_DEVINTERFACE_CSAUDIO_CODEC` and `\CallBack\CsAudioCallbackAPI`.
- The `CsAudioIntcsof` driver integrates this framework with the `ADSP_BUS_INTERFACE` DMA pipes for Sound Open Firmware (SOF) topologies.

### 3. Maxim MAX98373 & Nuvoton NAU88L25 Codec Drivers
- **MAX98373 Hardware Registers & DAI Configuration:** Directly referenced and ported from **CoolStar** ([coolstar/opengmaxcodec](https://github.com/coolstar/opengmaxcodec)), utilizing CoolStar's proven TDM Mode 0, clock ratio (128x fs), unmuted digital volume staging, and TX Hi-Z slot masking sequences that make the internal speakers work on Windows.
- Built to interface seamlessly with the `csaudio` callback and IOCTL sidecar interface designed by CoolStar.

---

### Special Thanks
- **CoolStar** ([github.com/coolstar](https://github.com/coolstar)) - Without CoolStar's work across Chromebook Windows audio, DSP buses, and custom drivers, this project would not exist.
- **MrChromebox** ([mrchromebox.tech](https://mrchromebox.tech)) - For coreboot and UEFI firmware on ChromeOS hardware.
- **The ChromiumOS Authors & Linux SOF Project** - For open-source hardware documentation, DSP topologies, and Linux driver references.
