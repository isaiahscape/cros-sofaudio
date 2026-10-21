/*
 * max98373_reg.h — Maxim MAX98373 Smart Amplifier Register Definitions
 *
 * Reverse-engineered from Linux kernel ALSA SoC sound driver (sound/soc/codecs/max98373.h)
 * and ChromeOS SOF topologies (sof-tgl-max98373-*.tplg).
 *
 * Copyright (c) 2026. Licensed under the MIT License.
 */

#pragma once

/* =========================================================================
 * I2C Slave Addresses & Device Identification
 * ========================================================================= */

#define MAX98373_I2C_ADDR_DEFAULT           0x31   /* 7-bit primary address (Left speaker) */
#define MAX98373_I2C_ADDR_SECONDARY         0x32   /* 7-bit secondary address (Right speaker) */
#define MAX98373_I2C_ADDR_ALT1              0x38
#define MAX98373_I2C_ADDR_ALT2              0x39

#define MAX98373_REVID_REG                  0x21FF /* Device Revision ID register */
#define MAX98373_REV_ID_VALUE               0x43   /* Silicon revision identifier */

/* =========================================================================
 * Register Memory Map (16-bit address offsets)
 * ========================================================================= */

/* Software Reset & Interrupts */
#define MAX98373_R2000_SW_RESET             0x2000
#define MAX98373_R2001_INT_RAW1             0x2001
#define MAX98373_R2002_INT_RAW2             0x2002
#define MAX98373_R2003_INT_RAW3             0x2003
#define MAX98373_R2004_INT_STATE1           0x2004
#define MAX98373_R2005_INT_STATE2           0x2005
#define MAX98373_R2006_INT_STATE3           0x2006
#define MAX98373_R2007_INT_FLAG1            0x2007
#define MAX98373_R2008_INT_FLAG2            0x2008
#define MAX98373_R2009_INT_FLAG3            0x2009
#define MAX98373_R200A_INT_EN1              0x200A
#define MAX98373_R200B_INT_EN2              0x200B
#define MAX98373_R200C_INT_EN3              0x200C
#define MAX98373_R200D_INT_FLAG_CLR1        0x200D
#define MAX98373_R200E_INT_FLAG_CLR2        0x200E
#define MAX98373_R200F_INT_FLAG_CLR3        0x200F
#define MAX98373_R2010_IRQ_CTRL             0x2010

/* Thermal Foldback & Protection */
#define MAX98373_R2014_THERM_WARN_THRESH    0x2014
#define MAX98373_R2015_THERM_SHDN_THRESH    0x2015
#define MAX98373_R2016_THERM_HYSTERESIS     0x2016
#define MAX98373_R2017_THERM_FOLDBACK_SET   0x2017
#define MAX98373_R2018_THERM_FOLDBACK_EN    0x2018
#define MAX98373_R201E_PIN_DRIVE_STRENGTH   0x201E

/* PCM & TDM Digital Audio Interface Configuration */
#define MAX98373_R2020_PCM_TX_HIZ_EN_1      0x2020
#define MAX98373_R2021_PCM_TX_HIZ_EN_2      0x2021
#define MAX98373_R2022_PCM_TX_SRC_1         0x2022
#define MAX98373_R2023_PCM_TX_SRC_2         0x2023
#define MAX98373_R2024_PCM_DATA_FMT_CFG     0x2024
#define MAX98373_R2025_AUDIO_IF_MODE        0x2025
#define MAX98373_R2026_PCM_CLOCK_RATIO      0x2026
#define MAX98373_R2027_PCM_SR_SETUP_1       0x2027
#define MAX98373_R2028_PCM_SR_SETUP_2       0x2028
#define MAX98373_R2029_PCM_TO_SPK_MONO_MIX_1 0x2029
#define MAX98373_R202A_PCM_TO_SPK_MONO_MIX_2 0x202A
#define MAX98373_R202B_PCM_RX_EN            0x202B
#define MAX98373_R202C_PCM_TX_EN            0x202C

/* Inter-Chip Communication (ICC) */
#define MAX98373_R202E_ICC_RX_CH_EN_1       0x202E
#define MAX98373_R202F_ICC_RX_CH_EN_2       0x202F
#define MAX98373_R2030_ICC_TX_HIZ_EN_1      0x2030
#define MAX98373_R2031_ICC_TX_HIZ_EN_2      0x2031
#define MAX98373_R2032_ICC_LINK_EN_CFG      0x2032
#define MAX98373_R2034_ICC_TX_CNTL          0x2034
#define MAX98373_R2035_ICC_TX_EN            0x2035
#define MAX98373_R2036_SOUNDWIRE_CTRL       0x2036

/* Digital Volume, Path Gain, and Amplifier Power Control */
#define MAX98373_R203D_AMP_DIG_VOL_CTRL     0x203D
#define MAX98373_R203E_AMP_PATH_GAIN        0x203E
#define MAX98373_R203F_AMP_DSP_CFG          0x203F
#define MAX98373_R2040_TONE_GEN_CFG         0x2040
#define MAX98373_R2041_AMP_CFG              0x2041
#define MAX98373_R2042_AMP_EDGE_RATE_CFG    0x2042
#define MAX98373_R2043_AMP_EN               0x2043

/* Current & Voltage (I/V) Sense ADCs (for impedance monitoring) */
#define MAX98373_R2046_IV_SENSE_ADC_DSP_CFG 0x2046
#define MAX98373_R2047_IV_SENSE_ADC_EN      0x2047
#define MAX98373_R2051_MEAS_ADC_SAMPLING_RATE 0x2051
#define MAX98373_R2052_MEAS_ADC_PVDD_FLT_CFG  0x2052
#define MAX98373_R2053_MEAS_ADC_THERM_FLT_CFG 0x2053
#define MAX98373_R2054_MEAS_ADC_PVDD_CH_READBACK 0x2054
#define MAX98373_R2055_MEAS_ADC_THERM_CH_READBACK 0x2055
#define MAX98373_R2056_MEAS_ADC_PVDD_CH_EN  0x2056

/* Brownout Prevention Engine (BDE) */
#define MAX98373_R2090_BDE_LVL_HOLD         0x2090
#define MAX98373_R2091_BDE_GAIN_ATK_REL_RATE 0x2091
#define MAX98373_R2092_BDE_CLIPPER_MODE     0x2092
#define MAX98373_R2097_BDE_L1_THRESH        0x2097
#define MAX98373_R2098_BDE_L2_THRESH        0x2098
#define MAX98373_R2099_BDE_L3_THRESH        0x2099
#define MAX98373_R209A_BDE_L4_THRESH        0x209A
#define MAX98373_R209B_BDE_THRESH_HYST      0x209B
#define MAX98373_R20A8_BDE_L1_CFG_1         0x20A8
#define MAX98373_R20B1_BDE_L4_CFG_1         0x20B1
#define MAX98373_R20B5_BDE_EN               0x20B5
#define MAX98373_R20B6_BDE_CUR_STATE_READBACK 0x20B6

/* Dynamic Headroom Tracker (DHT) & Limiter */
#define MAX98373_R20D1_DHT_CFG              0x20D1
#define MAX98373_R20D2_DHT_ATTACK_CFG       0x20D2
#define MAX98373_R20D3_DHT_RELEASE_CFG      0x20D3
#define MAX98373_R20D4_DHT_EN               0x20D4
#define MAX98373_R20E0_LIMITER_THRESH_CFG   0x20E0
#define MAX98373_R20E1_LIMITER_ATK_REL_RATES 0x20E1
#define MAX98373_R20E2_LIMITER_EN           0x20E2

/* Global Shutdown and Auto-Restart */
#define MAX98373_R20FE_DEVICE_AUTO_RESTART_CFG 0x20FE
#define MAX98373_R20FF_GLOBAL_SHDN          0x20FF

/* =========================================================================
 * Bitfield Masks and Encodings
 * ========================================================================= */

/* SW_RESET (0x2000) */
#define MAX98373_SW_RESET_ASSERT            0x01

/* GLOBAL_SHDN (0x20FF) */
#define MAX98373_GLOBAL_SHDN_DIS            0x01  /* 1 = Wake / Active, 0 = Power-down */

/* AMP_EN (0x2043) */
#define MAX98373_AMP_EN_MASK                0x01
#define MAX98373_AMP_ENABLE                 0x01
#define MAX98373_AMP_DISABLE                0x00

/* PCM_DATA_FMT_CFG (0x2024) - Matching Linux & OpenGMaxCodec */
#define MAX98373_PCM_MODE_CFG_FORMAT_MASK   (0x7 << 3)
#define MAX98373_PCM_MODE_CFG_FORMAT_SHIFT  (3)
#define MAX98373_PCM_TX_CH_INTERLEAVE_MASK  (0x1 << 2)
#define MAX98373_PCM_FORMAT_I2S             (0x0 << 0)
#define MAX98373_PCM_FORMAT_LJ              (0x1 << 0)
#define MAX98373_PCM_FORMAT_TDM_MODE0       (0x3 << 0)
#define MAX98373_PCM_FORMAT_TDM_MODE1       (0x4 << 0)
#define MAX98373_PCM_FORMAT_TDM_MODE2       (0x5 << 0)
#define MAX98373_PCM_FMT_I2S                (0x0 << 3)
#define MAX98373_PCM_FMT_LEFT_JUST          (0x1 << 3)
#define MAX98373_PCM_FMT_TDM_MODE0          (0x3 << 3)
#define MAX98373_PCM_FMT_TDM_MODE1          (0x4 << 3)
#define MAX98373_PCM_MODE_CFG_CHANSZ_16     (0x1 << 6)
#define MAX98373_PCM_MODE_CFG_CHANSZ_24     (0x2 << 6)
#define MAX98373_PCM_MODE_CFG_CHANSZ_32     (0x3 << 6)
#define MAX98373_PCM_MODE_CFG_CHANSZ_MASK   (0x3 << 6)

/* PCM_CLOCK_RATIO (0x2026) */
#define MAX98373_PCM_BCLKEDGE_RISING        (1 << 4)
#define MAX98373_CLOCK_RATIO_16             0x01  /* BCLK / LRCLK = 16 */
#define MAX98373_CLOCK_RATIO_32             0x02  /* BCLK / LRCLK = 32 */
#define MAX98373_CLOCK_RATIO_64             0x04  /* BCLK / LRCLK = 64 (Standard 2x32) */
#define MAX98373_CLOCK_RATIO_128            0x06  /* BCLK / LRCLK = 128 */
#define MAX98373_CLOCK_RATIO_256            0x08  /* BCLK / LRCLK = 256 (TDM) */

/* PCM_SR_SETUP_1 (0x2027) */
#define MAX98373_PCM_SR_8KHZ                0x00
#define MAX98373_PCM_SR_16KHZ               0x02
#define MAX98373_PCM_SR_44_1KHZ             0x07
#define MAX98373_PCM_SR_48KHZ               0x08  /* Standard ChromeOS SOF rate */
#define MAX98373_PCM_SR_96KHZ               0x0A

/* AMP_PATH_GAIN (0x203E) - Analog & Digital Gain */
#define MAX98373_AMP_PATH_GAIN_MASK         0x3F
#define MAX98373_SPK_GAIN_DEFAULT           0x0A  /* +18.0 dB */

/* AMP_DIG_VOL_CTRL (0x203D) */
#define MAX98373_AMP_VOL_0DB                0x00  /* 0 dB */
#define MAX98373_AMP_VOL_MUTE               0x7F  /* Mute / -63.5 dB */
