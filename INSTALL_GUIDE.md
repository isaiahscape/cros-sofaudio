# Chromebook Audio Driver Installation & Deployment Guide
## Complete Windows 11 Audio Driver Stack (SOF AudioDSP + MAX98373 + NAU88L25)

> **Credits:** Built upon foundational reverse-engineering and source code by **CoolStar** ([@coolstarorg](https://github.com/coolstar)), creator of `sklhdaudbus` and the `csaudio` architecture.

### 1. Prerequisites

Before installing the audio drivers on your Intel-based Chromebook running Windows 11:
1. **Custom UEFI Firmware:** Your Chromebook must have full UEFI firmware flashed (e.g., via **MrChromebox coreboot/UEFI firmware**) with ChromeOS Write-Protect disabled.
2. **Complete 4-Driver Stack:** This repository now includes the complete audio stack: `SklHdaudBus` (Intel HD Audio / AudioDSP bus driver), `CsAudioIntcsof` (Intel SOF WaveRT driver), `Max98373Audio` (internal speakers), and `Nau8825Audio` (headphone jack & mic).
3. **Test Signing Enabled:** Because these drivers are self-built open source drivers, Windows 11 requires Test Signing to be turned on.
4. **Vulnerable Driver Blocklist:** `Install.cmd` disables the Microsoft vulnerable-driver blocklist after validating all package signatures. A reboot is required for this and Test Mode.

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

#### Option C: GitHub Actions
Run **Build Windows driver package** from the repository's Actions tab, or push to `main`/a `v*` tag. The workflow uses VS 2022, SDK/WDK 10.0.26100, KMDF 1.33, and the encrypted PFX configured in the repository Actions secrets. Download the `cros-sofaudio-*-windows-x64-attempt-*` artifact when the run completes.

---

### 3. Installation Steps

1. **Enable Test Mode (Administrator Command Prompt):**
   ```cmd
   bcdedit /set testsigning on
   ```
   *Reboot your Chromebook after setting this.*

2. **Automated Driver Installation:**
   Extract the release/Actions ZIP, then double-click `Install.cmd`. It elevates itself, verifies every binary and catalog, configures the required driver policies, and installs the complete stack.

   Alternatively, install manually via `pnputil`:
   ```cmd
   pnputil /add-driver ".\Drivers\Max98373Audio\Max98373Audio.inf" /install
   pnputil /add-driver ".\Drivers\Nau8825Audio\Nau8825Audio.inf" /install
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
Tools\cros_audio_test.exe
```
For a source-tree build, the same utility is at `x64\Release\CrosAudioTest.exe`.

This utility will:
1. Verify I2C communication by reading the silicon revision IDs:
   * **MAX98373:** Revision ID register `0x21FF` should return `0x43`.
   * **NAU88L25:** Device ID register `0x58` should return `0x001A`.
2. Monitor headphone jack plug/unplug events in real time:
   * Plug in headphones -> Displays `CONNECTED: Standard 3-Pole (TRS) Stereo Headphones`.
   * Plug in a smartphone headset -> Displays `CONNECTED: TRRS 4-Pole Headset with Microphone`.
   * Unplug -> Displays `UNPLUGGED`.
3. Test speaker and headphone DAC stream powering and pop-free volume ramping.

---

### 5. Uninstallation

If you ever need to uninstall the drivers and restore Windows default audio drivers:
1. Double-click `Uninstall.cmd` from the extracted package and approve elevation.
2. The script will:
   * Stop and remove the `csaudioswitcher` background service.
   * Remove all installed SOF driver packages (`csaudiointcsof`, `max98373audio`, `nau8825audio`, `sklhdaudbus`) from the Windows Driver Store via `pnputil /delete-driver /uninstall /force`.
   * Restore the vulnerable-driver blocklist value that existed before installation.
   * Clean up firmware files (`sof-*.ri`) from `C:\Windows\System32\drivers\`.
   * Rescan hardware devices to let Windows reload fallback drivers.
3. Reboot your system.
