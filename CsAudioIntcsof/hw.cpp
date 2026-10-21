/*++

Module Name:

    hw.cpp

Abstract:

    Implementation of Intel Sound Open Firmware (SOF) Audio Hardware Interface.
    Communicates with SklHdaudBus via GUID_ADSP_BUS_INTERFACE for DSP DMA engines,
    and controls Maxim MAX98373 and Nuvoton NAU88L25 companion codecs.

--*/

#include "definitions.h"
#include "endpoints.h"
#include "hw.h"

#define BDL_BLOCK_SIZE 4096

static NTSTATUS QueryAdspBusInterface(
    _In_  PDEVICE_OBJECT DeviceObject,
    _Out_ PADSP_BUS_INTERFACE BusInterface
)
{
    IO_STATUS_BLOCK ioStatus;
    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    PIRP irp = IoBuildSynchronousFsdRequest(
        IRP_MJ_PNP,
        DeviceObject,
        NULL,
        0,
        NULL,
        &event,
        &ioStatus
    );

    if (!irp) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PIO_STACK_LOCATION irpSp = IoGetNextIrpStackLocation(irp);
    irpSp->MinorFunction = IRP_MN_QUERY_INTERFACE;
    irpSp->Parameters.QueryInterface.InterfaceType = (LPGUID)&GUID_ADSP_BUS_INTERFACE;
    irpSp->Parameters.QueryInterface.Size = sizeof(ADSP_BUS_INTERFACE);
    irpSp->Parameters.QueryInterface.Version = 1;
    irpSp->Parameters.QueryInterface.Interface = (PINTERFACE)BusInterface;
    irpSp->Parameters.QueryInterface.InterfaceSpecificData = NULL;

    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    NTSTATUS status = IoCallDriver(DeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }

    return status;
}

#pragma code_seg("PAGE")
CCsAudioSofHW::CCsAudioSofHW(_In_ PRESOURCELIST ResourceList, _In_ PDEVICE_OBJECT DeviceObject)
    : m_ulMux(0),
      m_bDevSpecific(FALSE),
      m_iDevSpecific(0),
      m_uiDevSpecific(0),
      m_BusInterfaceValid(FALSE),
      m_hRenderStream(NULL),
      m_renderStreamTag(0),
      m_runningRenderStreams(0),
      m_hCaptureStream(NULL),
      m_captureStreamTag(0),
      m_runningCaptureStreams(0),
      m_sampleRate(48000),
      m_channels(2),
      m_bitsPerSample(16),
      m_pDeviceObject(DeviceObject),
      m_CsAudioAPICallback(NULL),
      m_CsAudioAPICallbackObj(NULL),
      m_pnpNotificationHandle(NULL),
      m_SpeakerFileObject(NULL),
      m_SpeakerDeviceObject(NULL),
      m_HeadphoneFileObject(NULL),
      m_HeadphoneDeviceObject(NULL)
{
    UNREFERENCED_PARAMETER(ResourceList);
    PAGED_CODE();

    RtlZeroMemory(&m_BusInterface, sizeof(m_BusInterface));
    RtlZeroMemory(&m_hdaBar, sizeof(m_hdaBar));
    RtlZeroMemory(&m_adspBar, sizeof(m_adspBar));
    RtlZeroMemory(&m_nhltInfo, sizeof(m_nhltInfo));
    RtlZeroMemory(&m_pciConfig, sizeof(m_pciConfig));

    if (DeviceObject) {
        NTSTATUS status = QueryAdspBusInterface(DeviceObject, &m_BusInterface);
        if (NT_SUCCESS(status)) {
            m_BusInterfaceValid = TRUE;
            DPF(D_TERSE, ("[CCsAudioSofHW] Successfully acquired GUID_ADSP_BUS_INTERFACE"));
        } else {
            DPF(D_ERROR, ("[CCsAudioSofHW] QueryAdspBusInterface failed: 0x%08X", status));
        }
    }

    MixerReset();
}

#pragma code_seg()
bool CCsAudioSofHW::ResourcesValidated()
{
    if (!m_BusInterfaceValid) {
        return false;
    }
    return NT_SUCCESS(this->CSAudioAPIInit());
}

CCsAudioSofHW::~CCsAudioSofHW()
{
    this->sof_deinit();
    this->CSAudioAPIDeinit();
}

NTSTATUS CCsAudioSofHW::sof_init()
{
    if (!m_BusInterfaceValid) {
        return STATUS_NO_SUCH_DEVICE;
    }

    // Power on Intel Audio DSP
    if (m_BusInterface.SetDSPPowerState) {
        m_BusInterface.SetDSPPowerState(m_BusInterface.Context, PowerDeviceD0);
    }

    // Retrieve MMIO register BARs and NHLT topology
    if (m_BusInterface.GetResources) {
        m_BusInterface.GetResources(
            m_BusInterface.Context,
            &m_hdaBar,
            &m_adspBar,
            &m_ppcap,
            &m_nhltInfo,
            &m_pciConfig
        );
    }

    return STATUS_SUCCESS;
}

NTSTATUS CCsAudioSofHW::sof_deinit()
{
    if (m_hRenderStream && m_BusInterface.FreeStream) {
        m_BusInterface.FreeStream(m_BusInterface.Context, m_hRenderStream);
        m_hRenderStream = NULL;
    }

    if (m_hCaptureStream && m_BusInterface.FreeStream) {
        m_BusInterface.FreeStream(m_BusInterface.Context, m_hCaptureStream);
        m_hCaptureStream = NULL;
    }

    if (m_BusInterfaceValid && m_BusInterface.SetDSPPowerState) {
        m_BusInterface.SetDSPPowerState(m_BusInterface.Context, PowerDeviceD3);
    }

    return STATUS_SUCCESS;
}

NTSTATUS CCsAudioSofHW::sof_hw_params(eDeviceType deviceType)
{
    UNREFERENCED_PARAMETER(deviceType);
    return STATUS_SUCCESS;
}

NTSTATUS CCsAudioSofHW::sof_program_dma(eDeviceType deviceType, PMDL mdl, IPortWaveRTStream* stream)
{
    UNREFERENCED_PARAMETER(stream);

    if (!m_BusInterfaceValid) {
        return STATUS_NO_SUCH_DEVICE;
    }

    HDAUDIO_STREAM_FORMAT streamFormat;
    RtlZeroMemory(&streamFormat, sizeof(streamFormat));
    streamFormat.SampleRate = 48000;
    streamFormat.NumberOfChannels = 2;
    streamFormat.ValidBitsPerSample = 16;
    streamFormat.ContainerSize = 16;

    ULONG totalBytes = mdl ? MmGetMdlByteCount(mdl) : DMA_BUFFER_SIZE;
    if (totalBytes == 0) totalBytes = DMA_BUFFER_SIZE;
    int numBlocks = (int)(totalBytes / BDL_BLOCK_SIZE);
    if (numBlocks <= 0) numBlocks = 1;

    if (deviceType == eSpeakerDevice || deviceType == eHeadphoneDevice) {
        if (!m_hRenderStream && m_BusInterface.GetRenderStream) {
            NTSTATUS status = m_BusInterface.GetRenderStream(
                m_BusInterface.Context,
                streamFormat,
                &m_hRenderStream,
                &m_renderStreamTag
            );
            if (!NT_SUCCESS(status)) {
                return status;
            }
        }

        if (m_hRenderStream && m_BusInterface.PrepareDSP) {
            PVOID bdlBuf = NULL;
            NTSTATUS status = m_BusInterface.PrepareDSP(
                m_BusInterface.Context,
                m_hRenderStream,
                totalBytes,
                numBlocks,
                &bdlBuf
            );
            if (!NT_SUCCESS(status)) {
                return status;
            }

            if (bdlBuf && mdl) {
                PHDAC_BDLENTRY bdl = (PHDAC_BDLENTRY)bdlBuf;
                PPFN_NUMBER pfnArray = MmGetMdlPfnArray(mdl);
                ULONG remaining = totalBytes;
                ULONG offset = 0;
                ULONG pageIndex = 0;

                for (int i = 0; i < numBlocks && remaining > 0; i++) {
                    ULONG pageOff = offset % PAGE_SIZE;
                    ULONG chunk = (pageOff == 0) ? PAGE_SIZE : (PAGE_SIZE - pageOff);
                    if (chunk > remaining) chunk = remaining;
                    if (chunk > BDL_BLOCK_SIZE) chunk = BDL_BLOCK_SIZE;

                    PFN_NUMBER pfn = pfnArray[pageIndex];
                    PHYSICAL_ADDRESS addr;
                    addr.QuadPart = (LONGLONG)pfn << PAGE_SHIFT;

                    bdl[i].lowAddr = addr.LowPart + pageOff;
                    bdl[i].highAddr = addr.HighPart;
                    bdl[i].len = chunk;
                    remaining -= chunk;
                    offset += chunk;
                    if ((offset % PAGE_SIZE) == 0) pageIndex++;

                    bdl[i].ioc = (remaining == 0 || remaining == totalBytes / 2) ? 1 : 0;
                }
            }
            return STATUS_SUCCESS;
        }
    } else if (deviceType == eMicArrayDevice1 || deviceType == eMicJackDevice) {
        if (!m_hCaptureStream && m_BusInterface.GetCaptureStream) {
            NTSTATUS status = m_BusInterface.GetCaptureStream(
                m_BusInterface.Context,
                streamFormat,
                &m_hCaptureStream,
                &m_captureStreamTag
            );
            if (!NT_SUCCESS(status)) {
                return status;
            }
        }

        if (m_hCaptureStream && m_BusInterface.PrepareDSP) {
            PVOID bdlBuf = NULL;
            NTSTATUS status = m_BusInterface.PrepareDSP(
                m_BusInterface.Context,
                m_hCaptureStream,
                totalBytes,
                numBlocks,
                &bdlBuf
            );
            if (!NT_SUCCESS(status)) {
                return status;
            }

            if (bdlBuf && mdl) {
                PHDAC_BDLENTRY bdl = (PHDAC_BDLENTRY)bdlBuf;
                PPFN_NUMBER pfnArray = MmGetMdlPfnArray(mdl);
                ULONG remaining = totalBytes;
                ULONG offset = 0;
                ULONG pageIndex = 0;

                for (int i = 0; i < numBlocks && remaining > 0; i++) {
                    ULONG pageOff = offset % PAGE_SIZE;
                    ULONG chunk = (pageOff == 0) ? PAGE_SIZE : (PAGE_SIZE - pageOff);
                    if (chunk > remaining) chunk = remaining;
                    if (chunk > BDL_BLOCK_SIZE) chunk = BDL_BLOCK_SIZE;

                    PFN_NUMBER pfn = pfnArray[pageIndex];
                    PHYSICAL_ADDRESS addr;
                    addr.QuadPart = (LONGLONG)pfn << PAGE_SHIFT;

                    bdl[i].lowAddr = addr.LowPart + pageOff;
                    bdl[i].highAddr = addr.HighPart;
                    bdl[i].len = chunk;
                    remaining -= chunk;
                    offset += chunk;
                    if ((offset % PAGE_SIZE) == 0) pageIndex++;

                    bdl[i].ioc = (remaining == 0 || remaining == totalBytes / 2) ? 1 : 0;
                }
            }
            return STATUS_SUCCESS;
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS CCsAudioSofHW::sof_play(eDeviceType deviceType, UINT32 byteCount)
{
    UNREFERENCED_PARAMETER(byteCount);

    if (deviceType == eSpeakerDevice || deviceType == eHeadphoneDevice) {
        if (m_hRenderStream && m_BusInterface.TriggerDSP) {
            m_BusInterface.TriggerDSP(m_BusInterface.Context, m_hRenderStream, TRUE);
        }
        InterlockedIncrement16(&m_runningRenderStreams);
    } else {
        if (m_hCaptureStream && m_BusInterface.TriggerDSP) {
            m_BusInterface.TriggerDSP(m_BusInterface.Context, m_hCaptureStream, TRUE);
        }
        InterlockedIncrement16(&m_runningCaptureStreams);
    }

    // 1. Notify listeners on CsAudioCallbackAPI
    CsAudioArg arg;
    RtlZeroMemory(&arg, sizeof(CsAudioArg));
    arg.argSz = sizeof(CsAudioArg);
    arg.endpointType = GetCSAudioEndpoint(deviceType);
    arg.endpointRequest = CSAudioEndpointStart;
    if (m_CsAudioAPICallback) {
        ExNotifyCallback(m_CsAudioAPICallback, &arg, &CsAudioArg2);
    }

    // 2. Notify registered sidecar codec (MAX98373 for speakers, NAU88L25 for headphones)
    NTSTATUS sidecarStatus = NotifySidecarCodec(arg.endpointType, TRUE);
    if (!NT_SUCCESS(sidecarStatus)) {
        DPF(D_TERSE, ("[CsAudioIntcsof] sof_play: sidecar codec notification returned 0x%08X (continuing playback)\n", sidecarStatus));
    }

    return STATUS_SUCCESS;
}

NTSTATUS CCsAudioSofHW::sof_stop(eDeviceType deviceType)
{
    if (deviceType == eSpeakerDevice || deviceType == eHeadphoneDevice) {
        if (m_runningRenderStreams <= 0) {
            return STATUS_SUCCESS;
        }
        InterlockedDecrement16(&m_runningRenderStreams);
        if (m_hRenderStream && m_BusInterface.TriggerDSP) {
            m_BusInterface.TriggerDSP(m_BusInterface.Context, m_hRenderStream, FALSE);
        }
    } else {
        if (m_runningCaptureStreams <= 0) {
            return STATUS_SUCCESS;
        }
        InterlockedDecrement16(&m_runningCaptureStreams);
        if (m_hCaptureStream && m_BusInterface.TriggerDSP) {
            m_BusInterface.TriggerDSP(m_BusInterface.Context, m_hCaptureStream, FALSE);
        }
    }

    // 1. Notify listeners on CsAudioCallbackAPI
    CsAudioArg arg;
    RtlZeroMemory(&arg, sizeof(CsAudioArg));
    arg.argSz = sizeof(CsAudioArg);
    arg.endpointType = GetCSAudioEndpoint(deviceType);
    arg.endpointRequest = CSAudioEndpointStop;
    if (m_CsAudioAPICallback) {
        ExNotifyCallback(m_CsAudioAPICallback, &arg, &CsAudioArg2);
    }

    // 2. Notify sidecar codec to enter power saving / low power state
    NTSTATUS sidecarStatus = NotifySidecarCodec(arg.endpointType, FALSE);
    if (!NT_SUCCESS(sidecarStatus) && sidecarStatus != STATUS_DEVICE_NOT_CONNECTED) {
        DPF(D_TERSE, ("[CsAudioIntcsof] sof_stop: sidecar codec notification returned 0x%08X\n", sidecarStatus));
    }

    return STATUS_SUCCESS;
}

NTSTATUS CCsAudioSofHW::sof_current_position(eDeviceType deviceType, UINT32* linkPos, UINT64* linearPos)
{
    if (deviceType == eSpeakerDevice || deviceType == eHeadphoneDevice) {
        if (m_hRenderStream && m_BusInterface.StreamPosition) {
            UINT32 pos = m_BusInterface.StreamPosition(m_BusInterface.Context, m_hRenderStream);
            if (linkPos) *linkPos = pos;
            if (linearPos) *linearPos = (UINT64)pos;
            return STATUS_SUCCESS;
        }
    } else {
        if (m_hCaptureStream && m_BusInterface.StreamPosition) {
            UINT32 pos = m_BusInterface.StreamPosition(m_BusInterface.Context, m_hCaptureStream);
            if (linkPos) *linkPos = pos;
            if (linearPos) *linearPos = (UINT64)pos;
            return STATUS_SUCCESS;
        }
    }

    if (linkPos) *linkPos = 0;
    if (linearPos) *linearPos = 0;
    return STATUS_SUCCESS;
}

void CCsAudioSofHW::MixerReset()
{
    RtlZeroMemory(m_PeakMeterControls, sizeof(m_PeakMeterControls));
    m_ulMux = 0;
    m_bDevSpecific = FALSE;
    m_iDevSpecific = 0;
    m_uiDevSpecific = 0;
}

BOOL CCsAudioSofHW::bGetDevSpecific() { return m_bDevSpecific; }
void CCsAudioSofHW::bSetDevSpecific(_In_ BOOL bDevSpecific) { m_bDevSpecific = bDevSpecific; }
INT  CCsAudioSofHW::iGetDevSpecific() { return m_iDevSpecific; }
void CCsAudioSofHW::iSetDevSpecific(_In_ INT iDevSpecific) { m_iDevSpecific = iDevSpecific; }
UINT CCsAudioSofHW::uiGetDevSpecific() { return m_uiDevSpecific; }
void CCsAudioSofHW::uiSetDevSpecific(_In_ UINT uiDevSpecific) { m_uiDevSpecific = uiDevSpecific; }
ULONG CCsAudioSofHW::GetMixerMux() { return m_ulMux; }
void  CCsAudioSofHW::SetMixerMux(_In_ ULONG ulMux) { m_ulMux = ulMux; }

LONG CCsAudioSofHW::GetPeakMeterValue(_In_ ULONG ulNode, _In_ ULONG ulChannel)
{
    UNREFERENCED_PARAMETER(ulChannel);
    if (ulNode < MAX_TOPOLOGY_NODES) {
        return m_PeakMeterControls[ulNode];
    }
    return 0;
}

void CCsAudioSofHW::SetVolume(_In_ eDeviceType deviceType, _In_ LONG volumeLevel)
{
    CSAudioEndpointType ep = GetCSAudioEndpoint(deviceType);
    SendSidecarVolume(ep, volumeLevel, FALSE);
}

void CCsAudioSofHW::SetMute(_In_ eDeviceType deviceType, _In_ BOOL muted)
{
    CSAudioEndpointType ep = GetCSAudioEndpoint(deviceType);
    SendSidecarVolume(ep, 0, muted);
}

CSAudioEndpointType CCsAudioSofHW::GetCSAudioEndpoint(eDeviceType deviceType)
{
    switch (deviceType) {
    case eSpeakerDevice:
        return CSAudioEndpointTypeSpeaker;
    case eHeadphoneDevice:
        return CSAudioEndpointTypeHeadphone;
    case eMicArrayDevice1:
        return CSAudioEndpointTypeMicArray;
    case eMicJackDevice:
        return CSAudioEndpointTypeMicJack;
    default:
        return CSAudioEndpointTypeDSP;
    }
}

eDeviceType CCsAudioSofHW::GetDeviceType(CSAudioEndpointType endpointType)
{
    switch (endpointType) {
    case CSAudioEndpointTypeSpeaker:
        return eSpeakerDevice;
    case CSAudioEndpointTypeHeadphone:
        return eHeadphoneDevice;
    case CSAudioEndpointTypeMicArray:
        return eMicArrayDevice1;
    case CSAudioEndpointTypeMicJack:
        return eMicJackDevice;
    default:
        return eSpeakerDevice;
    }
}
