/*
 * Max98373Audio.c — Maxim MAX98373 Smart Amplifier KMDF Driver for Windows 11
 *
 * Implements:
 *   1. DriverEntry and EvtDriverDeviceAdd
 *   2. ACPI SPB (I2C) resource parsing and I2C connection management
 *   3. Hardware initialization, software reset, clock configuration, and gain staging
 *   4. Power transitions (EvtDeviceD0Entry / EvtDeviceD0Exit)
 *   5. CSAudio bus / Sidecar IOCTL dispatch (stream start/stop, volume control, reg I/O)
 *
 * Copyright (c) 2026. Licensed under the MIT License.
 */

#include "Max98373Audio.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, Max98373_EvtDeviceAdd)
#pragma alloc_text(PAGE, Max98373_EvtPrepareHardware)
#pragma alloc_text(PAGE, Max98373_EvtReleaseHardware)
#pragma alloc_text(PAGE, Max98373_EvtD0Entry)
#pragma alloc_text(PAGE, Max98373_EvtD0Exit)
#pragma alloc_text(PAGE, Max98373_InitializeHardware)
#pragma alloc_text(PAGE, Max98373_SetPowerState)
#pragma alloc_text(PAGE, Max98373_SetStreamState)
#pragma alloc_text(PAGE, Max98373_SetVolume)
#endif

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
    pnpCallbacks.EvtDevicePrepareHardware = Max98373_EvtPrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = Max98373_EvtReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry         = Max98373_EvtD0Entry;
    pnpCallbacks.EvtDeviceD0Exit          = Max98373_EvtD0Exit;
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
    devCtx->VolumeDb     = 0;                 /* 0 dB */
    devCtx->HardwareGain = MAX98373_SPK_GAIN_DEFAULT; /* +15.5 dB */
    devCtx->SampleRate   = 48000;             /* Standard 48 kHz */
    devCtx->SpeakerRole  = SPEAKER_ROLE_STEREO;

    /* Create Spinlock for hardware state synchronization */
    status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &devCtx->Lock);
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

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Max98373Audio: I2C target opened successfully\n"));

    return STATUS_SUCCESS;
}

/* =========================================================================
 * EvtReleaseHardware — Close I2C target
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

    if (devCtx->SpbTarget != NULL) {
        WdfIoTargetClose(devCtx->SpbTarget);
        WdfObjectDelete(devCtx->SpbTarget);
        devCtx->SpbTarget = NULL;
    }

    return STATUS_SUCCESS;
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

    UNREFERENCED_PARAMETER(PreviousState);
    PAGED_CODE();

    devCtx = GetMax98373Context(Device);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Max98373Audio: EvtD0Entry (powering up)\n"));

    devCtx->PowerActive = TRUE;

    status = Max98373_InitializeHardware(devCtx);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "Max98373Audio: Max98373_InitializeHardware returned 0x%08X\n", status));
    }

    return STATUS_SUCCESS;
}

/* =========================================================================
 * EvtD0Exit — Power Down & Pop Suppression
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
    devCtx->Streaming   = FALSE;

    /* Ramp down volume to avoid clicks/pops */
    Max98373_SetVolume(devCtx, MAX98373_AMP_VOL_MUTE, TRUE);

    /* Disable amplifier block */
    Max98373_SetStreamState(devCtx, FALSE);

    /* Put amplifier into global shutdown (ultra low power standby) */
    Max98373_SetPowerState(devCtx, FALSE);

    return STATUS_SUCCESS;
}

/* =========================================================================
 * Max98373_InitializeHardware — Soft Reset & Register Map Configuration
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Max98373_InitializeHardware(
    _In_ PMAX98373_DEVICE_CONTEXT DevCtx
    )
{
    NTSTATUS status;
    LARGE_INTEGER delay;
    UINT8 revId = 0;

    PAGED_CODE();

    if (!DevCtx->SpbTarget) return STATUS_INVALID_DEVICE_STATE;

    /* Step 1: Issue Software Reset */
    status = I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R2000_SW_RESET, MAX98373_SW_RESET_ASSERT);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "Max98373Audio: Failed to trigger software reset 0x%08X\n", status));
        return status;
    }

    /* Wait 10ms for internal calibration and reset completion */
    delay.QuadPart = -100000LL; /* 10ms */
    KeDelayExecutionThread(KernelMode, FALSE, &delay);

    /* Step 2: Read Revision ID to verify silicon presence */
    status = I2cReadRegister8(DevCtx->SpbTarget, MAX98373_REVID_REG, &revId);
    if (NT_SUCCESS(status)) {
        DevCtx->RevisionId = revId;
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
            "Max98373Audio: Detected MAX98373 silicon revision 0x%02X\n", revId));
    } else {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "Max98373Audio: Failed to read revision ID 0x%08X (continuing)\n", status));
    }

    /* Step 3: Pin Drive Strength & Thermal Configuration */
    I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R201E_PIN_DRIVE_STRENGTH, 0x55);
    I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R2014_THERM_WARN_THRESH, 0x10);
    I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R2015_THERM_SHDN_THRESH, 0x27);
    I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R2016_THERM_HYSTERESIS, 0x01);

    /* Step 4: Digital Audio Interface (DAI) - I2S 48kHz Stereo */
    /* PCM format: Standard I2S, 24/32-bit slot */
    I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R2024_PCM_DATA_FMT_CFG,
                      MAX98373_PCM_FMT_I2S | MAX98373_PCM_BCLKEDGE_RISING);

    /* Clock Ratio: 64x fs (e.g., 3.072 MHz BCLK for 48 kHz stereo) */
    I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R2026_PCM_CLOCK_RATIO, MAX98373_CLOCK_RATIO_64);

    /* Sample Rate: 48 kHz */
    I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R2027_PCM_SR_SETUP_1, MAX98373_PCM_SR_48KHZ);
    I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R2028_PCM_SR_SETUP_2, 0x88);

    /* Step 5: Speaker Path & Channel Mapping */
    /* Enable PCM Rx channel */
    I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R202B_PCM_RX_EN, 0x01);

    /* Speaker source: Mono Mix or Left/Right according to SpeakerRole */
    if (DevCtx->SpeakerRole == SPEAKER_ROLE_RIGHT) {
        I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R2029_PCM_TO_SPK_MONO_MIX_1, 0x01); /* Ch 1 (Right) */
    } else if (DevCtx->SpeakerRole == SPEAKER_ROLE_LEFT) {
        I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R2029_PCM_TO_SPK_MONO_MIX_1, 0x00); /* Ch 0 (Left) */
    } else {
        /* Stereo or Mono combined */
        I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R2029_PCM_TO_SPK_MONO_MIX_1, 0x00);
    }

    /* Step 6: Amplifier Path Gain & Digital Volume Control */
    I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R203E_AMP_PATH_GAIN, DevCtx->HardwareGain);
    I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R203D_AMP_DIG_VOL_CTRL, DevCtx->VolumeDb);

    /* Step 7: Limiter & Dynamic Headroom Protection */
    I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R20D1_DHT_CFG, 0x01);
    I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R20D4_DHT_EN, 0x01);

    /* Step 8: Auto restart configuration */
    I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R20FE_DEVICE_AUTO_RESTART_CFG, 0x01);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Max98373Audio: Hardware initialization complete\n"));

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

    return I2cWriteRegister8(
        DevCtx->SpbTarget,
        MAX98373_R20FF_GLOBAL_SHDN,
        PowerOn ? MAX98373_GLOBAL_SHDN_DIS : 0x00
    );
}

/* =========================================================================
 * Max98373_SetStreamState — Enable/Disable Speaker Power Stage
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Max98373_SetStreamState(
    _In_ PMAX98373_DEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN Start
    )
{
    NTSTATUS status;
    LARGE_INTEGER delay;

    PAGED_CODE();
    if (!DevCtx->SpbTarget) return STATUS_INVALID_DEVICE_STATE;

    if (Start) {
        /* Power on global chip first */
        Max98373_SetPowerState(DevCtx, TRUE);

        delay.QuadPart = -10000LL; /* 1ms delay for rail ramp */
        KeDelayExecutionThread(KernelMode, FALSE, &delay);

        /* Enable amplifier output stage */
        status = I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R2043_AMP_EN, MAX98373_AMP_ENABLE);
        DevCtx->Streaming = TRUE;

        /* Unmute to configured volume */
        Max98373_SetVolume(DevCtx, DevCtx->VolumeDb, FALSE);
    } else {
        /* Soft mute first to erase pop clicks */
        Max98373_SetVolume(DevCtx, MAX98373_AMP_VOL_MUTE, TRUE);

        delay.QuadPart = -10000LL; /* 1ms */
        KeDelayExecutionThread(KernelMode, FALSE, &delay);

        /* Disable amplifier output stage */
        status = I2cWriteRegister8(DevCtx->SpbTarget, MAX98373_R2043_AMP_EN, MAX98373_AMP_DISABLE);
        DevCtx->Streaming = FALSE;

        /* Put in global shutdown */
        Max98373_SetPowerState(DevCtx, FALSE);
    }

    return status;
}

/* =========================================================================
 * Max98373_SetVolume — Attenuation and Soft-Mute
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
    if (!DevCtx->SpbTarget) return STATUS_INVALID_DEVICE_STATE;

    DevCtx->VolumeDb = VolumeDb;
    DevCtx->Muted    = Mute;

    return I2cWriteRegister8(
        DevCtx->SpbTarget,
        MAX98373_R203D_AMP_DIG_VOL_CTRL,
        Mute ? MAX98373_AMP_VOL_MUTE : VolumeDb
    );
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
        status = Max98373_SetStreamState(devCtx, TRUE);
        break;
    }

    case IOCTL_CSAUDIO_STREAM_STOP: {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
            "Max98373Audio: IOCTL_CSAUDIO_STREAM_STOP received\n"));
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
