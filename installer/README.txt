CHROMEBOOK AUDIO FOR WINDOWS - Intel Alder Lake + MAX98373 + NAU88L25
=====================================================================

Supported laptops
  Chromebooks with an Intel Alder Lake-P/M processor, a Nuvoton NAU88L25
  headphone codec and Maxim MAX98373 speaker amps - the hardware Linux drives
  with the "sof-adl-max98373-nau8825" configuration (e.g. Acer Chromebook
  Vero 514 "volmar"). The installer checks this and stops on anything else.
  Windows must run in Test Mode (the drivers are test-signed); the installer
  can switch it on (Secure Boot must be off for that).

Install
  1. Double-click Install.cmd (it asks for administrator rights).
  2. Reboot.
  The installer verifies every driver signature, disables the Microsoft
  Vulnerable Driver Blocklist for these test-signed drivers, and enables Test
  Mode with permission. After installing an update, it removes superseded
  cros-sofaudio oem*.inf packages from Driver Store while retaining the newest
  package for each driver. These policy changes require the reboot in step 2.
  Windows then shows "Speakers" and "Headphones". The auto-switcher moves
  playback to Headphones when you plug in and back to Speakers when you
  unplug.

What it contains
  Drivers\SklHdaudBus     Intel audio bus driver (CoolStar) - only installed
                          when the laptop has no working bus driver yet
  Drivers\CsAudioIntcsof  boots Intel Sound Open Firmware on the audio DSP,
                          speaker/headphone playback and DMIC/headset capture
                          pipelines (SSP1, SSP0 and DMIC0)
  Drivers\Max98373Audio   speaker amps
  Drivers\Nau8825Audio    headphone codec + jack detection
  Switcher\               auto-switch service (csaudioswitcher)
  Tools\cros_audio_test.exe  signed codec and jack diagnostic utility

Volume tuning (registry, HKLM\SOFTWARE\CrosSofAudio, DWORD; reboot after)
  SpeakerDspAttenDb       speaker attenuation in dB, default 2 (~79% amplitude)
  SpeakerAmpAttenHalfDb   extra amp attenuation in 0.5 dB steps, default 0
  SpeakerLimiterThresholdDb  peak limiter headroom in dBFS, default 2
                             (valid 0-15; larger is more protective)
  HeadphoneDspAttenDb     headphone attenuation in dB, default 20
  Example (6 dB quieter speakers):
    reg add HKLM\SOFTWARE\CrosSofAudio /v SpeakerDspAttenDb /t REG_DWORD /d 6 /f
  The MAX98373 limiter uses a fast attack and smooth release to reduce peak
  clipping. If the speakers still distort at full Windows volume, use 3-6 dB
  of SpeakerDspAttenDb. The current SOF speaker pipeline has no tonal EQ block,
  so this release does not apply unverified speaker-specific EQ coefficients.

Jack detection
  Default JackActiveLow is 1 for volmar. If a different board reports the
  opposite GPIO polarity (plugging in switches to Speakers instead of
  Headphones), set:
    reg add HKLM\SOFTWARE\CrosSofAudio /v JackActiveLow /t REG_DWORD /d 0 /f
  and reboot.

Problems
  - Collect-Logs.cmd writes audio-log.txt with everything needed to debug.
  - A TRRS headset should set Codec\MicDetectCode to 1 (OMTP) or 2 (CTIA).
    DspIpc\MicJackPipelineResult and DspIpc\DmicPipelineResult report whether
    the two microphone DSP pipelines were created successfully.
  - Uninstall.cmd removes everything, restores the prior vulnerable-driver
    blocklist setting, and lets Windows fall back to older drivers.
  - Disable the DSP boot:
      reg add HKLM\SOFTWARE\CrosSofAudio /v EnableDspBoot /t REG_DWORD /d 0 /f
  - Blue screen at boot: start Safe Mode and run Uninstall.cmd.

Credits / licences: CoolStar (SklHdaudBus, csaudio architecture), isaiahscape
(cros-sofaudio), Kier (@3kierz) & Claude (fixing sound output drivers), Linux
SOF / ASoC drivers (sequences ported), Intel SOF firmware
(LICENCE.sof-firmware, BSD-3-Clause, redistributable).
