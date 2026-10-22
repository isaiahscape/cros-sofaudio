/*++

Module Name:

    sof_regs.h

Abstract:

    Register definitions needed to boot Sound Open Firmware (SOF) on the
    Intel cAVS 2.5 audio DSP (Tiger Lake / Alder Lake).

    Values are taken from the Linux kernel SOF driver
    (sound/soc/sof/intel/hda.h, dual licensed GPL-2.0 / BSD-3-Clause).

--*/

#ifndef _CSAUDIOINTCSOF_SOF_REGS_H_
#define _CSAUDIOINTCSOF_SOF_REGS_H_

/* ---------------------------------------------------------------------
 * HDA controller BAR (BAR0)
 * ------------------------------------------------------------------- */
#define SOF_HDA_INTCTL                  0x20
#define SOF_HDA_VS_INTEL_EM2            0x1030
#define SOF_HDA_VS_INTEL_LTRP           0x1048
#define SOF_HDA_VS_INTEL_LTRP_GB_MASK   0x3F
#define SOF_HDA_LTRP_GB_VALUE_US        95

/* Processing pipe capability (PPCAP) block, pointer provided by the bus */
#define SOF_HDA_REG_PP_PPCTL            0x04
#define SOF_HDA_PPCTL_PIE               (1u << 31)
#define SOF_HDA_PPCTL_GPROCEN           (1u << 30)

/* ---------------------------------------------------------------------
 * DSP BAR (BAR4)
 * ------------------------------------------------------------------- */
#define SOF_DSP_REG_ADSPCS              0x04
#define SOF_DSP_REG_ADSPIC              0x08
#define SOF_DSP_REG_ADSPIS              0x0C

#define SOF_ADSPCS_CRST(cm)             ((UINT32)(cm) << 0)
#define SOF_ADSPCS_CSTALL(cm)           ((UINT32)(cm) << 8)
#define SOF_ADSPCS_SPA(cm)              ((UINT32)(cm) << 16)
#define SOF_ADSPCS_CPA(cm)              ((UINT32)(cm) << 24)

/* IPC registers, Cannon Lake layout (used up to cAVS 2.5) */
#define SOF_CNL_IPC_BASE                0xC0
#define SOF_CNL_HIPCTDR                 (SOF_CNL_IPC_BASE + 0x00) /* DSP -> host request  */
#define SOF_CNL_HIPCTDA                 (SOF_CNL_IPC_BASE + 0x04) /* host ack of the above */
#define SOF_CNL_HIPCTDD                 (SOF_CNL_IPC_BASE + 0x08)
#define SOF_CNL_HIPCIDR                 (SOF_CNL_IPC_BASE + 0x10) /* host -> DSP request  */
#define SOF_CNL_HIPCIDA                 (SOF_CNL_IPC_BASE + 0x14) /* DSP ack of the above */
#define SOF_CNL_HIPCIDD                 (SOF_CNL_IPC_BASE + 0x18)
#define SOF_CNL_HIPCCTL                 (SOF_CNL_IPC_BASE + 0x28)

#define SOF_CNL_HIPCIDR_BUSY            (1u << 31)
#define SOF_CNL_HIPCIDA_DONE            (1u << 31)
#define SOF_CNL_HIPCTDR_BUSY            (1u << 31)
#define SOF_CNL_HIPCTDA_DONE            (1u << 31)
#define SOF_CNL_MSG_MASK                0x7FFFFFFF

/* ROM boot protocol */
#define SOF_ROM_IPC_CONTROL             0x01000000
#define SOF_ROM_IPC_PURGE_FW            0x00004000

/* SRAM windows */
#define SOF_SRAM_WINDOW_OFFSET(x)       (0x80000 + (x) * 0x20000)
#define SOF_SRAM_REG_ROM_STATUS         (SOF_SRAM_WINDOW_OFFSET(0) + 0x0)
#define SOF_SRAM_REG_ROM_ERROR          (SOF_SRAM_WINDOW_OFFSET(0) + 0x4)
#define SOF_MBOX_UPLINK_OFFSET          0x81000

#define SOF_FSR_STATE_MASK              0x00FFFFFF
#define SOF_FSR_MODULE_MASK             0x70000000
#define SOF_FSR_HALTED                  (1u << 31)
#define SOF_FSR_STATE(x)                ((x) & SOF_FSR_STATE_MASK)
#define SOF_FSR_STATE_INIT_DONE         0x1
#define SOF_FSR_STATE_FW_ENTERED        0x5

/* SSP ports: force clock-consumer mode before ROM boot (as Linux does) */
#define SOF_CNL_SSP_BASE_OFFSET         0x10000
#define SOF_SSP_DEV_MEM_SIZE            0x1000
#define SOF_SSP_SSC1_OFFSET             0x4
#define SOF_SSP_SET_CBP_CFP             ((1u << 25) | (1u << 24))
#define SOF_TGL_SSP_COUNT               3

/* Timeouts (microseconds) */
#define SOF_POLL_INTERVAL_US            500
#define SOF_RESET_TIMEOUT_US            50000
#define SOF_INIT_TIMEOUT_US             500000
#define SOF_ROM_INIT_TIMEOUT_US         300000
#define SOF_BASEFW_TIMEOUT_US           3000000
#define SOF_FW_READY_TIMEOUT_US         3000000
#define SOF_FW_BOOT_ATTEMPTS            3

/* ---------------------------------------------------------------------
 * Firmware image / IPC3 structures
 * ------------------------------------------------------------------- */
#define SOF_EXT_MAN_MAGIC_NUMBER        0x6e614d58 /* "XMan" */

#pragma pack(push, 1)
typedef struct _SOF_EXT_MAN_HEADER {
    UINT32 magic;
    UINT32 full_size;
    UINT32 header_size;
    UINT32 header_version;
} SOF_EXT_MAN_HEADER;

typedef struct _SOF_IPC_HDR {
    UINT32 size;
    UINT32 cmd;
} SOF_IPC_HDR;

/* struct sof_ipc_hdr: size only (no cmd) */
typedef struct _SOF_IPC_HDR_SIZE {
    UINT32 size;
} SOF_IPC_HDR_SIZE;

typedef struct _SOF_IPC_FW_VERSION {
    SOF_IPC_HDR_SIZE hdr;
    UINT16 major;
    UINT16 minor;
    UINT16 micro;
    UINT16 build;
    UINT8  date[12];
    UINT8  time[10];
    UINT8  tag[6];
    UINT32 abi_version;
    UINT32 src_hash;
    UINT32 reserved[3];
} SOF_IPC_FW_VERSION;

typedef struct _SOF_IPC_FW_READY_MSG {
    SOF_IPC_HDR hdr;
    UINT32 dspbox_offset;
    UINT32 hostbox_offset;
    UINT32 dspbox_size;
    UINT32 hostbox_size;
    SOF_IPC_FW_VERSION version;
    UINT64 flags;
    UINT32 reserved[4];
} SOF_IPC_FW_READY_MSG;
#pragma pack(pop)

#define SOF_IPC_GLB_TYPE_MASK           0xF0000000
#define SOF_IPC_FW_READY                0x70000000
#define SOF_IPC_PANIC_MAGIC             0x0dead000
#define SOF_IPC_PANIC_MAGIC_MASK        0x0ffff000

/* HDA code loader stream format: 48 kHz, 32-bit, mono (0x40) */
#define SOF_CL_SAMPLE_RATE              48000
#define SOF_CL_CHANNELS                 1
#define SOF_CL_BITS                     32

/* Boot stages written to the registry boot log */
enum SOF_BOOT_STAGE {
    SofStageNotStarted      = 0,
    SofStageLoadFile        = 1,
    SofStagePrepareStreams  = 2,
    SofStageCorePowerUp     = 3,
    SofStageRomControl      = 4,
    SofStageCoreRun         = 5,
    SofStageRomAck          = 6,
    SofStageRomInitDone     = 7,
    SofStageCodeLoad        = 8,
    SofStageFwReady         = 9,
    SofStageBooted          = 10,
    SofStageSkipped         = 99,
};

#endif /* _CSAUDIOINTCSOF_SOF_REGS_H_ */
