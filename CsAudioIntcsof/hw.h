/*++

Module Name:

    hw.h

Abstract:

    Declaration of Intel Sound Open Firmware (SOF) Audio Hardware Interface class.
    Manages ADSP_BUS_INTERFACE bus communication with SklHdaudBus,
    cyclic DMA stream allocations, and sidecar codec control (MAX98373 & NAU88L25).

--*/

#ifndef _CSAUDIOINTCSOF_HW_H_
#define _CSAUDIOINTCSOF_HW_H_

#include <hdaudio.h>
#include "..\SklHdaudBus\adsp.h"
#include "..\Common\csaudio_codec.h"

typedef struct _HDAC_BDLENTRY {
    UINT32 lowAddr;
    UINT32 highAddr;
    UINT32 len;
    UINT32 ioc;
} HDAC_BDLENTRY, *PHDAC_BDLENTRY;



#define MAX_TOPOLOGY_NODES      20

enum { SOF_PATH_SPK = 0, SOF_PATH_HP = 1, SOF_PATH_COUNT = 2, SOF_PATH_NONE = 0xFF };

typedef struct _SOF_RENDER_PATH {
    HANDLE      hStream;        // HDA host output stream (from SklHdaudBus)
    UINT8       tag;            // its stream tag (= DSP host DMA channel + 1)
    SHORT       running;        // WaveRT streams currently running on it
    UINT32      bufBytes;       // WaveRT cyclic buffer size
    BOOLEAN     pipelineReady;  // DSP pipeline created
    BOOLEAN     paramsSet;      // PCM_PARAMS sent
    BOOLEAN     dspRunning;     // TRIG_START sent
    ULONG       starts;
} SOF_RENDER_PATH;

class CCsAudioSofHW
{
public:
    int CsAudioArg2;
    void CSAudioAPICalled(CsAudioArg arg);

private:
    PCALLBACK_OBJECT            m_CsAudioAPICallback;
    PVOID                       m_CsAudioAPICallbackObj;
    PVOID                       m_pnpNotificationHandle;

    // Sidecar Device Objects (MAX98373 smart amp & NAU88L25 headset codec)
    PFILE_OBJECT                m_SpeakerFileObject;
    PDEVICE_OBJECT              m_SpeakerDeviceObject;
    PFILE_OBJECT                m_HeadphoneFileObject;
    PDEVICE_OBJECT              m_HeadphoneDeviceObject;

    NTSTATUS CSAudioAPIInit();
    NTSTATUS CSAudioAPIDeinit();
    NTSTATUS NotifySidecarCodec(CSAudioEndpointType endpoint, BOOL start);
    void     SendSidecarVolume(CSAudioEndpointType endpoint, LONG volumeLevel, BOOL muted);

    CSAudioEndpointType GetCSAudioEndpoint(eDeviceType deviceType);
    eDeviceType         GetDeviceType(CSAudioEndpointType endpointType);

protected:
    LONG                        m_PeakMeterControls[MAX_TOPOLOGY_NODES];
    ULONG                       m_ulMux;            // Mux selection
    BOOL                        m_bDevSpecific;
    INT                         m_iDevSpecific;
    UINT                        m_uiDevSpecific;

    // ADSP Bus Interface provided by SklHdaudBus
    ADSP_BUS_INTERFACE          m_BusInterface;
    BOOLEAN                     m_BusInterfaceValid;
    PCI_BAR                     m_hdaBar;
    PCI_BAR                     m_adspBar;
    PVOID                       m_ppcap;
    NHLT_INFO                   m_nhltInfo;
    BUS_INTERFACE_STANDARD      m_pciConfig;

    // Render paths: one host DMA stream + one DSP pipeline per output.
    //   SOF_PATH_SPK: speaker endpoint  -> pipeline 1 -> SSP1 -> 2x MAX98373
    //   SOF_PATH_HP : headphone endpoint -> pipeline 2 -> SSP0 -> NAU88L25
    SOF_RENDER_PATH             m_path[SOF_PATH_COUNT];

    HANDLE                      m_hCaptureStream;
    UINT8                       m_captureStreamTag;
    SHORT                       m_runningCaptureStreams;

    // Hardware parameters
    UINT32                      m_sampleRate;
    UINT16                      m_channels;
    UINT16                      m_bitsPerSample;

    PDEVICE_OBJECT              m_pDeviceObject;

    // SOF firmware boot state (sof_boot.cpp)
    BOOLEAN                     m_dspBooted;
    BOOLEAN                     m_gprocenWasSet;
    BOOLEAN                     m_gprocenOn;
    ULONG                       m_bootStage;
    ULONG                       m_bootCount;
    UINT32                      m_dspBoxOffset;
    UINT32                      m_dspBoxSize;
    UINT32                      m_hostBoxOffset;
    UINT32                      m_hostBoxSize;

    // IPC / pipeline state (sof_ipc.cpp)
    FAST_MUTEX                  m_ipcMutex;
    BOOLEAN                     m_ipcReady;
    BOOLEAN                     m_windowsFromManifest;
    UINT32                      m_mboxDownOffset;
    UINT32                      m_mboxDownSize;
    UINT32                      m_mboxUpOffset;
    UINT32                      m_mboxUpSize;
    ULONG                       m_ipcCount;
    ULONG                       m_ipcTimeouts;
    ULONG                       m_ipcSkippedIrql;
    ULONG                       m_notifyCount;
    UINT32                      m_lastIpcCmd;
    UINT32                      m_lastNotifyCmd;

    void     sof_parse_windows(_In_ const UCHAR* fw, _In_ ULONG fwSize);
    void     sof_ipc_service_notifications();
    NTSTATUS sof_ipc_tx(_In_reads_bytes_(size) const void* msg, _In_ ULONG size,
                        _Out_writes_bytes_opt_(replySize) void* reply, _In_ ULONG replySize,
                        _Out_ LONG* fwError);
    NTSTATUS sof_setup_pipelines();
    NTSTATUS sof_setup_pipeline(_In_ ULONG path);
    NTSTATUS sof_stream_prepare(_In_ ULONG path);
    NTSTATUS sof_stream_trigger_start(_In_ ULONG path);
    NTSTATUS sof_stream_trigger_stop(_In_ ULONG path);
    NTSTATUS sof_stream_free(_In_ ULONG path);
    void     sof_diag_snapshot(_In_ ULONG path, _In_ PCWSTR prefix);
    ULONG    sof_path_for(_In_ eDeviceType deviceType);
    void     sof_reset_pipeline_state();
    void     NotifyCodecs(_In_ CSAudioEndpointType endpointType, _In_ BOOLEAN start);

    BOOLEAN  sof_is_supported_platform();
    NTSTATUS sof_boot_dsp();
    NTSTATUS sof_wait_fw_ready();
    void     sof_dsp_power_down();
    void     sof_log_stage(_In_ ULONG stage, _In_ NTSTATUS status);

public:
    CCsAudioSofHW(_In_ PRESOURCELIST ResourceList, _In_ PDEVICE_OBJECT DeviceObject);
    ~CCsAudioSofHW();

    bool                        ResourcesValidated();

    NTSTATUS sof_init();
    NTSTATUS sof_deinit();

    NTSTATUS sof_hw_params(eDeviceType deviceType);
    NTSTATUS sof_program_dma(eDeviceType deviceType, PMDL mdl, IPortWaveRTStream* stream);
    NTSTATUS sof_play(eDeviceType deviceType, UINT32 byteCount);
    NTSTATUS sof_stop(eDeviceType deviceType);
    NTSTATUS sof_current_position(eDeviceType deviceType, UINT32* linkPos, UINT64* linearPos);
    
    void                        MixerReset();
    BOOL                        bGetDevSpecific();
    void                        bSetDevSpecific(_In_ BOOL bDevSpecific);
    INT                         iGetDevSpecific();
    void                        iSetDevSpecific(_In_ INT iDevSpecific);
    UINT                        uiGetDevSpecific();
    void                        uiSetDevSpecific(_In_ UINT uiDevSpecific);

    ULONG                       GetMixerMux();
    void                        SetMixerMux(_In_ ULONG ulMux);

    LONG                        GetPeakMeterValue(_In_ ULONG ulNode, _In_ ULONG ulChannel);
    void                        SetVolume(_In_ eDeviceType deviceType, _In_ LONG volumeLevel);
    void                        SetMute(_In_ eDeviceType deviceType, _In_ BOOL muted);
};

typedef CCsAudioSofHW* PCCsAudioSofHW;

#endif // _CSAUDIOINTCSOF_HW_H_
