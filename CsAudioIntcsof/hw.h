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

typedef enum {
    CSAudioEndpointTypeDSP,
    CSAudioEndpointTypeSpeaker,
    CSAudioEndpointTypeHeadphone,
    CSAudioEndpointTypeMicArray,
    CSAudioEndpointTypeMicJack
} CSAudioEndpointType;

typedef enum {
    CSAudioEndpointRegister,
    CSAudioEndpointStart,
    CSAudioEndpointStop,
    CSAudioEndpointOverrideFormat
} CSAudioEndpointRequest;

typedef struct CSAUDIOFORMATOVERRIDE {
    UINT16 channels;
    UINT16 frequency;
    UINT16 bitsPerSample;
    UINT16 validBitsPerSample;
    BOOL force32BitOutputContainer;
} CsAudioFormatOverride;

typedef struct CSAUDIOARG {
    UINT32 argSz;
    CSAudioEndpointType endpointType;
    CSAudioEndpointRequest endpointRequest;
    union {
        CsAudioFormatOverride formatOverride;
    };
} CsAudioArg, * PCsAudioArg;

#define MAX_TOPOLOGY_NODES      20

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
    void     NotifySidecarCodec(CSAudioEndpointType endpoint, BOOL start);
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

    // Streams managed through ADSP_BUS_INTERFACE
    HANDLE                      m_hRenderStream;
    UINT8                       m_renderStreamTag;
    SHORT                       m_runningRenderStreams;

    HANDLE                      m_hCaptureStream;
    UINT8                       m_captureStreamTag;
    SHORT                       m_runningCaptureStreams;

    // Hardware parameters
    UINT32                      m_sampleRate;
    UINT16                      m_channels;
    UINT16                      m_bitsPerSample;

    PDEVICE_OBJECT              m_pDeviceObject;

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
