# Chromebook Audio Driver Installation & Deployment Guide
## Windows 11 Support for MAX98373 Speaker Amp & NAU88L25 Headphone Jack

### 1. Prerequisites

Before installing the audio drivers on your Intel-based Chromebook running Windows 11:
1. **Custom UEFI Firmware:** Your Chromebook must have full UEFI firmware flashed (e.g., via **MrChromebox coreboot/UEFI firmware**) with ChromeOS Write-Protect disabled.
2. **Intel HD Audio / Bus Driver:** Ensure that an Intel AudioDSP / HD Audio bus driver (such as CoolStar's `sklhdaudbus` or `csaudiointcsof`) is installed so the audio pipelines are present.
3. **Test Signing Enabled:** Because these drivers are self-built open source drivers, Windows 11 requires Test Signing to be turned on.

---

### 2. Building the Drivers

You can build the drivers using Visual Studio 2022 with the Windows Driver Kit (WDK) installed, or directly using the provided automated script:

#### Option A: One-Click Build Script
Open an Administrator Command Prompt or PowerShell, navigate to this folder, and run:
```cmd
build.cmd
```
The script will locate your Visual Studio 2022 / WDK installation and build the entire solution in `Release|x64`.

#### Option B: Visual Studio 2022
1. Open `ChromebookAudioDrivers.sln` in Visual Studio 2022.
2. Select **Release** and **x64**.
3. Build the Solution (`Ctrl + Shift + B`).

---

### 3. Installation Steps

1. **Enable Test Mode (Administrator Command Prompt):**
   ```cmd
   bcdedit /set testsigning on
   ```
   *Reboot your Chromebook after setting this.*

2. **Automated Driver Installation:**
   Right-click `install-drivers.cmd` and select **Run as administrator**.

   Alternatively, install manually via `pnputil`:
   ```cmd
   pnputil /add-driver "d:\Documents\sofaudio-driver\Max98373Audio\Max98373Audio.inf" /install
   pnputil /add-driver "d:\Documents\sofaudio-driver\Nau8825Audio\Nau8825Audio.inf" /install
   ```

3. **Verify in Device Manager:**
   * Open `Device Manager` (`devmgmt.msc`).
   * Under **Sound, video and game controllers** (or **System devices**), verify that:
     * `Maxim Integrated MAX98373 Smart Audio Amplifier` is started without error code 10.
     * `Nuvoton NAU88L25 Headset Audio Codec & Jack Detect` is started without error code 10.

---

### 4. Hardware Verification & Real-Time Diagnostics

Launch the bundled diagnostic test tool:
```cmd
x64\Release\cros_audio_test.exe
```

This utility will:
1. Verify I2C communication by reading the silicon revision IDs:
   * **MAX98373:** Revision ID register `0x21FF` should return `0x43`.
   * **NAU88L25:** Device ID register `0x58` should return `0x001A`.
2. Monitor headphone jack plug/unplug events in real time:
   * Plug in headphones -> Displays `CONNECTED: Standard 3-Pole (TRS) Stereo Headphones`.
   * Plug in a smartphone headset -> Displays `CONNECTED: TRRS 4-Pole Headset with Microphone`.
   * Unplug -> Displays `UNPLUGGED`.
3. Test speaker and headphone DAC stream powering and pop-free volume ramping.
