/*++

Module Name:

    sof_boot.cpp

Abstract:

    Boots Sound Open Firmware (SOF, IPC3) on the Intel cAVS 2.5 audio DSP
    found in Alder Lake Chromebooks (e.g. Acer Chromebook Vero 514 "volmar").

    The sequence follows the Linux kernel SOF driver:
      sound/soc/sof/intel/hda-loader.c  (cl_dsp_init, hda_cl_copy_fw,
                                          hda_dsp_cl_boot_firmware_iccmax)
      sound/soc/sof/intel/hda-dsp.c     (core power / reset / run)
      sound/soc/sof/intel/cnl.c         (IPC register handshake)
      sound/soc/sof/ipc3.c              (FW_READY)

    Milestone 1 scope: load the firmware and wait for FW_READY. No
    topology, no pipelines, no audio routing. Nothing reaches the
    speaker amplifiers from this code.

    Every boot writes its result to
        HKLM\SOFTWARE\CrosSofAudio\DspBoot
    so it can be read back with:  reg query HKLM\SOFTWARE\CrosSofAudio\DspBoot

    Kill switch: set DWORD HKLM\SOFTWARE\CrosSofAudio\EnableDspBoot = 0
    to skip the DSP boot entirely.

--*/

#include "definitions.h"
#include "endpoints.h"
#include "hw.h"
#include "sof_regs.h"

/* Declared in ntddk.h, which this PortCls driver does not include. */
extern "C" NTKERNELAPI PHYSICAL_ADDRESS MmGetPhysicalAddress(_In_ PVOID BaseAddress);

#define SOF_POOLTAG                 'BfoS'
#define SOF_REG_ROOT                L"\\Registry\\Machine\\SOFTWARE\\CrosSofAudio"
#define SOF_REG_LOG                 L"\\Registry\\Machine\\SOFTWARE\\CrosSofAudio\\DspBoot"
#define SOF_FW_PATH_ADL             L"\\SystemRoot\\System32\\drivers\\sof-adl.ri"
#define SOF_MAX_FW_SIZE             (4 * 1024 * 1024)
#define SOF_BDL_CHUNK               4096
#define SOF_BDL_MAX_ENTRIES         256     /* bus allocates a 4 KiB BDL = 256 entries */

/* =========================================================================
 * Register helpers
 * ========================================================================= */

static __forceinline UINT32 SofRead32(_In_ PUCHAR base, _In_ ULONG off)
{
    return READ_REGISTER_ULONG((volatile ULONG*)(base + off));
}

static __forceinline void SofWrite32(_In_ PUCHAR base, _In_ ULONG off, _In_ UINT32 val)
{
    WRITE_REGISTER_ULONG((volatile ULONG*)(base + off), val);
}

static __forceinline void SofUpdate32(_In_ PUCHAR base, _In_ ULONG off, _In_ UINT32 mask, _In_ UINT32 val)
{
    UINT32 cur = SofRead32(base, off);
    SofWrite32(base, off, (cur & ~mask) | (val & mask));
}

static __forceinline UINT8 SofRead8(_In_ PUCHAR base, _In_ ULONG off)
{
    return READ_REGISTER_UCHAR((volatile UCHAR*)(base + off));
}

static __forceinline void SofWrite8(_In_ PUCHAR base, _In_ ULONG off, _In_ UINT8 val)
{
    WRITE_REGISTER_UCHAR((volatile UCHAR*)(base + off), val);
}

static void SofSleepUs(_In_ ULONG us)
{
    LARGE_INTEGER interval;
    interval.QuadPart = -((LONGLONG)us * 10);   /* relative, 100 ns units */
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

/* Poll until (reg & mask) == value. Returns TRUE on success. */
static BOOLEAN SofPoll32(
    _In_ PUCHAR base,
    _In_ ULONG off,
    _In_ UINT32 mask,
    _In_ UINT32 value,
    _In_ ULONG timeoutUs,
    _Out_opt_ UINT32* lastRead)
{
    ULONGLONG start = KeQueryInterruptTime();          /* 100 ns units */
    ULONGLONG limit = (ULONGLONG)timeoutUs * 10;
    UINT32 v;

    for (;;) {
        v = SofRead32(base, off);
        if ((v & mask) == value) {
            if (lastRead) *lastRead = v;
            return TRUE;
        }
        if (KeQueryInterruptTime() - start > limit) {
            break;
        }
        SofSleepUs(SOF_POLL_INTERVAL_US);
    }

    v = SofRead32(base, off);
    if (lastRead) *lastRead = v;
    return ((v & mask) == value) ? TRUE : FALSE;
}

/* Poll the ROM/FW status register until its state field equals 'state'. */
static BOOLEAN SofPollRomState(
    _In_ PUCHAR dsp,
    _In_ UINT32 state,
    _In_ ULONG timeoutUs,
    _Out_ UINT32* lastRead)
{
    return SofPoll32(dsp, SOF_SRAM_REG_ROM_STATUS, SOF_FSR_STATE_MASK, state, timeoutUs, lastRead);
}

/* =========================================================================
 * Registry boot log
 * ========================================================================= */

static void SofLogDword(_In_ PCWSTR name, _In_ ULONG value)
{
    RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SOF_REG_LOG, name, REG_DWORD, &value, sizeof(value));
}

static void SofLogString(_In_ PCWSTR name, _In_ PCWSTR value)
{
    RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SOF_REG_LOG, name, REG_SZ,
        (PVOID)value, (ULONG)((wcslen(value) + 1) * sizeof(WCHAR)));
}

static ULONG SofReadConfigDword(_In_ PCWSTR name, _In_ ULONG defaultValue)
{
    ULONG value = defaultValue;
    ULONG def = defaultValue;
    RTL_QUERY_REGISTRY_TABLE table[2];
    RtlZeroMemory(table, sizeof(table));

    table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    table[0].Name = (PWSTR)name;
    table[0].EntryContext = &value;
    table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
    table[0].DefaultData = &def;
    table[0].DefaultLength = sizeof(def);

    NTSTATUS status = RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, SOF_REG_ROOT, table, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        return defaultValue;
    }
    return value;
}

void CCsAudioSofHW::sof_log_stage(_In_ ULONG stage, _In_ NTSTATUS status)
{
    PUCHAR dsp = m_adspBar.Base.baseptr;

    m_bootStage = stage;
    SofLogDword(L"Stage", stage);
    SofLogDword(L"Status", (ULONG)status);

    /* Only touch DSP registers once GPROCEN is on, and the SRAM window
     * only while core 0 is powered (as the Linux driver does). */
    if (dsp && m_gprocenOn && m_adspBar.Len >= SOF_SRAM_WINDOW_OFFSET(1)) {
        UINT32 adspcs = SofRead32(dsp, SOF_DSP_REG_ADSPCS);
        SofLogDword(L"ADSPCS", adspcs);
        if (adspcs & SOF_ADSPCS_CPA(0x1)) {
            SofLogDword(L"RomStatus", SofRead32(dsp, SOF_SRAM_REG_ROM_STATUS));
            SofLogDword(L"RomError", SofRead32(dsp, SOF_SRAM_REG_ROM_ERROR));
        }
        SofLogDword(L"HIPCIDR", SofRead32(dsp, SOF_CNL_HIPCIDR));
        SofLogDword(L"HIPCIDA", SofRead32(dsp, SOF_CNL_HIPCIDA));
        SofLogDword(L"HIPCTDR", SofRead32(dsp, SOF_CNL_HIPCTDR));
        SofLogDword(L"HIPCTDD", SofRead32(dsp, SOF_CNL_HIPCTDD));
    }

    DPF(D_TERSE, ("[SOF] stage %lu status 0x%08X", stage, status));
}

/* =========================================================================
 * Firmware file
 * ========================================================================= */

static NTSTATUS SofLoadFile(_In_ PCWSTR path, _Out_ PUCHAR* data, _Out_ ULONG* size)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attrs;
    IO_STATUS_BLOCK iosb;
    HANDLE file = NULL;
    FILE_STANDARD_INFORMATION info;
    NTSTATUS status;

    *data = NULL;
    *size = 0;

    RtlInitUnicodeString(&name, path);
    InitializeObjectAttributes(&attrs, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = ZwCreateFile(&file, GENERIC_READ | SYNCHRONIZE, &attrs, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ZwQueryInformationFile(file, &iosb, &info, sizeof(info), FileStandardInformation);
    if (!NT_SUCCESS(status)) {
        goto out;
    }

    if (info.EndOfFile.QuadPart <= (LONGLONG)sizeof(SOF_EXT_MAN_HEADER) ||
        info.EndOfFile.QuadPart > SOF_MAX_FW_SIZE) {
        status = STATUS_INVALID_IMAGE_FORMAT;
        goto out;
    }

    {
        ULONG len = (ULONG)info.EndOfFile.QuadPart;
        PUCHAR buf = (PUCHAR)ExAllocatePoolZero(NonPagedPoolNx, len, SOF_POOLTAG);
        if (!buf) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto out;
        }

        LARGE_INTEGER offset;
        offset.QuadPart = 0;
        status = ZwReadFile(file, NULL, NULL, NULL, &iosb, buf, len, &offset, NULL);
        if (!NT_SUCCESS(status) || iosb.Information != len) {
            ExFreePoolWithTag(buf, SOF_POOLTAG);
            if (NT_SUCCESS(status)) status = STATUS_END_OF_FILE;
            goto out;
        }

        *data = buf;
        *size = len;
    }

out:
    ZwClose(file);
    return status;
}

/* =========================================================================
 * DSP core control (core 0 only: host managed cores mask = BIT(0) on TGL/ADL)
 * ========================================================================= */

#define SOF_CORE0   0x1

static BOOLEAN SofCorePowerUp(_In_ PUCHAR dsp)
{
    SofUpdate32(dsp, SOF_DSP_REG_ADSPCS, SOF_ADSPCS_SPA(SOF_CORE0), SOF_ADSPCS_SPA(SOF_CORE0));
    return SofPoll32(dsp, SOF_DSP_REG_ADSPCS, SOF_ADSPCS_CPA(SOF_CORE0), SOF_ADSPCS_CPA(SOF_CORE0),
        SOF_RESET_TIMEOUT_US, NULL);
}

static BOOLEAN SofCoreStallReset(_In_ PUCHAR dsp)
{
    SofUpdate32(dsp, SOF_DSP_REG_ADSPCS, SOF_ADSPCS_CSTALL(SOF_CORE0), SOF_ADSPCS_CSTALL(SOF_CORE0));
    SofUpdate32(dsp, SOF_DSP_REG_ADSPCS, SOF_ADSPCS_CRST(SOF_CORE0), SOF_ADSPCS_CRST(SOF_CORE0));
    return SofPoll32(dsp, SOF_DSP_REG_ADSPCS, SOF_ADSPCS_CRST(SOF_CORE0), SOF_ADSPCS_CRST(SOF_CORE0),
        SOF_RESET_TIMEOUT_US, NULL);
}

static BOOLEAN SofCoreResetPowerDown(_In_ PUCHAR dsp)
{
    BOOLEAN ok = SofCoreStallReset(dsp);
    SofUpdate32(dsp, SOF_DSP_REG_ADSPCS, SOF_ADSPCS_SPA(SOF_CORE0), 0);
    if (!SofPoll32(dsp, SOF_DSP_REG_ADSPCS, SOF_ADSPCS_CPA(SOF_CORE0), 0, SOF_RESET_TIMEOUT_US, NULL)) {
        ok = FALSE;
    }
    return ok;
}

static BOOLEAN SofCoreRun(_In_ PUCHAR dsp)
{
    /* leave reset */
    SofUpdate32(dsp, SOF_DSP_REG_ADSPCS, SOF_ADSPCS_CRST(SOF_CORE0), 0);
    if (!SofPoll32(dsp, SOF_DSP_REG_ADSPCS, SOF_ADSPCS_CRST(SOF_CORE0), 0, SOF_RESET_TIMEOUT_US, NULL)) {
        return FALSE;
    }

    /* unstall */
    SofUpdate32(dsp, SOF_DSP_REG_ADSPCS, SOF_ADSPCS_CSTALL(SOF_CORE0), 0);

    UINT32 v = SofRead32(dsp, SOF_DSP_REG_ADSPCS);
    BOOLEAN enabled =
        ((v & SOF_ADSPCS_CPA(SOF_CORE0)) != 0) &&
        ((v & SOF_ADSPCS_SPA(SOF_CORE0)) != 0) &&
        ((v & SOF_ADSPCS_CRST(SOF_CORE0)) == 0) &&
        ((v & SOF_ADSPCS_CSTALL(SOF_CORE0)) == 0);

    if (!enabled) {
        SofCoreStallReset(dsp);
    }
    return enabled;
}

/* =========================================================================
 * Contiguous DMA buffer + BDL helpers
 * ========================================================================= */

static PVOID SofAllocDma(_In_ SIZE_T size)
{
    PHYSICAL_ADDRESS low, high, skip;
    low.QuadPart = 0;
    high.QuadPart = 0xFFFFFFFF;     /* keep below 4 GiB like the bus driver does */
    skip.QuadPart = 0;
    return MmAllocateContiguousMemorySpecifyCache(size, low, high, skip, MmCached);
}

/* Fills 'entries' BDL entries of SOF_BDL_CHUNK bytes covering a
 * physically contiguous buffer. Returns the number of entries used. */
static int SofFillBdl(_In_ PVOID bdlBuf, _In_ PVOID buffer, _In_ ULONG size)
{
    PHDAC_BDLENTRY bdl = (PHDAC_BDLENTRY)bdlBuf;
    PHYSICAL_ADDRESS base = MmGetPhysicalAddress(buffer);
    ULONG offset = 0;
    int i = 0;

    while (offset < size && i < SOF_BDL_MAX_ENTRIES) {
        ULONG chunk = size - offset;
        if (chunk > SOF_BDL_CHUNK) chunk = SOF_BDL_CHUNK;

        ULONGLONG addr = (ULONGLONG)base.QuadPart + offset;
        bdl[i].lowAddr = (UINT32)(addr & 0xFFFFFFFF);
        bdl[i].highAddr = (UINT32)(addr >> 32);
        bdl[i].len = chunk;
        bdl[i].ioc = 0;

        offset += chunk;
        i++;
    }
    KeMemoryBarrier();
    return i;
}

/* =========================================================================
 * Main boot routine
 * ========================================================================= */

BOOLEAN CCsAudioSofHW::sof_is_supported_platform()
{
    switch (m_BusInterface.CtlrDevId) {
    case 0x51C8:    /* Alder Lake-P (volmar) */
    case 0x51CC:    /* Alder Lake-M */
        return TRUE;
    default:
        return FALSE;
    }
}

NTSTATUS CCsAudioSofHW::sof_boot_dsp()
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    PUCHAR dsp = m_adspBar.Base.baseptr;
    PUCHAR hda = m_hdaBar.Base.baseptr;
    PUCHAR ppcap = (PUCHAR)m_ppcap;

    PUCHAR fwFile = NULL;
    ULONG fwFileSize = 0;
    PUCHAR payload = NULL;
    ULONG payloadSize = 0;

    PVOID clBuf = NULL;
    PVOID iccBuf = NULL;
    HANDLE clStream = NULL;
    HANDLE iccStream = NULL;
    UINT8 clTag = 0;
    UINT8 iccTag = 0;
    BOOLEAN iccRunning = FALSE;
    BOOLEAN ltrpSaved = FALSE;
    UINT8 ltrpOriginal = 0;
    BOOLEAN romReady = FALSE;
    BOOLEAN fwEntered = FALSE;
    UINT32 last = 0;
    int attempt;

    PAGED_CODE();

    if (m_dspBooted) {
        return STATUS_SUCCESS;
    }

    RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SOF_REG_ROOT);
    RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SOF_REG_LOG);

    m_bootCount++;
    SofLogDword(L"BootCount", m_bootCount);
    SofLogDword(L"CtlrDevId", m_BusInterface.CtlrDevId);
    SofLogString(L"FwVersion", L"");
    SofLogString(L"Result", L"running");

    if (!sof_is_supported_platform()) {
        SofLogString(L"Result", L"skipped: controller is not Alder Lake-P/M");
        sof_log_stage(SofStageSkipped, STATUS_NOT_SUPPORTED);
        return STATUS_NOT_SUPPORTED;
    }

    if (SofReadConfigDword(L"EnableDspBoot", 1) == 0) {
        SofLogString(L"Result", L"skipped: EnableDspBoot = 0");
        sof_log_stage(SofStageSkipped, STATUS_NOT_SUPPORTED);
        return STATUS_NOT_SUPPORTED;
    }

    if (!dsp || m_adspBar.Len < SOF_SRAM_WINDOW_OFFSET(1) ||
        !hda || m_hdaBar.Len < (SOF_HDA_VS_INTEL_LTRP + 1) || !ppcap ||
        !m_BusInterface.GetRenderStream || !m_BusInterface.GetCaptureStream ||
        !m_BusInterface.PrepareDSP || !m_BusInterface.TriggerDSP ||
        !m_BusInterface.CleanupDSP || !m_BusInterface.FreeStream) {
        SofLogDword(L"AdspBarLen", m_adspBar.Len);
        SofLogDword(L"HdaBarLen", m_hdaBar.Len);
        SofLogString(L"Result", L"failed: missing BAR or bus interface function");
        sof_log_stage(SofStageNotStarted, STATUS_DEVICE_CONFIGURATION_ERROR);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* ---- 1. Load firmware image ------------------------------------ */
    sof_log_stage(SofStageLoadFile, STATUS_PENDING);
    status = SofLoadFile(SOF_FW_PATH_ADL, &fwFile, &fwFileSize);
    if (!NT_SUCCESS(status)) {
        SofLogString(L"Result", L"failed: cannot read System32\\drivers\\sof-adl.ri");
        sof_log_stage(SofStageLoadFile, status);
        goto cleanup;
    }

    /* IPC mailbox windows come from the firmware's extended manifest */
    sof_parse_windows(fwFile, fwFileSize);

    {
        const SOF_EXT_MAN_HEADER* hdr = (const SOF_EXT_MAN_HEADER*)fwFile;
        ULONG payloadOffset = 0;
        if (hdr->magic == SOF_EXT_MAN_MAGIC_NUMBER) {
            payloadOffset = hdr->full_size;
        }
        if (payloadOffset >= fwFileSize) {
            status = STATUS_INVALID_IMAGE_FORMAT;
            SofLogString(L"Result", L"failed: bad extended manifest in sof-adl.ri");
            sof_log_stage(SofStageLoadFile, status);
            goto cleanup;
        }
        payload = fwFile + payloadOffset;
        payloadSize = fwFileSize - payloadOffset;
        SofLogDword(L"FwFileSize", fwFileSize);
        SofLogDword(L"FwPayloadOffset", payloadOffset);
    }

    if (payloadSize > (ULONG)SOF_BDL_MAX_ENTRIES * SOF_BDL_CHUNK) {
        status = STATUS_BUFFER_OVERFLOW;
        SofLogString(L"Result", L"failed: firmware larger than one BDL can describe");
        sof_log_stage(SofStageLoadFile, status);
        goto cleanup;
    }

    /* ---- 2. DMA buffers and streams -------------------------------- */
    sof_log_stage(SofStagePrepareStreams, STATUS_PENDING);

    clBuf = SofAllocDma(ROUND_TO_PAGES(payloadSize));
    iccBuf = SofAllocDma(PAGE_SIZE);
    if (!clBuf || !iccBuf) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        SofLogString(L"Result", L"failed: DMA buffer allocation");
        sof_log_stage(SofStagePrepareStreams, status);
        goto cleanup;
    }
    RtlZeroMemory(clBuf, ROUND_TO_PAGES(payloadSize));
    RtlCopyMemory(clBuf, payload, payloadSize);
    RtlZeroMemory(iccBuf, PAGE_SIZE);
    KeMemoryBarrier();

    /* Enable DSP processing pipes (GPROCEN). IPC/PP interrupts stay OFF:
     * this milestone polls, so no interrupt can be left unhandled. */
    m_gprocenWasSet = (SofRead32(ppcap, SOF_HDA_REG_PP_PPCTL) & SOF_HDA_PPCTL_GPROCEN) ? TRUE : FALSE;
    SofUpdate32(ppcap, SOF_HDA_REG_PP_PPCTL, SOF_HDA_PPCTL_GPROCEN, SOF_HDA_PPCTL_GPROCEN);
    m_gprocenOn = TRUE;

    /* Make sure core 0 starts from a clean, powered-off state. */
    if (SofRead32(dsp, SOF_DSP_REG_ADSPCS) & SOF_ADSPCS_CPA(SOF_CORE0)) {
        SofCoreResetPowerDown(dsp);
    }

    {
        HDAUDIO_STREAM_FORMAT fmt;
        RtlZeroMemory(&fmt, sizeof(fmt));
        fmt.SampleRate = SOF_CL_SAMPLE_RATE;
        fmt.ValidBitsPerSample = SOF_CL_BITS;
        fmt.ContainerSize = SOF_CL_BITS;
        fmt.NumberOfChannels = SOF_CL_CHANNELS;

        /* ICCMAX capture stream, runs during boot (hda_dsp_cl_boot_firmware_iccmax) */
        status = m_BusInterface.GetCaptureStream(m_BusInterface.Context, fmt, &iccStream, &iccTag);
        if (!NT_SUCCESS(status)) {
            iccStream = NULL;
            SofLogString(L"Result", L"failed: no capture stream for ICCMAX");
            sof_log_stage(SofStagePrepareStreams, status);
            goto cleanup;
        }

        PVOID iccBdl = NULL;
        status = m_BusInterface.PrepareDSP(m_BusInterface.Context, iccStream, PAGE_SIZE, 1, &iccBdl);
        if (!NT_SUCCESS(status) || !iccBdl) {
            if (NT_SUCCESS(status)) status = STATUS_UNSUCCESSFUL;
            SofLogString(L"Result", L"failed: ICCMAX stream prepare");
            sof_log_stage(SofStagePrepareStreams, status);
            goto cleanup;
        }
        SofFillBdl(iccBdl, iccBuf, PAGE_SIZE);

        /* Code loader playback stream */
        status = m_BusInterface.GetRenderStream(m_BusInterface.Context, fmt, &clStream, &clTag);
        if (!NT_SUCCESS(status)) {
            clStream = NULL;
            SofLogString(L"Result", L"failed: no playback stream for code loader");
            sof_log_stage(SofStagePrepareStreams, status);
            goto cleanup;
        }

        int clBlocks = (int)((payloadSize + SOF_BDL_CHUNK - 1) / SOF_BDL_CHUNK);
        PVOID clBdl = NULL;
        status = m_BusInterface.PrepareDSP(m_BusInterface.Context, clStream, payloadSize, clBlocks, &clBdl);
        if (!NT_SUCCESS(status) || !clBdl) {
            if (NT_SUCCESS(status)) status = STATUS_UNSUCCESSFUL;
            SofLogString(L"Result", L"failed: code loader stream prepare");
            sof_log_stage(SofStagePrepareStreams, status);
            goto cleanup;
        }
        SofFillBdl(clBdl, clBuf, payloadSize);

        if (m_BusInterface.DSPEnableSPIB) {
            m_BusInterface.DSPEnableSPIB(m_BusInterface.Context, clStream, payloadSize);
        }

        SofLogDword(L"CodeLoaderTag", clTag);
        SofLogDword(L"IccmaxTag", iccTag);
    }

    /* LTRP guardband 95 us during boot, then start ICCMAX DMA */
    ltrpOriginal = SofRead8(hda, SOF_HDA_VS_INTEL_LTRP) & SOF_HDA_VS_INTEL_LTRP_GB_MASK;
    ltrpSaved = TRUE;
    SofWrite8(hda, SOF_HDA_VS_INTEL_LTRP,
        (SofRead8(hda, SOF_HDA_VS_INTEL_LTRP) & ~SOF_HDA_VS_INTEL_LTRP_GB_MASK) | SOF_HDA_LTRP_GB_VALUE_US);

    m_BusInterface.TriggerDSP(m_BusInterface.Context, iccStream, TRUE);
    iccRunning = TRUE;

    /* ---- 3. ROM init (cl_dsp_init), up to 3 attempts ---------------- */
    for (attempt = 1; attempt <= SOF_FW_BOOT_ATTEMPTS && !romReady; attempt++) {
        SofLogDword(L"Attempt", attempt);

        sof_log_stage(SofStageCorePowerUp, STATUS_PENDING);
        if (!SofCorePowerUp(dsp)) {
            sof_log_stage(SofStageCorePowerUp, STATUS_IO_TIMEOUT);
            SofCoreResetPowerDown(dsp);
            continue;
        }

        for (ULONG i = 0; i < SOF_TGL_SSP_COUNT; i++) {
            SofUpdate32(dsp, SOF_CNL_SSP_BASE_OFFSET + i * SOF_SSP_DEV_MEM_SIZE + SOF_SSP_SSC1_OFFSET,
                SOF_SSP_SET_CBP_CFP, SOF_SSP_SET_CBP_CFP);
        }

        sof_log_stage(SofStageRomControl, STATUS_PENDING);
        SofWrite32(dsp, SOF_CNL_HIPCIDR,
            SOF_CNL_HIPCIDR_BUSY | SOF_ROM_IPC_CONTROL | SOF_ROM_IPC_PURGE_FW |
            ((UINT32)(clTag - 1) << 9));

        sof_log_stage(SofStageCoreRun, STATUS_PENDING);
        if (!SofCoreRun(dsp)) {
            sof_log_stage(SofStageCoreRun, STATUS_IO_DEVICE_ERROR);
            SofCoreResetPowerDown(dsp);
            continue;
        }

        sof_log_stage(SofStageRomAck, STATUS_PENDING);
        if (!SofPoll32(dsp, SOF_CNL_HIPCIDA, SOF_CNL_HIPCIDA_DONE, SOF_CNL_HIPCIDA_DONE,
            SOF_INIT_TIMEOUT_US, &last)) {
            sof_log_stage(SofStageRomAck, STATUS_IO_TIMEOUT);
            SofCoreResetPowerDown(dsp);
            continue;
        }
        /* clear the ROM's reply */
        SofUpdate32(dsp, SOF_CNL_HIPCIDA, SOF_CNL_HIPCIDA_DONE, SOF_CNL_HIPCIDA_DONE);

        sof_log_stage(SofStageRomInitDone, STATUS_PENDING);
        if (!SofPollRomState(dsp, SOF_FSR_STATE_INIT_DONE, SOF_ROM_INIT_TIMEOUT_US, &last)) {
            sof_log_stage(SofStageRomInitDone, STATUS_IO_TIMEOUT);
            SofCoreResetPowerDown(dsp);
            continue;
        }

        romReady = TRUE;
    }

    if (!romReady) {
        status = STATUS_IO_TIMEOUT;
        SofLogString(L"Result", L"failed: DSP ROM did not initialise (see Stage)");
        goto cleanup;
    }

    /* ---- 4. Code load (hda_cl_copy_fw) ------------------------------ */
    sof_log_stage(SofStageCodeLoad, STATUS_PENDING);
    m_BusInterface.TriggerDSP(m_BusInterface.Context, clStream, TRUE);
    fwEntered = SofPollRomState(dsp, SOF_FSR_STATE_FW_ENTERED, SOF_BASEFW_TIMEOUT_US, &last);
    m_BusInterface.TriggerDSP(m_BusInterface.Context, clStream, FALSE);

    if (!fwEntered) {
        status = STATUS_IO_TIMEOUT;
        SofLogString(L"Result", L"failed: firmware download did not reach FW_ENTERED");
        sof_log_stage(SofStageCodeLoad, status);
        goto cleanup;
    }

    status = STATUS_SUCCESS;

cleanup:
    /* Stop and release the boot streams (always, success or not). */
    if (clStream) {
        if (m_BusInterface.DSPDisableSPIB) {
            m_BusInterface.DSPDisableSPIB(m_BusInterface.Context, clStream);
        }
        m_BusInterface.TriggerDSP(m_BusInterface.Context, clStream, FALSE);
        (void)m_BusInterface.CleanupDSP(m_BusInterface.Context, clStream);
        (void)m_BusInterface.FreeStream(m_BusInterface.Context, clStream);
        clStream = NULL;
    }
    if (iccStream) {
        if (iccRunning) {
            m_BusInterface.TriggerDSP(m_BusInterface.Context, iccStream, FALSE);
        }
        (void)m_BusInterface.CleanupDSP(m_BusInterface.Context, iccStream);
        (void)m_BusInterface.FreeStream(m_BusInterface.Context, iccStream);
        iccStream = NULL;
    }
    if (ltrpSaved) {
        SofWrite8(hda, SOF_HDA_VS_INTEL_LTRP,
            (SofRead8(hda, SOF_HDA_VS_INTEL_LTRP) & ~SOF_HDA_VS_INTEL_LTRP_GB_MASK) | ltrpOriginal);
    }

    /* DMA is stopped now; buffers can go. */
    if (clBuf) MmFreeContiguousMemory(clBuf);
    if (iccBuf) MmFreeContiguousMemory(iccBuf);
    if (fwFile) ExFreePoolWithTag(fwFile, SOF_POOLTAG);

    if (!NT_SUCCESS(status)) {
        if (dsp && m_bootStage >= SofStageCorePowerUp) {
            SofCoreResetPowerDown(dsp);
        }
        if (ppcap && m_gprocenOn && !m_gprocenWasSet) {
            SofUpdate32(ppcap, SOF_HDA_REG_PP_PPCTL, SOF_HDA_PPCTL_GPROCEN, 0);
            m_gprocenOn = FALSE;
        }
        return status;
    }

    /* ---- 5. Wait for FW_READY (IPC3) -------------------------------- */
    status = sof_wait_fw_ready();
    if (NT_SUCCESS(status)) {
        /* Milestone 2: build the headset playback pipeline. A failure here is
         * logged (DspIpc key) but leaves the DSP running. */
        (void)sof_setup_pipelines();
    }
    return status;
}

NTSTATUS CCsAudioSofHW::sof_wait_fw_ready()
{
    PUCHAR dsp = m_adspBar.Base.baseptr;
    UINT32 hipctdr = 0;

    sof_log_stage(SofStageFwReady, STATUS_PENDING);

    if (!SofPoll32(dsp, SOF_CNL_HIPCTDR, SOF_CNL_HIPCTDR_BUSY, SOF_CNL_HIPCTDR_BUSY,
        SOF_FW_READY_TIMEOUT_US, &hipctdr)) {
        SofLogString(L"Result", L"failed: firmware entered but never sent FW_READY");
        sof_log_stage(SofStageFwReady, STATUS_IO_TIMEOUT);
        m_dspBooted = TRUE;     /* core is running; power it down on deinit */
        return STATUS_IO_TIMEOUT;
    }

    if ((hipctdr & SOF_IPC_PANIC_MAGIC_MASK) == SOF_IPC_PANIC_MAGIC) {
        SofLogString(L"Result", L"failed: firmware PANIC during boot (see HIPCTDR/HIPCTDD)");
        sof_log_stage(SofStageFwReady, STATUS_DEVICE_HARDWARE_ERROR);
        m_dspBooted = TRUE;
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    /* Read the FW_READY message from the uplink mailbox. */
    SOF_IPC_FW_READY_MSG ready;
    RtlZeroMemory(&ready, sizeof(ready));
    READ_REGISTER_BUFFER_ULONG((volatile ULONG*)(dsp + SOF_MBOX_UPLINK_OFFSET),
        (PULONG)&ready, sizeof(ready) / sizeof(ULONG));

    /* Acknowledge the message (cnl_ipc_host_done). */
    SofUpdate32(dsp, SOF_CNL_HIPCTDR, SOF_CNL_HIPCTDR_BUSY, SOF_CNL_HIPCTDR_BUSY);
    SofUpdate32(dsp, SOF_CNL_HIPCTDA, SOF_CNL_HIPCTDA_DONE, SOF_CNL_HIPCTDA_DONE);

    m_dspBooted = TRUE;

    SofLogDword(L"MsgCmd", ready.hdr.cmd);
    SofLogDword(L"MsgSize", ready.hdr.size);

    if ((ready.hdr.cmd & SOF_IPC_GLB_TYPE_MASK) != SOF_IPC_FW_READY) {
        SofLogString(L"Result", L"failed: first DSP message was not FW_READY (see MsgCmd)");
        sof_log_stage(SofStageFwReady, STATUS_UNEXPECTED_IO_ERROR);
        return STATUS_UNEXPECTED_IO_ERROR;
    }

    m_dspBoxOffset = ready.dspbox_offset;
    m_dspBoxSize = ready.dspbox_size;
    m_hostBoxOffset = ready.hostbox_offset;
    m_hostBoxSize = ready.hostbox_size;

    {
        WCHAR ver[96];
        CHAR tag[7];
        RtlCopyMemory(tag, ready.version.tag, 6);
        tag[6] = 0;
        RtlStringCchPrintfW(ver, ARRAYSIZE(ver), L"%u.%u.%u build %u tag %S abi 0x%08X",
            ready.version.major, ready.version.minor, ready.version.micro,
            ready.version.build, tag, ready.version.abi_version);
        SofLogString(L"FwVersion", ver);
    }
    SofLogDword(L"DspBoxOffset", m_dspBoxOffset);
    SofLogDword(L"DspBoxSize", m_dspBoxSize);
    SofLogDword(L"HostBoxOffset", m_hostBoxOffset);
    SofLogDword(L"HostBoxSize", m_hostBoxSize);

    SofLogString(L"Result", L"SUCCESS: SOF firmware booted and sent FW_READY");
    sof_log_stage(SofStageBooted, STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

/* Called from sof_deinit (D3 / unload). */
void CCsAudioSofHW::sof_dsp_power_down()
{
    PUCHAR dsp = m_adspBar.Base.baseptr;
    PUCHAR ppcap = (PUCHAR)m_ppcap;

    if (!m_dspBooted) {
        return;
    }

    if (dsp) {
        SofCoreResetPowerDown(dsp);
    }
    if (ppcap && m_gprocenOn && !m_gprocenWasSet) {
        SofUpdate32(ppcap, SOF_HDA_REG_PP_PPCTL, SOF_HDA_PPCTL_GPROCEN, 0);
        m_gprocenOn = FALSE;
    }

    m_dspBooted = FALSE;
    m_ipcReady = FALSE;
    sof_reset_pipeline_state();
    SofLogDword(L"PoweredDown", m_bootCount);
}
