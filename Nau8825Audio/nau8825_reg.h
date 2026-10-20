/*
 * nau8825_reg.h — Nuvoton NAU88L25 Audio Codec & Jack Detection Register Definitions
 *
 * Reverse-engineered from Linux kernel ALSA SoC sound driver (sound/soc/codecs/nau8825.h)
 * and ChromeOS SOF topologies (sof-tgl-nau8825-*.tplg).
 *
 * All registers on NAU88L25 use 16-bit addresses and 16-bit data words (big-endian).
 *
 * Copyright (c) 2026. Licensed under the MIT License.
 */

#pragma once

/* =========================================================================
 * I2C Slave Address & Device Identification
 * ========================================================================= */

#define NAU8825_I2C_ADDR_DEFAULT            0x1A   /* 7-bit primary address (0x34 write / 0x35 read) */
#define NAU8825_REG_I2C_DEVICE_ID           0x58   /* Silicon ID register */
#define NAU8825_DEVICE_ID_VALUE             0x001A /* Device ID for NAU88L25 */

/* =========================================================================
 * Register Memory Map (16-bit address offsets)
 * ========================================================================= */

#define NAU8825_REG_RESET                   0x00
#define NAU8825_REG_ENA_CTRL                0x01
#define NAU8825_REG_IIC_ADDR_SET            0x02
#define NAU8825_REG_CLK_DIVIDER             0x03
#define NAU8825_REG_FLL1                    0x04
#define NAU8825_REG_FLL2                    0x05
#define NAU8825_REG_FLL3                    0x06
#define NAU8825_REG_FLL4                    0x07
#define NAU8825_REG_FLL5                    0x08
#define NAU8825_REG_FLL6                    0x09
#define NAU8825_REG_FLL_VCO_RSV             0x0A

/* Headset Detection & Interrupt Control */
#define NAU8825_REG_HSD_CTRL                0x0C
#define NAU8825_REG_JACK_DET_CTRL           0x0D
#define NAU8825_REG_INTERRUPT_MASK          0x0F
#define NAU8825_REG_IRQ_STATUS              0x10
#define NAU8825_REG_INT_CLR_KEY_STATUS      0x11
#define NAU8825_REG_INTERRUPT_DIS_CTRL      0x12
#define NAU8825_REG_SAR_CTRL                0x13
#define NAU8825_REG_KEYDET_CTRL             0x14
#define NAU8825_REG_VDET_THRESHOLD_1        0x15
#define NAU8825_REG_VDET_THRESHOLD_2        0x16
#define NAU8825_REG_VDET_THRESHOLD_3        0x17
#define NAU8825_REG_VDET_THRESHOLD_4        0x18

/* GPIO & Audio Interface */
#define NAU8825_REG_GPIO34_CTRL             0x19
#define NAU8825_REG_GPIO12_CTRL             0x1A
#define NAU8825_REG_TDM_CTRL                0x1B
#define NAU8825_REG_I2S_PCM_CTRL1           0x1C
#define NAU8825_REG_I2S_PCM_CTRL2           0x1D
#define NAU8825_REG_LEFT_TIME_SLOT          0x1E
#define NAU8825_REG_RIGHT_TIME_SLOT         0x1F

/* Equalizer / Bi-quad Filters */
#define NAU8825_REG_BIQ_CTRL                0x20
#define NAU8825_REG_ADC_RATE                0x2B
#define NAU8825_REG_DAC_CTRL1               0x2C
#define NAU8825_REG_DAC_CTRL2               0x2D
#define NAU8825_REG_DAC_DGAIN_CTRL          0x2F
#define NAU8825_REG_ADC_DGAIN_CTRL          0x30
#define NAU8825_REG_MUTE_CTRL               0x31
#define NAU8825_REG_HSVOL_CTRL              0x32
#define NAU8825_REG_DACL_CTRL               0x33
#define NAU8825_REG_DACR_CTRL               0x34

/* Impedance Measurement & Power Control */
#define NAU8825_REG_IMM_MODE_CTRL           0x4C
#define NAU8825_REG_IMM_RMS_L               0x4D
#define NAU8825_REG_IMM_RMS_R               0x4E
#define NAU8825_REG_CLASSG_CTRL             0x50
#define NAU8825_REG_OPT_EFUSE_CTRL          0x51
#define NAU8825_REG_MISC_CTRL               0x55
#define NAU8825_REG_SARDOUT_RAM_STATUS      0x59
#define NAU8825_REG_FLL2_LOWER              0x5A
#define NAU8825_REG_FLL2_UPPER              0x5B
#define NAU8825_REG_BIAS_ADJ                0x66
#define NAU8825_REG_TRIM_SETTINGS           0x68
#define NAU8825_REG_ANALOG_CONTROL_1        0x69
#define NAU8825_REG_ANALOG_CONTROL_2        0x6A
#define NAU8825_REG_ANALOG_ADC_1            0x71
#define NAU8825_REG_ANALOG_ADC_2            0x72
#define NAU8825_REG_RDAC                    0x73
#define NAU8825_REG_MIC_BIAS                0x74
#define NAU8825_REG_BOOST                   0x76
#define NAU8825_REG_FEPGA                   0x77
#define NAU8825_REG_POWER_UP_CONTROL        0x7F
#define NAU8825_REG_CHARGE_PUMP             0x80
#define NAU8825_REG_CHARGE_PUMP_INPUT_READ  0x81
#define NAU8825_REG_GENERAL_STATUS          0x82

/* =========================================================================
 * Bitfield Masks and Encodings
 * ========================================================================= */

/* ENA_CTRL (0x01) */
#define NAU8825_ENABLE_DACR                 (1 << 10)
#define NAU8825_ENABLE_DACL                 (1 << 9)
#define NAU8825_ENABLE_ADC                  (1 << 8)
#define NAU8825_ENABLE_ADC_CLK              (1 << 7)
#define NAU8825_ENABLE_DAC_CLK              (1 << 6)
#define NAU8825_ENABLE_SAR                  (1 << 1)

/* CLK_DIVIDER (0x03) */
#define NAU8825_CLK_SRC_MASK                (7 << 12)
#define NAU8825_CLK_SRC_MCLK                (0 << 12)
#define NAU8825_CLK_SRC_VCO                 (2 << 12)
#define NAU8825_CLK_MCLK_SRC_MASK           (0xF << 0)

/* HSD_CTRL (0x0C) */
#define NAU8825_HSD_AUTO_MODE               (1 << 6)
#define NAU8825_SPKR_DWN1R                  (1 << 1)
#define NAU8825_SPKR_DWN1L                  (1 << 0)

/* JACK_DET_CTRL (0x0D) */
#define NAU8825_JACK_DET_RESTART            (1 << 9)
#define NAU8825_JACK_DET_DB_BYPASS          (1 << 8)
#define NAU8825_JACK_INSERT_DEBOUNCE_MASK   (7 << 5)
#define NAU8825_JACK_EJECT_DEBOUNCE_MASK    (7 << 2)
#define NAU8825_JACK_POLARITY               (1 << 1)

/* INTERRUPT_MASK (0x0F) - 0 = Enable, 1 = Mask */
#define NAU8825_IRQ_OUTPUT_EN               (1 << 11) /* Active high / low */
#define NAU8825_IRQ_RMS_EN                  (1 << 8)
#define NAU8825_IRQ_KEY_RELEASE_EN          (1 << 7)
#define NAU8825_IRQ_KEY_SHORT_PRESS_EN      (1 << 5)
#define NAU8825_IRQ_EJECT_EN                (1 << 2)
#define NAU8825_IRQ_INSERT_EN               (1 << 0)

/* IRQ_STATUS (0x10) */
#define NAU8825_JACK_EJECT_DETECTED         (1 << 2)
#define NAU8825_JACK_INSERT_DETECTED        (1 << 0)
#define NAU8825_KEY_SHORT_PRESS             (1 << 5)
#define NAU8825_KEY_RELEASE                 (1 << 7)

/* SAR_CTRL (0x13) */
#define NAU8825_SAR_ADC_EN                  (1 << 12)
#define NAU8825_SAR_COMPARE_TIME_MASK       (7 << 8)
#define NAU8825_SAR_SAMPLING_TIME_MASK      (7 << 4)
#define NAU8825_SAR_TRACKING_GAIN_MASK      (7 << 0)

/* SARDOUT_RAM_STATUS (0x59) - Impedance / Headset Detection */
#define NAU8825_MICBIAS_FLT                 (1 << 15)
#define NAU8825_MIC_DETECTED_STATUS         (1 << 14)
#define NAU8825_SAR_DATA_MASK               (0xFF << 0)

/* HSVOL_CTRL (0x32) */
#define NAU8825_HPR_MUTE                    (1 << 14)
#define NAU8825_HPL_MUTE                    (1 << 15)
#define NAU8825_HPL_VOL_MASK                (0x3F << 8)
#define NAU8825_HPR_VOL_MASK                (0x3F << 0)
#define NAU8825_HP_VOL_0DB                  0x00
#define NAU8825_HP_VOL_MIN                  0x36 /* -54 dB */

/* CHARGE_PUMP (0x80) */
#define NAU8825_JAMNODCLOW                  (1 << 10)
#define NAU8825_CHARGE_PUMP_EN              (1 << 9)
#define NAU8825_POWER_DOWN_DACR             (1 << 8)
#define NAU8825_POWER_DOWN_DACL             (1 << 7)

/* POWER_UP_CONTROL (0x7F) */
#define NAU8825_POWERUP_HP_DRV_R            (1 << 3)
#define NAU8825_POWERUP_HP_DRV_L            (1 << 2)
#define NAU8825_POWERUP_INTEGR_R            (1 << 1)
#define NAU8825_POWERUP_INTEGR_L            (1 << 0)
#define NAU8825_POWERUP_DRV_IN_R            (1 << 5)
#define NAU8825_POWERUP_DRV_IN_L            (1 << 4)

/* RDAC (0x73) */
#define NAU8825_RDAC_EN                     (1 << 12)
#define NAU8825_RDAC_CLK_EN                 (1 << 13)
#define NAU8825_RDAC_FS_BCLK_ENB            (1 << 1)

/* BOOST (0x76) */
#define NAU8825_HP_BOOST_DIS                (1 << 9)

/* CLASSG_CTRL (0x50) */
#define NAU8825_CLASSG_LDAC_EN              (1 << 1)
#define NAU8825_CLASSG_RDAC_EN              (1 << 0)

/* MIC_BIAS (0x74) */
#define NAU8825_MICBIAS_POWERUP             (1 << 8)
#define NAU8825_MICBIAS_VOLTAGE_2_8V        (3 << 0)
#define NAU8825_MICBIAS_VOLTAGE_2_2V        (2 << 0)
