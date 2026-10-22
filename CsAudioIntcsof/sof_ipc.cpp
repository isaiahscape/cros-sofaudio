/*++

Module Name:

    sof_ipc.cpp

Abstract:

    Milestone 2: IPC3 messaging with the booted SOF firmware, creation of the
    headset playback pipeline (topology pipeline 2: PCM1P -> volume -> SSP0)
    and the per-stream PCM_PARAMS / TRIGGER sequence.

    The pipeline messages are pre-built from sof-adl-max98373-nau8825.tplg by
    tools-gen/gen_hp_pipeline.c using the firmware's own v2.2 IPC headers, so
    no firmware struct is redefined here.

    IPC mechanics follow Linux sound/soc/sof/intel/cnl.c (cnl_ipc_send_msg,
    cnl_ipc_dsp_done, cnl_ipc_host_done). Interrupts stay off; replies are
    polled at PASSIVE_LEVEL.

    Results are logged to HKLM\SOFTWARE\CrosSofAudio\DspIpc.

--*/

#include "definitions.h"
#include "endpoints.h"
#include "hw.h"
#include "sof_regs.h"

typedef struct _SOF_IPC_BLOB {
    const char* name;
    UINT32 size;
    const UINT8* data;
} SOF_IPC_BLOB;

#include "sof_hp_pipeline.h"
#include "sof_spk_pipeline.h"

#define SOF_REG_IPC_LOG     L"\\Registry\\Machine\\SOFTWARE\\CrosSofAudio\\DspIpc"
#define SOF_IPC_MAX_SIZE    384
#define SOF_IPC_REPLY_TIMEOUT_US   500000   /* Linux uses 500 ms */
#define SOF_IPC_IDLE_TIMEOUT_US    50000

/* ---------------------------------------------------------------------- */

static __forceinline UINT32 IpcRd(PUCHAR b, ULONG o) { return READ_REGISTER_ULONG((volatile ULONG*)(b + o)); }
static __forceinline void IpcWr(PUCHAR b, ULONG o, UINT32 v) { WRITE_REGISTER_ULONG((volatile ULONG*)(b + o), v); }

static BOOLEAN IpcPoll(PUCHAR b, ULONG off, UINT32 mask, UINT32 value, ULONG timeoutUs, UINT32* last)
{
    ULONGLONG start = KeQueryInterruptTime();
    ULONGLONG limit = (ULONGLONG)timeoutUs * 10;
    for (;;) {
        UINT32 v = IpcRd(b, off);
        if ((v & mask) == value) { if (last) *last = v; return TRUE; }
        if (KeQueryInterruptTime() - start > limit) { if (last) *last = v; return FALSE; }
        LARGE_INTEGER d; d.QuadPart = -500;      /* 50 us */
        KeDelayExecutionThread(KernelMode, FALSE, &d);
    }
}

static void IpcLogDword(PCWSTR name, ULONG v)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SOF_REG_IPC_LOG, name, REG_DWORD, &v, sizeof(v));
}

static void IpcLogString(PCWSTR name, PCWSTR s)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SOF_REG_IPC_LOG, name, REG_SZ,
        (PVOID)s, (ULONG)((wcslen(s) + 1) * sizeof(WCHAR)));
}

/* "Setup07" = "DAI_CONFIG SSP0: fw err 0 status 0x00000000" */
static void IpcLogStep(PCWSTR key, const char* what, LONG fwErr, NTSTATUS st)
{
    WCHAR buf[128];
    RtlStringCchPrintfW(buf, ARRAYSIZE(buf), L"%S: fw err %d, status 0x%08X", what, fwErr, st);
    IpcLogString(key, buf);
}

/* ---------------------------------------------------------------------- */

/* Mailbox windows from the firmware's extended manifest (SOF_EXT_MAN_ELEM_WINDOW). */
void CCsAudioSofHW::sof_parse_windows(_In_ const UCHAR* fw, _In_ ULONG fwSize)
{
    /* defaults for sof-adl.ri v2.2 (verified from its manifest) */
    m_mboxDownOffset = SOF_SRAM_WINDOW_OFFSET(1);           /* host -> DSP */
    m_mboxDownSize = 0x2000;
    m_mboxUpOffset = SOF_MBOX_UPLINK_OFFSET;                /* DSP -> host */
    m_mboxUpSize = 0x1000;
    m_windowsFromManifest = FALSE;

    if (!fw || fwSize < 16) return;
    const UINT32* h = (const UINT32*)fw;
    if (h[0] != SOF_EXT_MAN_MAGIC_NUMBER) return;
    ULONG full = h[1], hdrSize = h[2];
    if (full > fwSize || hdrSize >= full) return;

    ULONG off = hdrSize;
    while (off + 8 <= full) {
        UINT32 type = *(const UINT32*)(fw + off);
        UINT32 size = *(const UINT32*)(fw + off + 4);
        if (size < 8 || off + size > full) break;

        if (type == 1 /* SOF_EXT_MAN_ELEM_WINDOW */ && size >= 8 + 16) {
            const UCHAR* w = fw + off + 8;                       /* struct sof_ipc_window */
            UINT32 num = *(const UINT32*)(w + 12);
            for (UINT32 i = 0; i < num && 8 + 16 + (i + 1) * 24 <= size; i++) {
                const UINT32* e = (const UINT32*)(w + 16 + i * 24);   /* hdr,type,id,flags,size,offset */
                UINT32 wtype = e[1], wid = e[2], wsize = e[4], woff = e[5];
                ULONG base = SOF_SRAM_WINDOW_OFFSET(wid) + woff;
                if (wtype == 0 /* DOWNBOX */) { m_mboxDownOffset = base; m_mboxDownSize = wsize; m_windowsFromManifest = TRUE; }
                if (wtype == 1 /* UPBOX */)   { m_mboxUpOffset = base;   m_mboxUpSize = wsize; }
            }
        }
        off += size;
    }
}

void CCsAudioSofHW::sof_ipc_service_notifications()
{
    PUCHAR dsp = m_adspBar.Base.baseptr;
    UINT32 tdr = IpcRd(dsp, SOF_CNL_HIPCTDR);
    if (!(tdr & SOF_CNL_HIPCTDR_BUSY)) return;

    UINT32 hdr[2] = { 0, 0 };
    READ_REGISTER_BUFFER_ULONG((volatile ULONG*)(dsp + m_mboxUpOffset), (PULONG)hdr, 2);
    m_notifyCount++;
    m_lastNotifyCmd = hdr[1];

    /* cnl_ipc_host_done: clear BUSY (write 1) and signal DONE */
    IpcWr(dsp, SOF_CNL_HIPCTDR, IpcRd(dsp, SOF_CNL_HIPCTDR) | SOF_CNL_HIPCTDR_BUSY);
    IpcWr(dsp, SOF_CNL_HIPCTDA, IpcRd(dsp, SOF_CNL_HIPCTDA) | SOF_CNL_HIPCTDA_DONE);
}

NTSTATUS CCsAudioSofHW::sof_ipc_tx(
    _In_reads_bytes_(size) const void* msg, _In_ ULONG size,
    _Out_writes_bytes_opt_(replySize) void* reply, _In_ ULONG replySize,
    _Out_ LONG* fwError)
{
    PUCHAR dsp = m_adspBar.Base.baseptr;
    ULONG buf[SOF_IPC_MAX_SIZE / 4];
    NTSTATUS status = STATUS_SUCCESS;
    UINT32 last = 0;

    *fwError = 0;

    if (!m_dspBooted || !m_ipcReady || !dsp) return STATUS_DEVICE_NOT_READY;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { m_ipcSkippedIrql++; return STATUS_INVALID_DEVICE_STATE; }
    if (size < 8 || size > SOF_IPC_MAX_SIZE || size > m_mboxDownSize ||
        replySize > SOF_IPC_MAX_SIZE || replySize > m_mboxDownSize) return STATUS_INVALID_PARAMETER;

    ExAcquireFastMutex(&m_ipcMutex);

    sof_ipc_service_notifications();

    /* previous host->DSP message must have been consumed */
    if (!IpcPoll(dsp, SOF_CNL_HIPCIDR, SOF_CNL_HIPCIDR_BUSY, 0, SOF_IPC_IDLE_TIMEOUT_US, &last)) {
        status = STATUS_DEVICE_BUSY;
        goto out;
    }

    RtlZeroMemory(buf, sizeof(buf));
    RtlCopyMemory(buf, msg, size);
    WRITE_REGISTER_BUFFER_ULONG((volatile ULONG*)(dsp + m_mboxDownOffset), buf, (size + 3) / 4);
    KeMemoryBarrier();

    m_ipcCount++;
    m_lastIpcCmd = ((const UINT32*)msg)[1];
    IpcWr(dsp, SOF_CNL_HIPCIDR, SOF_CNL_HIPCIDR_BUSY);

    if (!IpcPoll(dsp, SOF_CNL_HIPCIDA, SOF_CNL_HIPCIDA_DONE, SOF_CNL_HIPCIDA_DONE,
                 SOF_IPC_REPLY_TIMEOUT_US, &last)) {
        status = STATUS_IO_TIMEOUT;
        m_ipcTimeouts++;
        goto out;
    }

    /* reply is placed in the host box (sof_ipc3 get_reply) */
    {
        ULONG words = (replySize > 12 ? replySize : 12);
        words = (words + 3) / 4;
        RtlZeroMemory(buf, sizeof(buf));
        READ_REGISTER_BUFFER_ULONG((volatile ULONG*)(dsp + m_mboxDownOffset), buf, words);
        LONG err = (LONG)buf[2];       /* struct sof_ipc_reply: size, cmd, error */
        *fwError = err;
        if (reply && replySize) RtlCopyMemory(reply, buf, replySize);
        if (err < 0) status = STATUS_UNSUCCESSFUL;
    }

    /* cnl_ipc_dsp_done: acknowledge the reply */
    IpcWr(dsp, SOF_CNL_HIPCIDA, IpcRd(dsp, SOF_CNL_HIPCIDA) | SOF_CNL_HIPCIDA_DONE);

out:
    sof_ipc_service_notifications();
    ExReleaseFastMutex(&m_ipcMutex);
    return status;
}

/* ----------------------------------------------------------------------
 * Render paths
 * ---------------------------------------------------------------------- */
typedef struct _SOF_PATH_DESC {
    PCWSTR              prefix;          /* registry key prefix */
    PCWSTR              attenName;       /* HKLM\SOFTWARE\CrosSofAudio\<attenName> (dB) */
    ULONG               attenDefault;
    ULONG               volMsgIndex;
    PCSTR               sspName;
    ULONG               sspIndex;
    PCWSTR              okText;
    const SOF_IPC_BLOB* setup;
    ULONG               setupCount;
    const UINT8*        daiHwParams;  ULONG daiHwParamsSize;
    const UINT8*        daiHwFree;    ULONG daiHwFreeSize;
    const UINT8*        pcmParams;    ULONG pcmParamsSize;
    const UINT8*        trigStart;    ULONG trigStartSize;
    const UINT8*        trigStop;     ULONG trigStopSize;
    const UINT8*        pcmFree;      ULONG pcmFreeSize;
} SOF_PATH_DESC;

#define BLOB(x) x, sizeof(x)

static const SOF_PATH_DESC c_PathDesc[SOF_PATH_COUNT] = {
    /* SOF_PATH_SPK */
    { L"Spk", L"SpeakerDspAttenDb", 0, SOF_SPK_VOL_MSG_INDEX, "SSP1", 1, L"SUCCESS: speaker pipeline 1 (PCM0P -> volume -> SSP1) created",
      c_SofSpkSetup, ARRAYSIZE(c_SofSpkSetup),
      BLOB(c_SofSpkDaiHwParams), BLOB(c_SofSpkDaiHwFree), BLOB(c_SofSpkPcmParams),
      BLOB(c_SofSpkTrigStart), BLOB(c_SofSpkTrigStop), BLOB(c_SofSpkPcmFree) },
    /* SOF_PATH_HP */
    { L"Hp", L"HeadphoneDspAttenDb", 20, SOF_HP_VOL_MSG_INDEX, "SSP0", 0, L"SUCCESS: headset pipeline 2 (PCM1P -> volume -> SSP0) created",
      c_SofHpSetup, ARRAYSIZE(c_SofHpSetup),
      BLOB(c_SofHpDaiHwParams), BLOB(c_SofHpDaiHwFree), BLOB(c_SofHpPcmParams),
      BLOB(c_SofHpTrigStart), BLOB(c_SofHpTrigStop), BLOB(c_SofHpPcmFree) },
};

C_ASSERT(sizeof(c_SofSpkPcmParams) == sizeof(c_SofHpPcmParams));

static void PathLogDword(ULONG path, PCWSTR name, ULONG v)
{
    WCHAR key[64];
    if (NT_SUCCESS(RtlStringCchPrintfW(key, ARRAYSIZE(key), L"%s%s", c_PathDesc[path].prefix, name)))
        IpcLogDword(key, v);
}

static void PathLogString(ULONG path, PCWSTR name, PCWSTR v)
{
    WCHAR key[64];
    if (NT_SUCCESS(RtlStringCchPrintfW(key, ARRAYSIZE(key), L"%s%s", c_PathDesc[path].prefix, name)))
        IpcLogString(key, v);
}

static void PathLogStep(ULONG path, PCWSTR name, const char* what, LONG fwErr, NTSTATUS st)
{
    WCHAR key[64];
    if (NT_SUCCESS(RtlStringCchPrintfW(key, ARRAYSIZE(key), L"%s%s", c_PathDesc[path].prefix, name)))
        IpcLogStep(key, what, fwErr, st);
}

/* 65536 * 10^(-dB/20) for dB = 0..60 (Q8.16 linear volume) */
static const UINT32 c_AttenQ816[61] = {
    65536,58409,52057,46396,41350,36854,32846,29274,26090,23253,20724,18471,16462,14672,13076,11654,
    10387,9257,8250,7353,6554,5841,5206,4640,4135,3685,3285,2927,2609,2325,2072,1847,1646,1467,1308,
    1165,1039,926,825,735,655,584,521,464,414,369,328,293,261,233,207,185,165,147,131,117,104,93,83,74,66
};

static ULONG IpcReadConfigDword(_In_ PCWSTR name, _In_ ULONG defaultValue)
{
    ULONG value = defaultValue, def = defaultValue;
    RTL_QUERY_REGISTRY_TABLE table[2];
    RtlZeroMemory(table, sizeof(table));
    table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    table[0].Name = (PWSTR)name;
    table[0].EntryContext = &value;
    table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
    table[0].DefaultData = &def;
    table[0].DefaultLength = sizeof(def);
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE,
            L"\\Registry\\Machine\\SOFTWARE\\CrosSofAudio", table, NULL, NULL)))
        return defaultValue;
    return value;
}

void CCsAudioSofHW::sof_reset_pipeline_state()
{
    for (ULONG p = 0; p < SOF_PATH_COUNT; p++) {
        m_path[p].pipelineReady = FALSE;
        m_path[p].paramsSet = FALSE;
        m_path[p].dspRunning = FALSE;
    }
}

/* Called right after FW_READY: headset pipeline first (known good), then
 * the speaker pipeline. Each one is independent. */
NTSTATUS CCsAudioSofHW::sof_setup_pipelines()
{
    RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, L"\\Registry\\Machine\\SOFTWARE\\CrosSofAudio");
    RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SOF_REG_IPC_LOG);

    sof_reset_pipeline_state();
    m_ipcReady = TRUE;

    IpcLogDword(L"MboxDownOffset", m_mboxDownOffset);
    IpcLogDword(L"MboxDownSize", m_mboxDownSize);
    IpcLogDword(L"MboxUpOffset", m_mboxUpOffset);
    IpcLogDword(L"MboxUpSize", m_mboxUpSize);
    IpcLogDword(L"WindowsFromManifest", m_windowsFromManifest);
    IpcLogDword(L"AdspBarLen", m_adspBar.Len);

    if (m_adspBar.Len < m_mboxDownOffset + m_mboxDownSize ||
        m_adspBar.Len < m_mboxUpOffset + m_mboxUpSize) {
        IpcLogString(L"PipelineResult", L"failed: mailbox outside DSP BAR");
        m_ipcReady = FALSE;
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    NTSTATUS hp = sof_setup_pipeline(SOF_PATH_HP);
    NTSTATUS spk = sof_setup_pipeline(SOF_PATH_SPK);

    IpcLogDword(L"IpcCount", m_ipcCount);
    IpcLogDword(L"NotifyCount", m_notifyCount);
    IpcLogDword(L"LastNotifyCmd", m_lastNotifyCmd);
    return NT_SUCCESS(hp) ? spk : hp;
}

NTSTATUS CCsAudioSofHW::sof_setup_pipeline(_In_ ULONG path)
{
    const SOF_PATH_DESC* d = &c_PathDesc[path];
    NTSTATUS status = STATUS_SUCCESS;
    LONG fwErr = 0;
    WCHAR key[16];

    m_path[path].pipelineReady = FALSE;

    ULONG atten = IpcReadConfigDword(d->attenName, d->attenDefault);
    if (atten > 60) atten = 60;
    PathLogDword(path, L"DspAttenDb", atten);

    for (ULONG i = 0; i < d->setupCount; i++) {
        if (i == d->volMsgIndex &&
            d->setup[i].size >= SOF_CTRL_CHANV0_VALUE_OFF + SOF_CTRL_CHANV_STRIDE + 4 &&
            d->setup[i].size <= SOF_IPC_MAX_SIZE) {
            /* initial volume from the registry (both channels) */
            UCHAR vol[SOF_IPC_MAX_SIZE];
            RtlCopyMemory(vol, d->setup[i].data, d->setup[i].size);
            *(UINT32*)(vol + SOF_CTRL_CHANV0_VALUE_OFF) = c_AttenQ816[atten];
            *(UINT32*)(vol + SOF_CTRL_CHANV0_VALUE_OFF + SOF_CTRL_CHANV_STRIDE) = c_AttenQ816[atten];
            status = sof_ipc_tx(vol, d->setup[i].size, NULL, 0, &fwErr);
        } else {
            status = sof_ipc_tx(d->setup[i].data, d->setup[i].size, NULL, 0, &fwErr);
        }
        RtlStringCchPrintfW(key, ARRAYSIZE(key), L"Setup%02lu", i);
        PathLogStep(path, key, d->setup[i].name, fwErr, status);
        if (!NT_SUCCESS(status)) {
            WCHAR msg[128];
            RtlStringCchPrintfW(msg, ARRAYSIZE(msg), L"failed at step %lu (%S)", i, d->setup[i].name);
            PathLogString(path, L"PipelineResult", msg);
            return status;
        }
    }

    m_path[path].pipelineReady = TRUE;
    PathLogString(path, L"PipelineResult", d->okText);
    return STATUS_SUCCESS;
}

/* Before host DMA start: DAI hw_params + STREAM_PCM_PARAMS. */
NTSTATUS CCsAudioSofHW::sof_stream_prepare(_In_ ULONG path)
{
    const SOF_PATH_DESC* d = &c_PathDesc[path];
    SOF_RENDER_PATH* rp = &m_path[path];
    LONG fwErr = 0;
    NTSTATUS status;
    UCHAR pcm[sizeof(c_SofHpPcmParams)];
    UCHAR reply[SOF_PCM_PARAMS_REPLY_SIZE];
    char what[48];

    if (!rp->pipelineReady || !rp->hStream) return STATUS_DEVICE_NOT_READY;

    rp->starts++;
    PathLogDword(path, L"StreamStarts", rp->starts);
    PathLogDword(path, L"StreamTag", rp->tag);
    PathLogDword(path, L"StreamBufferBytes", rp->bufBytes);

    status = sof_ipc_tx(d->daiHwParams, d->daiHwParamsSize, NULL, 0, &fwErr);
    RtlStringCchPrintfA(what, sizeof(what), "DAI_CONFIG %s HW_PARAMS", d->sspName);
    PathLogStep(path, L"StreamDaiHwParams", what, fwErr, status);
    if (!NT_SUCCESS(status)) return status;

    RtlCopyMemory(pcm, d->pcmParams, sizeof(pcm));
    UINT32 bytes = rp->bufBytes ? rp->bufBytes : DMA_BUFFER_SIZE;
    *(UINT32*)(pcm + SOF_PCM_PARAMS_OFF_BUF_SIZE) = bytes;
    *(UINT32*)(pcm + SOF_PCM_PARAMS_OFF_BUF_PAGES) = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    *(UINT32*)(pcm + SOF_PCM_PARAMS_OFF_BUF_PHY) = 0;     /* no host page table on cAVS (HDA DMA) */
    *(UINT16*)(pcm + SOF_PCM_PARAMS_OFF_STREAM_TAG) = rp->tag;
    *(UINT32*)(pcm + SOF_PCM_PARAMS_OFF_PERIOD_BYTES) = bytes / 2;

    RtlZeroMemory(reply, sizeof(reply));
    status = sof_ipc_tx(pcm, sizeof(pcm), reply, sizeof(reply), &fwErr);
    PathLogStep(path, L"StreamPcmParams", "STREAM_PCM_PARAMS", fwErr, status);
    if (!NT_SUCCESS(status)) return status;

    rp->paramsSet = TRUE;
    return STATUS_SUCCESS;
}

/* After host DMA start. */
NTSTATUS CCsAudioSofHW::sof_stream_trigger_start(_In_ ULONG path)
{
    SOF_RENDER_PATH* rp = &m_path[path];
    LONG fwErr = 0;
    if (!rp->paramsSet) return STATUS_DEVICE_NOT_READY;
    NTSTATUS status = sof_ipc_tx(c_PathDesc[path].trigStart, c_PathDesc[path].trigStartSize, NULL, 0, &fwErr);
    PathLogStep(path, L"StreamTrigStart", "STREAM_TRIG_START", fwErr, status);
    if (NT_SUCCESS(status)) rp->dspRunning = TRUE;
    return status;
}

/* Before host DMA stop. */
NTSTATUS CCsAudioSofHW::sof_stream_trigger_stop(_In_ ULONG path)
{
    SOF_RENDER_PATH* rp = &m_path[path];
    LONG fwErr = 0;
    if (!rp->dspRunning) return STATUS_SUCCESS;
    NTSTATUS status = sof_ipc_tx(c_PathDesc[path].trigStop, c_PathDesc[path].trigStopSize, NULL, 0, &fwErr);
    PathLogStep(path, L"StreamTrigStop", "STREAM_TRIG_STOP", fwErr, status);
    rp->dspRunning = FALSE;
    return status;
}

/* After host DMA stop and codec power-down. */
NTSTATUS CCsAudioSofHW::sof_stream_free(_In_ ULONG path)
{
    const SOF_PATH_DESC* d = &c_PathDesc[path];
    SOF_RENDER_PATH* rp = &m_path[path];
    LONG fwErr = 0;
    NTSTATUS status = STATUS_SUCCESS;
    char what[48];
    if (!rp->paramsSet) return STATUS_SUCCESS;

    status = sof_ipc_tx(d->pcmFree, d->pcmFreeSize, NULL, 0, &fwErr);
    PathLogStep(path, L"StreamPcmFree", "STREAM_PCM_FREE", fwErr, status);

    NTSTATUS s2 = sof_ipc_tx(d->daiHwFree, d->daiHwFreeSize, NULL, 0, &fwErr);
    RtlStringCchPrintfA(what, sizeof(what), "DAI_CONFIG %s HW_FREE", d->sspName);
    PathLogStep(path, L"StreamDaiHwFree", what, fwErr, s2);

    rp->paramsSet = FALSE;
    IpcLogDword(L"IpcCount", m_ipcCount);
    IpcLogDword(L"IpcTimeouts", m_ipcTimeouts);
    IpcLogDword(L"NotifyCount", m_notifyCount);
    IpcLogDword(L"LastNotifyCmd", m_lastNotifyCmd);
    return status;
}

/* ----------------------------------------------------------------------
 * Data-flow snapshot of one render path (taken just before it stops):
 *   - HDA host output stream: SD_CTL/STS and LPIB sampled 20 ms apart
 *   - DSP firmware status word, pending DSP->host message
 *   - the path's SSP port: SSCR0.SSE = enabled, SSSR = FIFO level/underrun
 * Logged to HKLM\SOFTWARE\CrosSofAudio\DspDiag as <prefix>_<name>.
 * ---------------------------------------------------------------------- */
#define SOF_REG_DIAG_LOG    L"\\Registry\\Machine\\SOFTWARE\\CrosSofAudio\\DspDiag"

static void DiagLog(PCWSTR prefix, PCWSTR name, ULONG v)
{
    WCHAR key[64];
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    if (!NT_SUCCESS(RtlStringCchPrintfW(key, ARRAYSIZE(key), L"%s_%s", prefix, name))) return;
    RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SOF_REG_DIAG_LOG, key, REG_DWORD, &v, sizeof(v));
}

void CCsAudioSofHW::sof_diag_snapshot(_In_ ULONG path, _In_ PCWSTR prefix)
{
    PUCHAR hda = m_hdaBar.Base.baseptr;
    PUCHAR dsp = m_adspBar.Base.baseptr;
    UINT8 tag = m_path[path].tag;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;

    ULONG sd = 0;
    if (hda && m_hdaBar.Len >= 0x100) {
        USHORT gcap = READ_REGISTER_USHORT((volatile USHORT*)(hda + 0x00));
        ULONG iss = (gcap >> 8) & 0xF;
        ULONG oss = (gcap >> 12) & 0xF;
        if (tag >= 1 && tag <= oss) {
            sd = 0x80 + 0x20 * (iss + tag - 1);
            if (sd + 0x20 > m_hdaBar.Len) sd = 0;
        }
        DiagLog(prefix, L"StreamTag", tag);
        if (sd) {
            DiagLog(prefix, L"SD_CTLSTS", IpcRd(hda, sd + 0x00));   /* CTL[23:0], STS[31:24] */
            DiagLog(prefix, L"SD_LPIB_A", IpcRd(hda, sd + 0x04));
        }
    }

    const ULONG ssp = SOF_CNL_SSP_BASE_OFFSET + c_PathDesc[path].sspIndex * SOF_SSP_DEV_MEM_SIZE;
    BOOLEAN dspOk = (dsp && m_adspBar.Len >= SOF_SRAM_WINDOW_OFFSET(1));
    if (dspOk) {
        DiagLog(prefix, L"FwStatus",  IpcRd(dsp, SOF_SRAM_WINDOW_OFFSET(0) + 0x0));
        DiagLog(prefix, L"HIPCTDR",   IpcRd(dsp, SOF_CNL_HIPCTDR));
        DiagLog(prefix, L"SSCR0", IpcRd(dsp, ssp + 0x00));
        DiagLog(prefix, L"SSCR1", IpcRd(dsp, ssp + 0x04));
        DiagLog(prefix, L"SSSR_A", IpcRd(dsp, ssp + 0x08));
        DiagLog(prefix, L"SSPSP", IpcRd(dsp, ssp + 0x2C));
        DiagLog(prefix, L"SSTSA", IpcRd(dsp, ssp + 0x30));
        DiagLog(prefix, L"SSRSA", IpcRd(dsp, ssp + 0x34));
    }

    LARGE_INTEGER d; d.QuadPart = -200000;  /* 20 ms */
    KeDelayExecutionThread(KernelMode, FALSE, &d);
    if (sd) {
        DiagLog(prefix, L"SD_LPIB_B", IpcRd(hda, sd + 0x04));
    }
    if (dspOk) {
        DiagLog(prefix, L"SSSR_B", IpcRd(dsp, ssp + 0x08));
        if (IpcRd(dsp, SOF_CNL_HIPCTDR) & SOF_CNL_HIPCTDR_BUSY) {
            UINT32 hdr[2] = { 0, 0 };
            READ_REGISTER_BUFFER_ULONG((volatile ULONG*)(dsp + m_mboxUpOffset), (PULONG)hdr, 2);
            DiagLog(prefix, L"PendingMsgCmd", hdr[1]);
        }
    }
}
