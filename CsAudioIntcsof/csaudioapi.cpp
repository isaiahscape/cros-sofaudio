/*++

Module Name:

    csaudioapi.cpp

Abstract:

    Cross-driver interop and sidecar codec bridge for Intel SOF Audio.
    Communicates with Maxim MAX98373 smart amplifier and Nuvoton NAU88L25
    codec drivers using \CallBack\CsAudioCallbackAPI and direct IOCTLs
    over GUID_DEVINTERFACE_CSAUDIO_CODEC.

--*/

#include "definitions.h"
#include "endpoints.h"
#include "hw.h"
#include <ioevent.h>

#ifndef GUID_DEVICE_INTERFACE_ARRIVAL
// {CB3A4004-46F0-11D0-B08F-00609713053F}
DEFINE_GUID(GUID_DEVICE_INTERFACE_ARRIVAL,
    0xcb3a4004, 0x46f0, 0x11d0, 0xb0, 0x8f, 0x00, 0x60, 0x97, 0x13, 0x05, 0x3f);
#endif

static VOID CsAudioCallbackFunction(
    CCsAudioSofHW* hw,
    CsAudioArg* arg,
    PVOID Argument2
)
{
    if (!hw) {
        return;
    }

    if (Argument2 == &hw->CsAudioArg2) {
        return;
    }

    CsAudioArg localArg;
    RtlZeroMemory(&localArg, sizeof(CsAudioArg));
    RtlCopyMemory(&localArg, arg, min(arg->argSz, sizeof(CsAudioArg)));

    hw->CSAudioAPICalled(localArg);
}

static NTSTATUS CsAudioPnpNotificationCallback(
    _In_ PVOID NotificationStructure,
    _Inout_opt_ PVOID Context
)
{
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION notification =
        (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)NotificationStructure;
    CCsAudioSofHW* hw = (CCsAudioSofHW*)Context;

    if (!hw || !notification) {
        return STATUS_SUCCESS;
    }

    if (IsEqualGUID(notification->Event, GUID_DEVICE_INTERFACE_ARRIVAL)) {
        DPF(D_TERSE, ("[CSAudio] Detected sidecar codec interface arrival: %wZ", notification->SymbolicLinkName));
    }

    return STATUS_SUCCESS;
}

NTSTATUS CCsAudioSofHW::CSAudioAPIInit()
{
    NTSTATUS status;

    UNICODE_STRING CSAudioCallbackAPI;
    RtlInitUnicodeString(&CSAudioCallbackAPI, L"\\CallBack\\CsAudioCallbackAPI");

    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(
        &attributes,
        &CSAudioCallbackAPI,
        OBJ_KERNEL_HANDLE | OBJ_OPENIF | OBJ_CASE_INSENSITIVE | OBJ_PERMANENT,
        NULL,
        NULL
    );

    status = ExCreateCallback(&this->m_CsAudioAPICallback, &attributes, TRUE, TRUE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    this->m_CsAudioAPICallbackObj = ExRegisterCallback(
        this->m_CsAudioAPICallback,
        (PCALLBACK_FUNCTION)CsAudioCallbackFunction,
        this
    );

    if (!this->m_CsAudioAPICallbackObj) {
        return STATUS_NO_CALLBACK_ACTIVE;
    }

    // Register for PnP notifications of companion codecs
    if (m_pDeviceObject && m_pDeviceObject->DriverObject) {
        IoRegisterPlugPlayNotification(
            EventCategoryDeviceInterfaceChange,
            PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
            (PVOID)&GUID_DEVINTERFACE_CSAUDIO_CODEC,
            m_pDeviceObject->DriverObject,
            CsAudioPnpNotificationCallback,
            this,
            &m_pnpNotificationHandle
        );
    }

    // Broadcast registration
    CsAudioArg arg;
    RtlZeroMemory(&arg, sizeof(CsAudioArg));
    arg.argSz = sizeof(CsAudioArg);
    arg.endpointType = CSAudioEndpointTypeDSP;
    arg.endpointRequest = CSAudioEndpointRegister;
    ExNotifyCallback(this->m_CsAudioAPICallback, &arg, &CsAudioArg2);

    return STATUS_SUCCESS;
}

NTSTATUS CCsAudioSofHW::CSAudioAPIDeinit()
{
    if (m_pnpNotificationHandle) {
        IoUnregisterPlugPlayNotification(m_pnpNotificationHandle);
        m_pnpNotificationHandle = NULL;
    }

    if (this->m_CsAudioAPICallbackObj) {
        ExUnregisterCallback(this->m_CsAudioAPICallbackObj);
        this->m_CsAudioAPICallbackObj = NULL;
    }

    if (this->m_CsAudioAPICallback) {
        ObfDereferenceObject(this->m_CsAudioAPICallback);
        this->m_CsAudioAPICallback = NULL;
    }

    if (m_SpeakerFileObject) {
        ObDereferenceObject(m_SpeakerFileObject);
        m_SpeakerFileObject = NULL;
        m_SpeakerDeviceObject = NULL;
    }

    if (m_HeadphoneFileObject) {
        ObDereferenceObject(m_HeadphoneFileObject);
        m_HeadphoneFileObject = NULL;
        m_HeadphoneDeviceObject = NULL;
    }

    return STATUS_SUCCESS;
}

void CCsAudioSofHW::CSAudioAPICalled(CsAudioArg arg)
{
    if (arg.endpointRequest == CSAudioEndpointRegister) {
        CsAudioArg newArg;
        RtlZeroMemory(&newArg, sizeof(CsAudioArg));
        newArg.argSz = sizeof(CsAudioArg);
        newArg.endpointType = arg.endpointType;
        newArg.endpointRequest = CSAudioEndpointStop;
        ExNotifyCallback(this->m_CsAudioAPICallback, &newArg, &CsAudioArg2);
    }
}

static NTSTATUS SendIoctlToDeviceInterface(
    _In_ const GUID* InterfaceGuid,
    _In_ ULONG IoControlCode,
    _In_opt_ PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_opt_ PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength
)
{
    PWSTR symbolicLinkList = NULL;
    NTSTATUS status = IoGetDeviceInterfaces(
        InterfaceGuid,
        NULL,
        0,
        &symbolicLinkList
    );

    if (!NT_SUCCESS(status) || !symbolicLinkList || !symbolicLinkList[0]) {
        if (symbolicLinkList) {
            ExFreePool(symbolicLinkList);
        }
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    NTSTATUS overallStatus = STATUS_DEVICE_NOT_CONNECTED;
    for (PWSTR symLinkStr = symbolicLinkList; *symLinkStr; symLinkStr += wcslen(symLinkStr) + 1) {
        UNICODE_STRING symLink;
        RtlInitUnicodeString(&symLink, symLinkStr);

        PFILE_OBJECT fileObject = NULL;
        PDEVICE_OBJECT targetDevice = NULL;

        status = IoGetDeviceObjectPointer(
            &symLink,
            FILE_READ_DATA | FILE_WRITE_DATA,
            &fileObject,
            &targetDevice
        );

        if (NT_SUCCESS(status) && targetDevice) {
            IO_STATUS_BLOCK ioStatus;
            KEVENT event;
            KeInitializeEvent(&event, NotificationEvent, FALSE);

            PIRP irp = IoBuildDeviceIoControlRequest(
                IoControlCode,
                targetDevice,
                InputBuffer,
                InputBufferLength,
                OutputBuffer,
                OutputBufferLength,
                FALSE,
                &event,
                &ioStatus
            );

            if (irp) {
                status = IoCallDriver(targetDevice, irp);
                if (status == STATUS_PENDING) {
                    KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
                    status = ioStatus.Status;
                }
            } else {
                status = STATUS_INSUFFICIENT_RESOURCES;
            }

            ObDereferenceObject(fileObject);
            overallStatus = status;
        }
    }

    ExFreePool(symbolicLinkList);
    return overallStatus;
}

void CCsAudioSofHW::NotifySidecarCodec(CSAudioEndpointType endpoint, BOOL start)
{
    CSAUDIO_STREAM_CONFIG config;
    RtlZeroMemory(&config, sizeof(config));
    config.SampleRate = 48000;
    config.BitsPerSample = 16;
    config.Channels = 2;
    config.SlotWidth = 16;

    ULONG ioctl = start ? IOCTL_CSAUDIO_STREAM_START : IOCTL_CSAUDIO_STREAM_STOP;

    if (endpoint == CSAudioEndpointTypeSpeaker) {
        // Notify MAX98373 smart amplifier
        SendIoctlToDeviceInterface(
            &GUID_DEVINTERFACE_MAX98373_AMP,
            ioctl,
            &config,
            sizeof(config),
            NULL,
            0
        );
        // Also send generic CSAudio codec IOCTL
        SendIoctlToDeviceInterface(
            &GUID_DEVINTERFACE_CSAUDIO_CODEC,
            ioctl,
            &config,
            sizeof(config),
            NULL,
            0
        );
    } else if (endpoint == CSAudioEndpointTypeHeadphone || endpoint == CSAudioEndpointTypeMicJack) {
        // Notify NAU88L25 codec
        SendIoctlToDeviceInterface(
            &GUID_DEVINTERFACE_NAU8825_CODEC,
            ioctl,
            &config,
            sizeof(config),
            NULL,
            0
        );
        SendIoctlToDeviceInterface(
            &GUID_DEVINTERFACE_CSAUDIO_CODEC,
            ioctl,
            &config,
            sizeof(config),
            NULL,
            0
        );
    }
}

void CCsAudioSofHW::SendSidecarVolume(CSAudioEndpointType endpoint, LONG volumeLevel, BOOL muted)
{
    CSAUDIO_VOLUME_PARAMS params;
    RtlZeroMemory(&params, sizeof(params));
    params.Channel = 0;
    params.VolumeDbLevel = (UINT8)(volumeLevel > 0 ? volumeLevel : -volumeLevel);
    params.Muted = muted ? TRUE : FALSE;

    ULONG ioctl = muted ? IOCTL_CSAUDIO_SET_MUTE : IOCTL_CSAUDIO_SET_VOLUME;

    if (endpoint == CSAudioEndpointTypeSpeaker) {
        SendIoctlToDeviceInterface(&GUID_DEVINTERFACE_MAX98373_AMP, ioctl, &params, sizeof(params), NULL, 0);
        SendIoctlToDeviceInterface(&GUID_DEVINTERFACE_CSAUDIO_CODEC, ioctl, &params, sizeof(params), NULL, 0);
    } else if (endpoint == CSAudioEndpointTypeHeadphone) {
        SendIoctlToDeviceInterface(&GUID_DEVINTERFACE_NAU8825_CODEC, ioctl, &params, sizeof(params), NULL, 0);
        SendIoctlToDeviceInterface(&GUID_DEVINTERFACE_CSAUDIO_CODEC, ioctl, &params, sizeof(params), NULL, 0);
    }
}
