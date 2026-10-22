/*
 * Max98373Audio.c — Maxim MAX98373 Smart Amplifier KMDF Driver for Windows 11
 *
 * Implements:
 *   1. DriverEntry and EvtDriverDeviceAdd
 *   2. ACPI SPB (I2C) resource parsing and I2C connection management
 *   3. Hardware initialization matching CoolStar opengmaxcodec:
 *      - TDM Mode 0 digital audio interface format (0x58)
 *      - 128x fs clock ratio (0x06) for 6.144 MHz BCLK
 *      - Unattenuated digital volume (12 = 0x0C) and analog gain
 *      - TX Hi-Z masking to avoid playback bus contention on slots 0-3
 *      - Slot routing: Left to Slot 0 (0x00), Right to Slot 1 (0x40)
 *      - Global shutdown disable followed by amplifier enable
 *   4. Dual notification:
 *      - CsAudioCallbackAPI listener (\CallBack\CsAudioCallbackAPI)
 *      - IOCTL_CSAUDIO_STREAM_START / STOP sidecar bridge
 *   5. Power transitions (EvtDeviceD0Entry / EvtDeviceD0Exit)
 *
 * Copyright (c) 2026. Licensed under the MIT License.
 */

#include "Max98373Audio.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, Max98373_EvtDeviceAdd)
#pragma alloc_text(PAGE, Max98373_EvtPrepareHardware)
#pragma alloc_text(PAGE, Max98373_EvtReleaseHardware)
#pragma alloc_text(PAGE, Max98373_EvtSelfManagedIoInit)
#pragma alloc_text(PAGE, Max98373_EvtD0Entry)
#pragma alloc_text(PAGE, Max98373_EvtD0Exit)
#pragma alloc_text(PAGE, Max98373_InitializeHardware)
#pragma alloc_text(PAGE, Max98373_SetPowerState)
#pragma alloc_text(PAGE, Max98373_SetStreamState)
#pragma alloc_text(PAGE, Max98373_SetVolume)
#endif

static int CsAudioArg2 = 1;

static VOID AmpLog(_In_ PMAX98373_DEVICE_CONTEXT DevCtx, _In_ PCWSTR Name, _In_ ULONG Value);

/* =========================================================================
 * DriverEntry
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Max98373Audio: DriverEntry initializing\n"));

    WDF_DRIVER_CONFIG_INIT(&config, Max98373_EvtDeviceAdd);

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE
    );

    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "Max98373Audio: WdfDriverCreate failed 0x%08X\n", status));
    }

    return status;
}

/* =========================================================================
 * EvtDeviceAdd
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Max98373_EvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDFDEVICE device;
    PMAX98373_DEVICE_CONTEXT devCtx;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_IO_QUEUE_CONFIG queueConfig;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Max98373Audio: EvtDeviceAdd\n"));

    /* Register PnP and Power Callbacks */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware      = Max98373_EvtPrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware      = Max98373_EvtReleaseHardware;
    pnpCallbacks.EvtDeviceSelfManagedIoInit    = Max98373_EvtSelfManagedIoInit;
    pnpCallbacks.EvtDeviceD0Entry              = Max98373_EvtD0Entry;
    pnpCallbacks.EvtDeviceD0Exit               = Max98373_EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    /* Allocate Device Context */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, MAX98373_DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "Max98373Audio: WdfDeviceCreate failed 0x%08X\n", status));
        return status;
    }

    devCtx = GetMax98373Context(device);
    RtlZeroMemory(devCtx, sizeof(MAX98373_DEVICE_CONTEXT));
    devCtx->Device       = device;
    devCtx->VolumeDb     = 0;                 /* 0 dB unattenuated */
    devCtx->HardwareGain = 0x08;              /* reset default */
    devCtx->SampleRate   = 48000;             /* Standard 48 kHz */
    devCtx->SpeakerRole  = SPEAKER_ROLE_STEREO;

    /* Create Spinlock for hardware state synchronization */
    status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &devCtx->Lock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    {
        WDF_OBJECT_ATTRIBUTES lockAttr;
        WDF_OBJECT_ATTRIBUTES_INIT(&lockAttr);
        lockAttr.ParentObject = device;
        status = WdfWaitLockCreate(&lockAttr, &devCtx->StateLock);
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Configure default parallel I/O queue for IOCTLs */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = Max98373_EvtIoDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &devCtx->DefaultQueue);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "Max98373Audio: WdfIoQueueCreate failed 0x%08X\n", status));
        return status;
    }

    /* Expose Device Interfaces */
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_MAX98373_AMP, NULL);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "Max98373Audio: Failed to create MAX98373 amp interface 0x%08X\n", status));
    }

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_CSAUDIO_CODEC, NULL);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "Max98373Audio: Failed to create CSAudio codec interface 0x%08X\n", status));
    }

    return STATUS_SUCCESS;
}

/* =========================================================================
 * ACPI Helpers — Query _UID and _DSD Integer Properties
 * ========================================================================= */

static NTSTATUS
Max98373_GetDeviceUID(
    _In_ WDFDEVICE FxDevice,
    _Out_ PINT32 PUID
    )
{
    NTSTATUS status;
    ACPI_EVAL_INPUT_BUFFER_EX inputBuffer;
    RtlZeroMemory(&inputBuffer, sizeof(inputBuffer));

    inputBuffer.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE_EX;
    status = RtlStringCchPrintfA(
        inputBuffer.MethodName,
        sizeof(inputBuffer.MethodName),
        "_UID"
    );
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDFMEMORY outputMemory = WDF_NO_HANDLE;
    PACPI_EVAL_OUTPUT_BUFFER outputBuffer = NULL;
    size_t outputArgumentBufferSize = 32;
    size_t outputBufferSize = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) + outputArgumentBufferSize;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = FxDevice;

    status = WdfMemoryCreate(&attributes,
        NonPagedPoolNx,
        MAX98373_POOL_TAG,
        outputBufferSize,
        &outputMemory,
        (PVOID*)&outputBuffer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(outputBuffer, outputBufferSize);

    WDF_MEMORY_DESCRIPTOR inputMemDesc;
    WDF_MEMORY_DESCRIPTOR outputMemDesc;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputMemDesc, &inputBuffer, (ULONG)sizeof(inputBuffer));
    WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&outputMemDesc, outputMemory, NULL);

    status = WdfIoTargetSendInternalIoctlSynchronously(
        WdfDeviceGetIoTarget(FxDevice),
        NULL,
        IOCTL_ACPI_EVAL_METHOD_EX,
        &inputMemDesc,
        &outputMemDesc,
        NULL,
        NULL
    );
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    if (outputBuffer->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || outputBuffer->Count < 1) {
        status = STATUS_ACPI_INVALID_DATA;
        goto Exit;
    }

    UINT32 uid = 0;
    if (outputBuffer->Argument[0].DataLength >= 4) {
        uid = *(UINT32*)outputBuffer->Argument[0].Data;
    } else if (outputBuffer->Argument[0].DataLength >= 2) {
        uid = *(UINT16*)outputBuffer->Argument[0].Data;
    } else if (outputBuffer->Argument[0].DataLength >= 1) {
        uid = *(UINT8*)outputBuffer->Argument[0].Data;
    }

    if (PUID) {
        *PUID = (INT32)uid;
    }

Exit:
    if (outputMemory != WDF_NO_HANDLE) {
        WdfObjectDelete(outputMemory);
    }
    return status;
}

static NTSTATUS
Max98373_GetIntegerProperty(
    _In_ WDFDEVICE FxDevice,
    _In_ PCSTR PropertyStr,
    _Out_ UINT16* Property
    )
{
    WDFMEMORY outputMemory = WDF_NO_HANDLE;
    NTSTATUS status = STATUS_ACPI_NOT_INITIALIZED;
    size_t propLen = strlen(PropertyStr) + 1;
    size_t inputBufferLen = sizeof(ACPI_GET_DEVICE_SPECIFIC_DATA) + propLen;

    ACPI_GET_DEVICE_SPECIFIC_DATA* inputBuffer = (ACPI_GET_DEVICE_SPECIFIC_DATA*)ExAllocatePoolWithTag(
        NonPagedPoolNx,
        inputBufferLen,
        MAX98373_POOL_TAG
    );
    if (!inputBuffer) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(inputBuffer, inputBufferLen);

    inputBuffer->Signature = IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA_SIGNATURE;

    /* ACPI _DSD device properties UUID: daffd814-6eba-4d8c-8a91-bc9bbf4aa301 */
    unsigned char uuidend[] = { 0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01 };
    inputBuffer->Section.Data1 = 0xdaffd814;
    inputBuffer->Section.Data2 = 0x6eba;
    inputBuffer->Section.Data3 = 0x4d8c;
    memcpy(inputBuffer->Section.Data4, uuidend, sizeof(uuidend));

    RtlCopyMemory(inputBuffer->PropertyName, PropertyStr, propLen);
    inputBuffer->PropertyNameLength = (ULONG)propLen;

    PACPI_EVAL_OUTPUT_BUFFER outputBuffer = NULL;
    size_t outputArgumentBufferSize = 8;
    size_t outputBufferSize = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) + sizeof(ACPI_METHOD_ARGUMENT_V1) + outputArgumentBufferSize;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = FxDevice;

    status = WdfMemoryCreate(&attributes,
        NonPagedPoolNx,
        MAX98373_POOL_TAG,
        outputBufferSize,
        &outputMemory,
        (PVOID*)&outputBuffer
    );
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    WDF_MEMORY_DESCRIPTOR inputMemDesc;
    WDF_MEMORY_DESCRIPTOR outputMemDesc;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputMemDesc, inputBuffer, (ULONG)inputBufferLen);
    WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&outputMemDesc, outputMemory, NULL);

    status = WdfIoTargetSendInternalIoctlSynchronously(
        WdfDeviceGetIoTarget(FxDevice),
        NULL,
        IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA,
        &inputMemDesc,
        &outputMemDesc,
        NULL,
        NULL
    );
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    if (outputBuffer->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1 ||
        outputBuffer->Count < 1 ||
        outputBuffer->Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER ||
        outputBuffer->Argument[0].DataLength < 1) {
        status = STATUS_ACPI_INVALID_ARGUMENT;
        goto Exit;
    }

    if (Property) {
        *Property = (UINT16)(outputBuffer->Argument[0].Data[0] & 0xF);
    }

Exit:
    if (inputBuffer) {
        ExFreePoolWithTag(inputBuffer, MAX98373_POOL_TAG);
    }
    if (outputMemory != WDF_NO_HANDLE) {
        WdfObjectDelete(outputMemory);
    }
    return status;
}

/* =========================================================================
 * EvtPrepareHardware — Parse ACPI I2C Resource and open SPB target
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Max98373_EvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PMAX98373_DEVICE_CONTEXT devCtx;
    ULONG resourceCount;
    BOOLEAN foundI2c = FALSE;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    PAGED_CODE();

    devCtx = GetMax98373Context(Device);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Max98373Audio: EvtPrepareHardware\n"));

    resourceCount = WdfCmResourceListGetCount(ResourcesTranslated);

    for (ULONG i = 0; i < resourceCount; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc;
        desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        if (desc->Type == CmResourceTypeConnection &&
            desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
            desc->u.Connection.Type  == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {

            devCtx->I2cConnectionId.LowPart  = desc->u.Connection.IdLowPart;
            devCtx->I2cConnectionId.HighPart = desc->u.Connection.IdHighPart;
            foundI2c = TRUE;

            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                "Max98373Audio: Found I2C connection ID (0x%08X:0x%08X)\n",
                devCtx->I2cConnectionId.HighPart,
                devCtx->I2cConnectionId.LowPart));
            break;
        }
    }

    if (!foundI2c) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "Max98373Audio: No I2C resource found in ACPI table!\n"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* Open SPB I2C target via Resource Hub path */
    {
        DECLARE_UNICODE_STRING_SIZE(devicePath, RESOURCE_HUB_PATH_SIZE);
        WDF_OBJECT_ATTRIBUTES targetAttributes;
        WDF_IO_TARGET_OPEN_PARAMS openParams;

        status = RESOURCE_HUB_CREATE_PATH_FROM_ID(
            &devicePath,
            devCtx->I2cConnectionId.LowPart,
            devCtx->I2cConnectionId.HighPart
        );

        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                "Max98373Audio: RESOURCE_HUB_CREATE_PATH_FROM_ID failed 0x%08X\n", status));
            return status;
        }

        WDF_OBJECT_ATTRIBUTES_INIT(&targetAttributes);
        targetAttributes.ParentObject = Device;

        status = WdfIoTargetCreate(Device, &targetAttributes, &devCtx->SpbTarget);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                "Max98373Audio: WdfIoTargetCreate failed 0x%08X\n", status));
            return status;
        }

        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
            &openParams,
            &devicePath,
            (GENERIC_READ | GENERIC_WRITE)
        );
        openParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;

        status = WdfIoTargetOpen(devCtx->SpbTarget, &openParams);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                "Max98373Audio: WdfIoTargetOpen failed 0x%08X\n", status));
            return status;
        }
    }

    /* Query ACPI _UID method */
    INT32 uid = 0;
    if (NT_SUCCESS(Max98373_GetDeviceUID(Device, &uid))) {
        devCtx->UID = uid;
    } else {
        devCtx->UID = 0;
    }

    /* Detect Left vs Right speaker channel role from ACPI BiosDeviceName / Instance ID */
    {
        WDF_DEVICE_PROPERTY_DATA propertyData;
        WDFMEMORY mem = NULL;
        DEVPROPTYPE propType;

        WDF_DEVICE_PROPERTY_DATA_INIT(&propertyData, &DEVPKEY_Device_BiosDeviceName);
        if (NT_SUCCESS(WdfDeviceAllocAndQueryPropertyEx(
                Device,
                &propertyData,
                PagedPool,
                WDF_NO_OBJECT_ATTRIBUTES,
                &mem,
                &propType))) {
            PWSTR biosName = (PWSTR)WdfMemoryGetBuffer(mem, NULL);
            if (biosName) {
                if (wcsstr(biosName, L"MAXR")) {
                    devCtx->SpeakerRole = SPEAKER_ROLE_RIGHT;
                } else if (wcsstr(biosName, L"MAXL")) {
                    devCtx->SpeakerRole = SPEAKER_ROLE_LEFT;
                }
            }
            WdfObjectDelete(mem);
        }

        if (devCtx->SpeakerRole == SPEAKER_ROLE_STEREO) {
            WDF_DEVICE_PROPERTY_DATA_INIT(&propertyData, &DEVPKEY_Device_InstanceId);
            if (NT_SUCCESS(WdfDeviceAllocAndQueryPropertyEx(
                    Device,
                    &propertyData,
                    PagedPool,
                    WDF_NO_OBJECT_ATTRIBUTES,
                    &mem,
                    &propType))) {
                PWSTR instanceId = (PWSTR)WdfMemoryGetBuffer(mem, NULL);
                if (instanceId) {
                    if (wcsstr(instanceId, L"\\0")) {
                        devCtx->SpeakerRole = SPEAKER_ROLE_RIGHT;
                        devCtx->UID = 0;
                    } else if (wcsstr(instanceId, L"\\1")) {
                        devCtx->SpeakerRole = SPEAKER_ROLE_LEFT;
                        devCtx->UID = 1;
                    }
                }
                WdfObjectDelete(mem);
            }
        }
    }

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Max98373Audio: I2C target opened (UID=%d, SpeakerRole=%d)\n",
        devCtx->UID, devCtx->SpeakerRole));

    /* Registry log for this amp */
    RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, L"\\Registry\\Machine\\SOFTWARE\\CrosSofAudio");
    if (devCtx->SpeakerRole == SPEAKER_ROLE_LEFT) {
        RtlStringCchCopyW(devCtx->LogKey, ARRAYSIZE(devCtx->LogKey), L"\\Registry\\Machine\\SOFTWARE\\CrosSofAudio\\AmpLeft");
    } else if (devCtx->SpeakerRole == SPEAKER_ROLE_RIGHT) {
        RtlStringCchCopyW(devCtx->LogKey, ARRAYSIZE(devCtx->LogKey), L"\\Registry\\Machine\\SOFTWARE\\CrosSofAudio\\AmpRight");
    } else {
        RtlStringCchPrintfW(devCtx->LogKey, ARRAYSIZE(devCtx->LogKey), L"\\Registry\\Machine\\SOFTWARE\\CrosSofAudio\\Amp%d", devCtx->UID);
    }
    RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, devCtx->LogKey);
    AmpLog(devCtx, L"Uid", (ULONG)devCtx->UID);
    AmpLog(devCtx, L"Role", (ULONG)devCtx->SpeakerRole);   /* 1 left, 2 right, 3 unknown */

    return STATUS_SUCCESS;
}

/* =========================================================================
 * EvtReleaseHardware — Close I2C target and unregister callbacks
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Max98373_EvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PMAX98373_DEVICE_CONTEXT devCtx;

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    devCtx = GetMax98373Context(Device);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Max98373Audio: EvtReleaseHardware\n"));

    if (devCtx->CSAudioAPICallbackObj) {
        ExUnregisterCallback(devCtx->CSAudioAPICallbackObj);
        devCtx->CSAudioAPICallbackObj = NULL;
    }

    if (devCtx->CSAudioAPICallback) {
        ObfDereferenceObject(devCtx->CSAudioAPICallback);
        devCtx->CSAudioAPICallback = NULL;
    }

    if (devCtx->SpbTarget != NULL) {
        WdfIoTargetClose(devCtx->SpbTarget);
        WdfObjectDelete(devCtx->SpbTarget);
        devCtx->SpbTarget = NULL;
    }

    return STATUS_SUCCESS;
}

/* =========================================================================
 * EvtSelfManagedIoInit — Register CsAudioCallbackAPI listener
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Max98373_EvtSelfManagedIoInit(
    _In_ WDFDEVICE Device
    )
{
    PMAX98373_DEVICE_CONTEXT devCtx = GetMax98373Context(Device);
    NTSTATUS status;

    PAGED_CODE();

    UNICODE_STRING callbackName;
    RtlInitUnicodeString(&callbackName, L"\\CallBack\\CsAudioCallbackAPI");

    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(
        &attributes,
        &callbackName,
        OBJ_KERNEL_HANDLE | OBJ_OPENIF | OBJ_CASE_INSENSITIVE | OBJ_PERMANENT,
        NULL,
        NULL
    );

    status = ExCreateCallback(&devCtx->CSAudioAPICallback, &attributes, TRUE, TRUE);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "Max98373Audio: ExCreateCallback failed 0x%08X\n", status));
        return status;
    }

    devCtx->CSAudioAPICallbackObj = ExRegisterCallback(
        devCtx->CSAudioAPICallback,
        (PCALLBACK_FUNCTION)Max98373_CsAudioCallbackFunction,
        devCtx
    );

    if (!devCtx->CSAudioAPICallbackObj) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "Max98373Audio: ExRegisterCallback failed\n"));
        return STATUS_NO_CALLBACK_ACTIVE;
    }

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Max98373Audio: Registered CsAudioCallbackAPI listener\n"));

    return STATUS_SUCCESS;
}

/* =========================================================================
 * CsAudioCallbackFunction — Inter-driver audio stream notification
 * ========================================================================= */

VOID
Max98373_CsAudioCallbackFunction(
    _In_opt_ PVOID CallbackContext,
    _In_opt_ PVOID Argument1,
    _In_opt_ PVOID Argument2
    )
{
    PMAX98373_DEVICE_CONTEXT devCtx = (PMAX98373_DEVICE_CONTEXT)CallbackContext;
    CsAudioArg* arg = (CsAudioArg*)Argument1;

    if (!devCtx || !arg) {
        return;
    }

    if (Argument2 == &CsAudioArg2) {
        return;
    }

    CsAudioArg localArg;
    RtlZeroMemory(&localArg, sizeof(CsAudioArg));
    RtlCopyMemory(&localArg, arg, min(arg->argSz, sizeof(CsAudioArg)));

    if (localArg.endpointType != CSAudioEndpointTypeSpeaker) {
        return;
    }

    if (localArg.endpointRequest == CSAudioEndpointStop) {
        if (devCtx->CSAudioRequestsOn) {
            devCtx->CSAudioRequestsOn = FALSE;
            Max98373_SetStreamState(devCtx, FALSE);
        }
    }
    else if (localArg.endpointRequest == CSAudioEndpointStart) {
        if (!devCtx->CSAudioRequestsOn) {
            devCtx->CSAudioRequestsOn = TRUE;
            Max98373_SetStreamState(devCtx, TRUE);
        }
    }
}

/* =========================================================================
 * EvtD0Entry — Power Up & Hardware Initialization
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Max98373_EvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PMAX98373_DEVICE_CONTEXT devCtx;
    NTSTATUS status;
    UINT8 revId = 0;

    UNREFERENCED_PARAMETER(PreviousState);
    PAGED_CODE();

    devCtx = GetMax98373Context(Device);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Max98373Audio: EvtD0Entry (powering up)\n"));

    devCtx->PowerActive = TRUE;
    devCtx->Streaming = FALSE;

    /* Verify device presence by reading RevID */
    status = I2cReadRegister8(devCtx->SpbTarget, MAX98373_REVID_REG, &revId);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "Max98373Audio: Failed to read revision ID 0x%08X\n", status));
        devCtx->PowerActive = FALSE;
        return status;
    }
    devCtx->RevisionId = revId;
    AmpLog(devCtx, L"RevIdAtD0", revId);
    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Max98373Audio: Detected MAX98373 silicon revision 0x%02X (UID=%d, Role=%d)\n",
        revId, devCtx->UID, devCtx->SpeakerRole));

    /* Keep amplifier in standby until audio stream actually starts (when DSP clocks are active) */
    I2cWriteRegister8(devCtx->SpbTarget, MAX98373_R2043_AMP_EN, 0x00);
    I2cWriteRegister8(devCtx->SpbTarget, MAX98373_R20FF_GLOBAL_SHDN, 0x00);

    return STATUS_SUCCESS;
}

/* =========================================================================
 * EvtD0Exit — Power Down
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Max98373_EvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PMAX98373_DEVICE_CONTEXT devCtx;

    UNREFERENCED_PARAMETER(TargetState);
    PAGED_CODE();

    devCtx = GetMax98373Context(Device);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Max98373Audio: EvtD0Exit (powering down)\n"));

    devCtx->PowerActive = FALSE;
    Max98373_SetStreamState(devCtx, FALSE);

    return STATUS_SUCCESS;
}

/* =========================================================================
 * Logging: HKLM\SOFTWARE\CrosSofAudio\AmpLeft / AmpRight
 * ========================================================================= */

static VOID AmpLog(_In_ PMAX98373_DEVICE_CONTEXT DevCtx, _In_ PCWSTR Name, _In_ ULONG Value)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || DevCtx->LogKey[0] == 0) return;
    RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, DevCtx->LogKey, Name, REG_DWORD, &Value, sizeof(Value));
}

static VOID AmpLogReg(_In_ PMAX98373_DEVICE_CONTEXT DevCtx, _In_ PCWSTR Name, _In_ UINT16 Reg)
{
    UINT8 v = 0;
    if (NT_SUCCESS(I2cReadRegister8(DevCtx->SpbTarget, Reg, &v))) AmpLog(DevCtx, Name, v);
    else AmpLog(DevCtx, Name, 0xDEAD0000u | Reg);
}

static VOID AmpSnapshot(_In_ PMAX98373_DEVICE_CONTEXT DevCtx, _In_ BOOLEAN AtStop)
{
    if (!AtStop) {
        AmpLogReg(DevCtx, L"R2024_DATA_FMT",   MAX98373_R2024_PCM_DATA_FMT_CFG);
        AmpLogReg(DevCtx, L"R2026_CLK_RATIO",  MAX98373_R2026_PCM_CLOCK_RATIO);
        AmpLogReg(DevCtx, L"R2027_SR1",        MAX98373_R2027_PCM_SR_SETUP_1);
        AmpLogReg(DevCtx, L"R2028_SR2",        MAX98373_R2028_PCM_SR_SETUP_2);
        AmpLogReg(DevCtx, L"R2029_MONO_MIX1",  MAX98373_R2029_PCM_TO_SPK_MONO_MIX_1);
        AmpLogReg(DevCtx, L"R202A_MONO_MIX2",  MAX98373_R202A_PCM_TO_SPK_MONO_MIX_2);
        AmpLogReg(DevCtx, L"R202B_RX_EN",      MAX98373_R202B_PCM_RX_EN);
        AmpLogReg(DevCtx, L"R203D_DIG_VOL",    MAX98373_R203D_AMP_DIG_VOL_CTRL);
        AmpLogReg(DevCtx, L"R203E_PATH_GAIN",  MAX98373_R203E_AMP_PATH_GAIN);
        AmpLogReg(DevCtx, L"R2043_AMP_EN",     MAX98373_R2043_AMP_EN);
        AmpLogReg(DevCtx, L"R20FF_GLOBAL",     MAX98373_R20FF_GLOBAL_SHDN);
        AmpLogReg(DevCtx, L"Start_INT_RAW1",   MAX98373_R2001_INT_RAW1);
        AmpLogReg(DevCtx, L"Start_INT_RAW2",   MAX98373_R2002_INT_RAW2);
        AmpLogReg(DevCtx, L"Start_INT_RAW3",   MAX98373_R2003_INT_RAW3);
    } else {
        /* latched fault flags over the whole stream (clock, thermal, OVC...) */
        AmpLogReg(DevCtx, L"Stop_INT_RAW1",    MAX98373_R2001_INT_RAW1);
        AmpLogReg(DevCtx, L"Stop_INT_RAW2",    MAX98373_R2002_INT_RAW2);
        AmpLogReg(DevCtx, L"Stop_INT_RAW3",    MAX98373_R2003_INT_RAW3);
        AmpLogReg(DevCtx, L"Stop_INT_FLAG1",   MAX98373_R2007_INT_FLAG1);
        AmpLogReg(DevCtx, L"Stop_INT_FLAG2",   MAX98373_R2008_INT_FLAG2);
        AmpLogReg(DevCtx, L"Stop_INT_FLAG3",   MAX98373_R2009_INT_FLAG3);
        AmpLogReg(DevCtx, L"Stop_THERM_ADC",   MAX98373_R2055_MEAS_ADC_THERM_CH_READBACK);
        AmpLogReg(DevCtx, L"Stop_PVDD_ADC",    MAX98373_R2054_MEAS_ADC_PVDD_CH_READBACK);
    }
}

#define AMP_WR(reg, val) \
    do { status = I2cWriteRegister8(DevCtx->SpbTarget, (UINT16)(reg), (UINT8)(val)); \
         if (!NT_SUCCESS(status)) { AmpLog(DevCtx, L"LastFailedReg", (reg)); return status; } } while (0)
#define AMP_UPD(reg, mask, val) \
    do { status = I2cUpdateBits8(DevCtx->SpbTarget, (UINT16)(reg), (UINT8)(mask), (UINT8)(val)); \
         if (!NT_SUCCESS(status)) { AmpLog(DevCtx, L"LastFailedReg", (reg)); return status; } } while (0)

static VOID AmpSleepMs(ULONG ms)
{
    LARGE_INTEGER d;
    d.QuadPart = -(LONGLONG)ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &d);
}

/* =========================================================================
 * Max98373_InitializeHardware — Linux sequence for the volmar speaker link
 *
 *   max98373_reset() + max98373_probe()      (sound/soc/codecs/max98373.c)
 *   max98373_dai_set_fmt(DSP_B, NB_NF)       (max98373-i2c.c)
 *   max98373_dai_hw_params(S32_LE, 48 kHz)
 *   max98373_dai_tdm_slot(tx, rx 0x3, 8 slots, 32 bit)
 *                                            (sof_maxim_common.c: both amps
 *                                             read slots 0+1)
 *   "DAI Sel" Left / Right per amp, then DAPM "Amp Enable":
 *   PCM_RX_EN -> GLOBAL_EN -> 30 ms          (max98373_dac_event)
 *
 * Deliberate differences from ChromeOS:
 *   - I/V sense feedback is not used (no DSP speaker-protection module is
 *     loaded), so every TX slot stays Hi-Z and the I/V ADC stays off.
 *   - Amp digital volume: 0 dB (Linux default) unless
 *     HKLM\SOFTWARE\CrosSofAudio\SpeakerAmpAttenHalfDb (0.5 dB steps) says
 *     otherwise; the speaker path gain keeps its reset default. Windows' own
 *     volume slider scales the audio before it reaches the DSP.
 * ========================================================================= */

static ULONG AmpReadConfigDword(_In_ PCWSTR Name, _In_ ULONG Default)
{
    ULONG value = Default, def = Default;
    RTL_QUERY_REGISTRY_TABLE table[2];
    RtlZeroMemory(table, sizeof(table));
    table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    table[0].Name = (PWSTR)Name;
    table[0].EntryContext = &value;
    table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
    table[0].DefaultData = &def;
    table[0].DefaultLength = sizeof(def);
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE,
            L"\\Registry\\Machine\\SOFTWARE\\CrosSofAudio", table, NULL, NULL)))
        return Default;
    return value;
}

static UINT8 AmpDigVol(VOID)
{
    ULONG v = AmpReadConfigDword(L"SpeakerAmpAttenHalfDb", 0);
    return (UINT8)(v > 0x7E ? 0x7E : v);
}

_Use_decl_annotations_
NTSTATUS
Max98373_InitializeHardware(
    _In_ PMAX98373_DEVICE_CONTEXT DevCtx
    )
{
    NTSTATUS status;
    UINT8 rev = 0;
    UINT8 daiSel;

    PAGED_CODE();
    if (!DevCtx->SpbTarget) return STATUS_INVALID_DEVICE_STATE;

    /* max98373_reset() */
    AMP_WR(MAX98373_R2000_SW_RESET, 0x01);
    for (int i = 0; i < 3; i++) {
        AmpSleepMs(10);
        if (NT_SUCCESS(I2cReadRegister8(DevCtx->SpbTarget, MAX98373_REVID_REG, &rev))) break;
    }
    AmpLog(DevCtx, L"RevId", rev);

    /* max98373_probe() */
    AMP_WR(MAX98373_R2020_PCM_TX_HIZ_EN_1, 0xFF);
    AMP_WR(MAX98373_R2021_PCM_TX_HIZ_EN_2, 0xFF);
    AMP_WR(MAX98373_R2029_PCM_TO_SPK_MONO_MIX_1, 0x80);
    AMP_WR(MAX98373_R202A_PCM_TO_SPK_MONO_MIX_2, 0x01);
    AMP_WR(MAX98373_R203F_AMP_DSP_CFG, 0x03);                 /* DC blocker + dither */
    AMP_WR(MAX98373_R2046_IV_SENSE_ADC_DSP_CFG, 0x07);
    AMP_WR(MAX98373_R20FE_DEVICE_AUTO_RESTART_CFG, 0x0F);     /* OVC/THERM/CMON auto restart, clock monitor */
    AMP_UPD(MAX98373_R2043_AMP_EN, 0x01, 0x01);               /* speaker enable */

    /* set_fmt: DSP_B -> TDM mode 0, normal BCLK polarity */
    AMP_UPD(MAX98373_R2026_PCM_CLOCK_RATIO, 0x10, 0x00);
    AMP_UPD(MAX98373_R2024_PCM_DATA_FMT_CFG, 0x38, 0x03 << 3);

    /* hw_params: 32-bit, 48 kHz (also the I/V ADC rate) */
    AMP_UPD(MAX98373_R2024_PCM_DATA_FMT_CFG, 0xC0, 0xC0);
    AMP_UPD(MAX98373_R2027_PCM_SR_SETUP_1, 0x0F, 0x08);
    AMP_UPD(MAX98373_R2028_PCM_SR_SETUP_2, 0xF0, 0x08 << 4);
    AMP_UPD(MAX98373_R2028_PCM_SR_SETUP_2, 0x0F, 0x08);

    /* tdm_slot: 8 x 32 = 256 BCLK per frame -> BSEL 8; rx slots 0 and 1 */
    AMP_UPD(MAX98373_R2026_PCM_CLOCK_RATIO, 0x0F, 0x08);
    AMP_UPD(MAX98373_R2029_PCM_TO_SPK_MONO_MIX_1, 0x0F, 0x00);
    AMP_WR(MAX98373_R202A_PCM_TO_SPK_MONO_MIX_2, 0x01);
    AMP_WR(MAX98373_R2020_PCM_TX_HIZ_EN_1, 0xFF);             /* no I/V TX slots */
    AMP_WR(MAX98373_R2021_PCM_TX_HIZ_EN_2, 0xFF);

    /* "DAI Sel": Left amp plays slot 0, Right amp slot 1, unknown: (L+R)/2 */
    if (DevCtx->SpeakerRole == SPEAKER_ROLE_LEFT)       daiSel = 0;
    else if (DevCtx->SpeakerRole == SPEAKER_ROLE_RIGHT) daiSel = 1;
    else                                                daiSel = 2;
    AMP_UPD(MAX98373_R2029_PCM_TO_SPK_MONO_MIX_1, 0xC0, daiSel << 6);
    AmpLog(DevCtx, L"DaiSel", daiSel);

    /* amp digital volume (0 dB default) */
    DevCtx->VolumeDb = AmpDigVol();
    AmpLog(DevCtx, L"DigVolHalfDb", DevCtx->VolumeDb);
    AMP_WR(MAX98373_R203D_AMP_DIG_VOL_CTRL, DevCtx->VolumeDb);

    /* clear old fault flags, then DAPM "Amp Enable" */
    I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R200D_INT_FLAG_CLR1, 0xFF);
    I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R200E_INT_FLAG_CLR2, 0xFF);
    I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R200F_INT_FLAG_CLR3, 0xFF);
    AMP_UPD(MAX98373_R202B_PCM_RX_EN, 0x01, 0x01);
    AMP_UPD(MAX98373_R20FF_GLOBAL_SHDN, 0x01, 0x01);
    AmpSleepMs(30);

    AmpSnapshot(DevCtx, FALSE);
    return STATUS_SUCCESS;
}

/* =========================================================================
 * Max98373_SetPowerState — Global Shutdown Wake / Standby
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Max98373_SetPowerState(
    _In_ PMAX98373_DEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN PowerOn
    )
{
    PAGED_CODE();
    if (!DevCtx->SpbTarget) return STATUS_INVALID_DEVICE_STATE;
    return I2cUpdateBits8(DevCtx->SpbTarget, MAX98373_R20FF_GLOBAL_SHDN, 0x01, PowerOn ? 0x01 : 0x00);
}

/* =========================================================================
 * Max98373_SetStreamState — DAPM power up / down of the speaker path
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Max98373_SetStreamState(
    _In_ PMAX98373_DEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN Start
    )
{
    NTSTATUS status = STATUS_SUCCESS;

    PAGED_CODE();
    if (!DevCtx->SpbTarget) return STATUS_INVALID_DEVICE_STATE;

    WdfWaitLockAcquire(DevCtx->StateLock, NULL);
    if (Start) {
        if (!DevCtx->Streaming) {
            DevCtx->StartCount++;
            AmpLog(DevCtx, L"StartCount", DevCtx->StartCount);
            status = Max98373_InitializeHardware(DevCtx);
            AmpLog(DevCtx, L"StartStatus", (ULONG)status);
            if (NT_SUCCESS(status)) {
                DevCtx->Streaming = TRUE;
            } else {
                /* never leave a half-configured amp switched on */
                I2cUpdateBits8(DevCtx->SpbTarget, MAX98373_R20FF_GLOBAL_SHDN, 0x01, 0x00);
            }
        }
    } else if (DevCtx->Streaming) {
        AmpSnapshot(DevCtx, TRUE);
        /* max98373_dac_event(PRE_PMD): GLOBAL_EN off, 30 ms, then RX off */
        status = I2cUpdateBits8(DevCtx->SpbTarget, MAX98373_R20FF_GLOBAL_SHDN, 0x01, 0x00);
        AmpSleepMs(30);
        I2cUpdateBits8(DevCtx->SpbTarget, MAX98373_R202B_PCM_RX_EN, 0x01, 0x00);
        AmpLog(DevCtx, L"StopStatus", (ULONG)status);
        DevCtx->Streaming = FALSE;
    }
    WdfWaitLockRelease(DevCtx->StateLock);

    return status;
}

/* =========================================================================
 * Max98373_SetVolume — volume requests are ignored (fixed safe level, see
 * Max98373_InitializeHardware); mute only takes the amp out of the path.
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Max98373_SetVolume(
    _In_ PMAX98373_DEVICE_CONTEXT DevCtx,
    _In_ UINT8 VolumeDb,
    _In_ BOOLEAN Mute
    )
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(VolumeDb);
    if (!DevCtx->SpbTarget) return STATUS_INVALID_DEVICE_STATE;

    DevCtx->Muted = Mute;
    if (!DevCtx->Streaming) return STATUS_SUCCESS;
    return I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R203D_AMP_DIG_VOL_CTRL,
                             Mute ? 0x7F : DevCtx->VolumeDb);
}

/* =========================================================================
 * EvtIoDeviceControl — IOCTL Dispatch for Audio Bus / User Mode
 * ========================================================================= */

_Use_decl_annotations_
VOID
Max98373_EvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PMAX98373_DEVICE_CONTEXT devCtx = GetMax98373Context(device);
    NTSTATUS status = STATUS_SUCCESS;
    size_t bytesReturned = 0;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {

    case IOCTL_CSAUDIO_REGISTER_CODEC: {
        PCSAUDIO_REGISTER_PARAMS params;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(CSAUDIO_REGISTER_PARAMS), (PVOID*)&params, NULL);
        if (NT_SUCCESS(status)) {
            params->CodecType = CSAUDIO_CODEC_TYPE_SPEAKER;
            params->SupportedEndpoints = CSAUDIO_ENDPOINT_SPEAKER;
            RtlStringCbCopyA(params->DeviceName, sizeof(params->DeviceName), "Maxim MAX98373 Smart Amplifier");
            bytesReturned = sizeof(CSAUDIO_REGISTER_PARAMS);
        }
        break;
    }

    case IOCTL_CSAUDIO_STREAM_START: {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
            "Max98373Audio: IOCTL_CSAUDIO_STREAM_START received\n"));
        devCtx->CSAudioRequestsOn = TRUE;
        status = Max98373_SetStreamState(devCtx, TRUE);
        break;
    }

    case IOCTL_CSAUDIO_STREAM_STOP: {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
            "Max98373Audio: IOCTL_CSAUDIO_STREAM_STOP received\n"));
        devCtx->CSAudioRequestsOn = FALSE;
        status = Max98373_SetStreamState(devCtx, FALSE);
        break;
    }

    case IOCTL_CSAUDIO_SET_VOLUME: {
        PCSAUDIO_VOLUME_PARAMS volParams;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(CSAUDIO_VOLUME_PARAMS), (PVOID*)&volParams, NULL);
        if (NT_SUCCESS(status)) {
            status = Max98373_SetVolume(devCtx, volParams->VolumeDbLevel, volParams->Muted);
        }
        break;
    }

    case IOCTL_CSAUDIO_SET_MUTE: {
        PCSAUDIO_VOLUME_PARAMS volParams;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(CSAUDIO_VOLUME_PARAMS), (PVOID*)&volParams, NULL);
        if (NT_SUCCESS(status)) {
            status = Max98373_SetVolume(devCtx, volParams->VolumeDbLevel, TRUE);
        } else {
            status = Max98373_SetVolume(devCtx, devCtx->VolumeDb, TRUE);
        }
        break;
    }

    case IOCTL_CSAUDIO_READ_REG: {
        PCSAUDIO_REG_IO regIoIn;
        PCSAUDIO_REG_IO regIoOut;
        UINT8 val8 = 0;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(CSAUDIO_REG_IO), (PVOID*)&regIoIn, NULL);
        if (!NT_SUCCESS(status)) break;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(CSAUDIO_REG_IO), (PVOID*)&regIoOut, NULL);
        if (!NT_SUCCESS(status)) break;

        status = I2cReadRegister8(devCtx->SpbTarget, regIoIn->Register, &val8);
        if (NT_SUCCESS(status)) {
            regIoOut->Register = regIoIn->Register;
            regIoOut->Value = val8;
            bytesReturned = sizeof(CSAUDIO_REG_IO);
        }
        break;
    }

    case IOCTL_CSAUDIO_WRITE_REG: {
        PCSAUDIO_REG_IO regIo;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(CSAUDIO_REG_IO), (PVOID*)&regIo, NULL);
        if (NT_SUCCESS(status)) {
            status = I2cWriteRegister8(devCtx->SpbTarget, regIo->Register, (UINT8)(regIo->Value & 0xFF));
        }
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}
