/*
 * nau8825_seq.h - NAU88L25 register sequences for the volmar board
 *
 * Register/bit values and the order of operations follow the Linux kernel
 * driver (sound/soc/codecs/nau8825.c, nau8825.h) and the Intel machine driver
 * (sound/soc/intel/boards/sof_nau8825.c):
 *
 *   - init:     nau8825_init_regs() with the coreboot ACPI properties of
 *               google/brya/variants/volmar (jkdet enable, no pull, active low,
 *               vref_impedance 2, micbias 6, debounce 7/7)
 *   - playback: set_dai_fmt (I2S, codec clock consumer), hw_params (32-bit,
 *               DAC OSR 64), sysclk FLL_BLK + set_pll(BCLK 3.072 MHz ->
 *               256*fs) and the DAPM power-up order of the headphone path
 *   - stop:     DAPM power-down in reverse, then system_clock_control()
 *
 * Bit definitions use a NAUL_ prefix so they cannot collide with the
 * (partly incorrect) definitions in nau8825_reg.h.
 */
#pragma once

/* ---- register addresses (identical to nau8825_reg.h / Linux) -------- */
#define NAUL_RESET              0x00
#define NAUL_ENA_CTRL           0x01
#define NAUL_IIC_ADDR_SET       0x02
#define NAUL_CLK_DIVIDER        0x03
#define NAUL_FLL1               0x04
#define NAUL_FLL2               0x05
#define NAUL_FLL3               0x06
#define NAUL_FLL4               0x07
#define NAUL_FLL5               0x08
#define NAUL_FLL6               0x09
#define NAUL_HSD_CTRL           0x0C
#define NAUL_JACK_DET_CTRL      0x0D
#define NAUL_INTERRUPT_MASK     0x0F
#define NAUL_GPIO12_CTRL        0x1A
#define NAUL_I2S_PCM_CTRL1      0x1C
#define NAUL_I2S_PCM_CTRL2      0x1D
#define NAUL_LEFT_TIME_SLOT     0x1E
#define NAUL_ADC_RATE           0x2B
#define NAUL_DAC_CTRL1          0x2C
#define NAUL_MUTE_CTRL          0x31
#define NAUL_HSVOL_CTRL         0x32
#define NAUL_DACL_CTRL          0x33
#define NAUL_DACR_CTRL          0x34
#define NAUL_CLASSG_CTRL        0x50
#define NAUL_I2C_DEVICE_ID      0x58
#define NAUL_FLL2_LOWER         0x5A
#define NAUL_FLL2_UPPER         0x5B
#define NAUL_BIAS_ADJ           0x66
#define NAUL_ANALOG_CONTROL_2   0x6A
#define NAUL_RDAC               0x73
#define NAUL_MIC_BIAS           0x74
#define NAUL_BOOST              0x76
#define NAUL_POWER_UP_CONTROL   0x7F
#define NAUL_CHARGE_PUMP        0x80

/* ---- bits ------------------------------------------------------------ */
#define NAUL_ENABLE_DACR        (1u << 10)
#define NAUL_ENABLE_DACL        (1u << 9)
#define NAUL_ENABLE_DAC_CLK     (1u << 6)

#define NAUL_CLK_SRC_MASK       (1u << 15)
#define NAUL_CLK_SRC_VCO        (1u << 15)
#define NAUL_CLK_SRC_MCLK       0
#define NAUL_CLK_DAC_SRC_MASK   (0x3u << 4)
#define NAUL_CLK_MCLK_SRC_MASK  0xFu

#define NAUL_ICTRL_LATCH_MASK   (0x7u << 10)
#define NAUL_FLL_RATIO_MASK     0x7Fu
#define NAUL_GAIN_ERR_MASK      (0xFu << 12)
#define NAUL_FLL_CLK_SRC_MASK   (0x3u << 10)
#define NAUL_FLL_CLK_SRC_BLK    (0x2u << 10)
#define NAUL_FLL_INTEGER_MASK   0x3FFu
#define NAUL_FLL_REF_DIV_MASK   (0x3u << 10)
#define NAUL_FLL_PDB_DAC_EN     (1u << 15)
#define NAUL_FLL_LOOP_FTR_EN    (1u << 14)
#define NAUL_FLL_CLK_SW_MASK    (1u << 13)
#define NAUL_FLL_FTR_SW_MASK    (1u << 12)
#define NAUL_FLL_FTR_SW_ACCU    (1u << 12)
#define NAUL_DCO_EN             (1u << 15)
#define NAUL_SDM_EN             (1u << 14)
#define NAUL_CUTOFF500          (1u << 13)

#define NAUL_HSD_HPL_GND        (1u << 0)   /* "HPOL Pulldown" (inverted widget) */
#define NAUL_HSD_HPR_GND        (1u << 1)

#define NAUL_JACK_POLARITY      (1u << 1)
#define NAUL_JACK_INSERT_DEB_MASK (0x7u << 5)
#define NAUL_JACK_EJECT_DEB_MASK  (0x7u << 2)

#define NAUL_IRQ_PIN_PULLUP     (1u << 14)
#define NAUL_IRQ_PIN_PULL_EN    (1u << 13)

#define NAUL_JKDET_PULL_UP      (1u << 11)
#define NAUL_JKDET_PULL_EN      (1u << 9)
#define NAUL_JKDET_OUTPUT_EN    (1u << 8)

#define NAUL_I2S_BP_MASK        (1u << 7)
#define NAUL_I2S_PCMB_MASK      (1u << 6)
#define NAUL_I2S_DL_MASK        (0x3u << 2)
#define NAUL_I2S_DL_32          (0x3u << 2)
#define NAUL_I2S_DF_MASK        0x3u
#define NAUL_I2S_DF_I2S         0x2u
#define NAUL_I2S_MS_MASK        (1u << 3)

#define NAUL_DIS_FS_SHORT_DET   (1u << 13)
#define NAUL_ADC_SINC4_EN       (1u << 4)
#define NAUL_ADC_SYNC_DOWN_MASK 0x3u
#define NAUL_ADC_SYNC_DOWN_64   1u
#define NAUL_DAC_CLIP_OFF       (1u << 7)
#define NAUL_DAC_OVERSAMPLE_MASK 0x7u
#define NAUL_DAC_OVERSAMPLE_64  0u

#define NAUL_HP_MUTE            (1u << 15)
#define NAUL_HPL_MUTE           (1u << 13)
#define NAUL_HPR_MUTE           (1u << 12)
#define NAUL_HPL_VOL_SFT        6
#define NAUL_HP_VOL_MASK        0x3Fu

#define NAUL_DAC_CH_SEL_MASK    (1u << 9)
#define NAUL_DAC_CH_SEL_L       0
#define NAUL_DAC_CH_SEL_R       (1u << 9)

#define NAUL_CLASSG_TIMER_MASK  (0x3Fu << 8)
#define NAUL_CLASSG_TIMER_64MS  (0x20u << 8)
#define NAUL_CLASSG_LDAC_EN     (1u << 2)   /* "HP amp L" */
#define NAUL_CLASSG_RDAC_EN     (1u << 1)   /* "HP amp R" */
#define NAUL_CLASSG_EN          (1u << 0)   /* "Class G"  */

#define NAUL_SOFTWARE_ID_MASK   0x3u
#define NAUL_SW_ID_NAU8825      0x0u
#define NAUL_SW_ID_NAU8825C     0x1u
#define NAUL_GPIO2JD1           (1u << 7)

#define NAUL_BIAS_TESTDAC_EN    (0x3u << 8)
#define NAUL_BIAS_VMID          (1u << 6)
#define NAUL_BIAS_VMID_SEL_MASK (0x3u << 4)

#define NAUL_HP_NON_CLASSG_2X   (1u << 12)
#define NAUL_DAC_CAP_MSB        (1u << 1)
#define NAUL_DAC_CAP_LSB        (1u << 0)

#define NAUL_RDAC_EN            (0x3u << 12)   /* "ADACL"/"ADACR" */
#define NAUL_RDAC_CLK_EN        (0x3u << 8)    /* "ADACL/R Clock" */
#define NAUL_RDAC_CLK_DELAY_MASK (0x7u << 4)
#define NAUL_RDAC_VREF_MASK     (0x3u << 2)

#define NAUL_MICBIAS_VOLTAGE_MASK 0x7u

#define NAUL_PRECHARGE_DIS      (1u << 13)
#define NAUL_GLOBAL_BIAS_EN     (1u << 12)
#define NAUL_HP_BOOST_DIS       (1u << 9)      /* "HP Boost Driver" (inverted) */
#define NAUL_HP_BOOST_G_DIS     (1u << 8)
#define NAUL_SHORT_SHUTDOWN_EN  (1u << 6)

#define NAUL_PU_OUT_STAGE1      ((1u << 5) | (1u << 4))  /* R | L */
#define NAUL_PU_OUT_STAGE2      ((1u << 3) | (1u << 2))
#define NAUL_PU_OUT_STAGE3      ((1u << 1) | (1u << 0))

#define NAUL_ADCOUT_DS_MASK     (1u << 12)
#define NAUL_JAMNODCLOW         (1u << 10)
#define NAUL_POWER_DOWN_DACR    (1u << 9)
#define NAUL_POWER_DOWN_DACL    (1u << 8)
#define NAUL_CHARGE_PUMP_EN     (1u << 5)      /* "Charge Pump" */

/* ---- volmar board values (coreboot overridetree.cb) ------------------ */
#define VOLMAR_VREF_IMPEDANCE   2      /* 125 kOhm */
#define VOLMAR_MICBIAS_VOLTAGE  6      /* 2.754 V  */
#define VOLMAR_JACK_INSERT_DEB  7
#define VOLMAR_JACK_EJECT_DEB   7

/*
 * FLL for BCLK 3.072 MHz -> 256 * 48 kHz, computed with Linux
 * nau8825_calc_fll_param():
 *   fref = 3.072 MHz (pre-scaler /1, clk_ref_div 0)
 *   ratio = 1 (fref >= 512 kHz)
 *   FDCO  = 256*48000*2*5 = 122.88 MHz (largest within 90..124 MHz),
 *           mclk_src scaling 5 -> 0xF
 *   fll_int = 122.88 MHz / 3.072 MHz = 40, fll_frac = 0
 */
#define VOLMAR_FLL_MCLK_SRC     0xFu
#define VOLMAR_FLL_RATIO        0x01u
#define VOLMAR_FLL_INT          40u
#define VOLMAR_FLL_REF_DIV      0u
#define VOLMAR_DAC_CLK_SRC      (2u << 4)   /* osr_dac_sel[OSR64].clk_src = 2 */

/* jack polling period */
#define NAU_JACK_POLL_MS        300
