# Chromebook Sound Open Firmware (SOF) Audio Reverse Engineering Specification
## Maxim MAX98373 Amplifier & Nuvoton NAU88L25 Codec Support for Windows 11

### 1. Architectural Overview & System Context

Modern Intel-based Chromebooks (Comet Lake, Tiger Lake, Jasper Lake, Alder Lake, and Meteor Lake) utilize **Sound Open Firmware (SOF)** within the Linux/ChromeOS kernel rather than standard High Definition Audio (HDA) codec configurations. In this design:
* The **Intel cAVS / ACE Audio DSP** offloads audio streams and manages hardware pipelines.
* The internal stereo speakers are powered by one or two **Maxim MAX98373** Class-D smart power amplifiers connected via an I2S/TDM link and controlled over an I2C bus.
* The 3.5mm analog audio jack and microphone input are handled by a **Nuvoton NAU88L25** high-performance audio codec, controlled over I2C and providing hardware-level jack detection through a dedicated GPIO interrupt.

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

---

### 2. Upstream Linux Machine Driver Analysis

In the upstream Linux kernel, this hardware combination is orchestrated by:
1. **Machine Driver:** `sound/soc/intel/boards/sof_nau8825.c`
2. **Speaker Common Helper:** `sound/soc/intel/boards/sof_maxim_common.c`
3. **Codec Component:** `sound/soc/codecs/nau8825.c` and `nau8825.h`
4. **Amplifier Component:** `sound/soc/codecs/max98373.c`, `max98373-i2c.c`, and `max98373.h`

#### DAI Link Topology
* **SSP0 / SSP1 (I2S/TDM):** Configured as a 4-slot or 2-slot TDM frame.
  * Slot 0: MAX98373 Left channel playback.
  * Slot 1: MAX98373 Right channel playback.
  * Slot 2 & 3: I/V sense feedback (current and voltage telemetry from speakers for excursion protection).
* **SSP1 / SSP0 (I2S):** Standard 48 kHz stereo I2S link for NAU88L25 DAC playback and ADC capture.

---

### 3. ACPI Hardware Mapping (DSDT Extraction)

When Coreboot/UEFI boots on a Chromebook with this audio subsystem, it describes the devices in ACPI as follows:

#### A. Maxim MAX98373 Amplifier
* **Hardware ID:** `ACPI\MX98373` or `ACPI\MAX98373`
* **Bus Resource:** `I2cSerialBusV2` on `I2C1` or `I2C2` with 7-bit slave address `0x31` (Left) or `0x32` (Right).
* **Properties (`_DSD`):**
  * `maxim,vmon-slot-no`: TDM slot index for voltage sensing (0 or 2)
  * `maxim,imon-slot-no`: TDM slot index for current sensing (1 or 3)
  * `maxim,spk-gain`: Default analog gain stage (`0x08` = +15.5 dB)

#### B. Nuvoton NAU88L25 Codec
* **Hardware ID:** `ACPI\10508825`, `ACPI\NVTN2000`, or `ACPI\NAU8825`
* **Bus Resource:** `I2cSerialBusV2` on `I2C0` or `I2C1` with 7-bit slave address `0x1A`.
* **Interrupt Resource:** `GpioInt` edge-triggered line connected to PCH GPIO controller.

---

### 4. Maxim MAX98373 Reverse-Engineered Register Control

The MAX98373 uses **16-bit register addresses** and **8-bit data words**:

| Register | Name | Value | Purpose |
| :--- | :--- | :--- | :--- |
| `0x2000` | `SW_RESET` | `0x01` | Triggers complete chip reset |
| `0x21FF` | `REVID` | `0x43` | Read-only Silicon Revision ID |
| `0x201E` | `PIN_DRIVE_STRENGTH`| `0x55` | Configures I2S pin drive strength |
| `0x2024` | `PCM_DATA_FMT_CFG` | `0xC0` | Standard I2S, rising edge clocking |
| `0x2026` | `PCM_CLOCK_RATIO` | `0x04` | 64x fs clock ratio (3.072 MHz for 48kHz) |
| `0x2027` | `PCM_SR_SETUP_1` | `0x08` | Configures 48 kHz PCM sample rate |
| `0x2029` | `PCM_TO_SPK_MONO_MIX_1` | `0x00`/`0x01` | Channel assignment (0=Left, 1=Right) |
| `0x202B` | `PCM_RX_EN` | `0x01` | Enables PCM receiver |
| `0x203D` | `AMP_DIG_VOL_CTRL`| `0x00` - `0xFF`| Digital volume attenuation (`0x00` = 0dB, `0xFF` = mute) |
| `0x203E` | `AMP_PATH_GAIN` | `0x08` | Gain staging (+15.5 dB) |
| `0x2043` | `AMP_EN` | `0x01` | Powers on the Class-D output stage |
| `0x20D1` | `DHT_CFG` | `0x01` | Dynamic Headroom Tracker configuration |
| `0x20FF` | `GLOBAL_SHDN` | `0x01` | De-asserts global shutdown; active operational mode |

#### Power Sequencing:
1. **Startup:** Soft reset (`0x2000` = 1) -> wait 10ms -> program DAI formatting -> set gain -> de-assert shutdown (`0x20FF` = 1) -> wait 1ms -> enable amp (`0x2043` = 1) -> unmute volume.
2. **Shutdown / Sleep:** Ramp volume to mute (`0x203D` = 0xFF) -> disable amp (`0x2043` = 0) -> enter global shutdown (`0x20FF` = 0).

---

### 5. Nuvoton NAU88L25 Reverse-Engineered Register Control

The NAU88L25 uses **16-bit register addresses** and **16-bit data words (big-endian)**:

| Register | Name | Value | Purpose |
| :--- | :--- | :--- | :--- |
| `0x00` | `RESET` | `0x0000` | Issues full software reset |
| `0x58` | `DEVICE_ID` | `0x001A` | Silicon identification register |
| `0x01` | `ENA_CTRL` | `0x07C2` | Enables DACL, DACR, ADC, clock gating, SAR |
| `0x03` | `CLK_DIVIDER` | `0x0050` | Master clock source and prescaler |
| `0x04`-`0x0A`| `FLL1`-`FLL_VCO_RSV` | Tables | FLL clock synthesizer for 48 kHz audio |
| `0x0C` | `HSD_CTRL` | `0x0000` | Headset speaker disconnect from ground |
| `0x0D` | `JACK_DET_CTRL` | `0x02D0` | Debounce: 250ms insert, 50ms eject |
| `0x0F` | `INTERRUPT_MASK` | `0xFEDE` | Unmasks insert, eject, and button interrupts |
| `0x10` | `IRQ_STATUS` | Read-only | Status bits for jack insert, eject, key press |
| `0x11` | `INT_CLR_KEY_STATUS`| `0xFFFF` | Write-1-to-clear interrupt status latch |
| `0x13` | `SAR_CTRL` | `0x1015` | Enables Successive Approximation ADC |
| `0x32` | `HSVOL_CTRL` | `0x0000` | Left & Right headphone volume and mute control |
| `0x59` | `SARDOUT_RAM_STATUS`| Read-only | Bit 14 indicates microphone presence |
| `0x74` | `MIC_BIAS` | `0x0102` | Powers on 2.2V low-noise microphone bias |
| `0x7F` | `POWER_UP_CONTROL` | `0x003C` | Powers up multi-stage output drivers |
| `0x80` | `CHARGE_PUMP` | `0x0600` | Pop-free charge pump enable |

#### Jack Detection State Machine:
1. **Interrupt Fired:** Hardware pulls GPIO interrupt pin low/high.
2. **ISR:** Handled at `DIRQL`, queues a passive-level work item (`Nau8825_EvtInterruptWorkItem`).
3. **Register Read:** Work item reads `NAU8825_REG_IRQ_STATUS` (0x10) over I2C.
4. **Classification:**
   * If `JACK_INSERT_DETECTED` (bit 0): Queries `NAU8825_REG_SARDOUT_RAM_STATUS` (0x59). If bit 14 is set, classifies as **4-Pole TRRS Headset with Microphone**; otherwise classifies as **3-Pole TRS Stereo Headphones**.
   * If `JACK_EJECT_DETECTED` (bit 2): Classifies as **Unplugged**.
5. **Notification:** Completes any pending asynchronous IRPs in `JackNotificationQueue` so Windows Audio immediately switches the active default endpoint.

---

### 6. Windows 11 KMDF Driver Architecture

Because Windows audio uses PortCls / WaveRT rather than ALSA, the driver stack is split into:
1. **Host Audio Bus / DSP Driver:** Manages DMA streaming on the Intel AudioDSP.
2. **Codec KMDF Satellite Drivers:**
   * `Max98373Audio.sys` (ACPI `MX98373`)
   * `Nau8825Audio.sys` (ACPI `10508825`)

Both drivers register `GUID_DEVINTERFACE_CSAUDIO_CODEC` and standard device interfaces, exposing IOCTLs for:
* Stream Start / Stop
* Digital Volume & Mute Control
* Live Silicon Register Read / Write (debugging and tuning)
* Asynchronous Jack Insertion / Ejection Event Notification
