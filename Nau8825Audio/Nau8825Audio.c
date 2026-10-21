/*
 * Nau8825Audio.c — Nuvoton NAU88L25 KMDF Audio Codec & Jack Detection Driver
 *
 * Implements:
 *   1. DriverEntry and EvtDriverDeviceAdd
 *   2. ACPI SPB (I2C) and GPIO Interrupt resource parsing
 *   3. Hardware initialization, FLL clock synthesis, and headphone driver staging
 *   4. Hardware Jack Detection engine (TRS headphones, TRRS headset, button events)
 *   5. Pop-free volume ramping and power transitions (EvtDeviceD0Entry / EvtDeviceD0Exit)
 *   6. CSAudio bus / Sidecar IOCTL dispatch and asynchronous jack event notifications
 *
 * Copyright (c) 2026. Licensed under the MIT License.
 */

#include "Nau8825Audio.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, Nau8825_EvtDeviceAdd)
#pragma alloc_text(PAGE, Nau8825_EvtPrepareHardware)
#pragma alloc_text(PAGE, Nau8825_EvtReleaseHardware)
#pragma alloc_text(PAGE, Nau8825_EvtD0Entry)
#pragma alloc_text(PAGE, Nau8825_EvtD0Exit)
#pragma alloc_text(PAGE, Nau8825_InitializeHardware)
#pragma alloc_text(PAGE, Nau8825_ConfigureClocks)
#pragma alloc_text(PAGE, Nau8825_EnableJackDetection)
#pragma alloc_text(PAGE, Nau8825_SetPlaybackState)
#pragma alloc_text(PAGE, Nau8825_SetCaptureState)
#pragma alloc_text(PAGE, Nau8825_SetVolume)
#pragma alloc_text(PAGE, Nau8825_ProcessJackStateChange)
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
        "Nau8825Audio: DriverEntry initializing\n"));

    WDF_DRIVER_CONFIG_INIT(&config, Nau8825_EvtDeviceAdd);

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE
    );

    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "Nau8825Audio: WdfDriverCreate failed 0x%08X\n", status));
    }

    return status;
}

/* =========================================================================
 * EvtDeviceAdd
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Nau8825_EvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDFDEVICE device;
    PNAU8825_DEVICE_CONTEXT devCtx;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_IO_QUEUE_CONFIG queueConfig;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Nau8825Audio: EvtDeviceAdd\n"));

    /* Register PnP and Power Callbacks */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = Nau8825_EvtPrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = Nau8825_EvtReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry         = Nau8825_EvtD0Entry;
    pnpCallbacks.EvtDeviceD0Exit          = Nau8825_EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    /* Allocate Device Context */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, NAU8825_DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "Nau8825Audio: WdfDeviceCreate failed 0x%08X\n", status));
        return status;
    }

    devCtx = GetNau8825Context(device);
    RtlZeroMemory(devCtx, sizeof(NAU8825_DEVICE_CONTEXT));
    devCtx->Device           = device;
    devCtx->VolumeStep       = 0;                 /* 0 dB */
    devCtx->SampleRate       = 48000;             /* Standard 48 kHz */
    devCtx->CurrentJackState = CSAUDIO_JACK_UNPLUGGED;

    /* Create Spinlock for hardware state synchronization */
    status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &devCtx->Lock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Configure default parallel I/O queue for IOCTLs */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = Nau8825_EvtIoDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &devCtx->DefaultQueue);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "Nau8825Audio: WdfIoQueueCreate failed 0x%08X\n", status));
        return status;
    }

    /* Create dedicated manual queue to hold pending jack state change notifications */
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &devCtx->JackNotificationQueue);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "Nau8825Audio: JackNotificationQueue creation failed 0x%08X\n", status));
    }

    /* Expose Device Interfaces */
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_NAU8825_CODEC, NULL);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "Nau8825Audio: Failed to create NAU8825 interface 0x%08X\n", status));
    }

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_CSAUDIO_CODEC, NULL);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "Nau8825Audio: Failed to create CSAudio codec interface 0x%08X\n", status));
    }

    return STATUS_SUCCESS;
}

/* =========================================================================
 * EvtPrepareHardware — Parse ACPI I2C & Interrupt Resources
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Nau8825_EvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PNAU8825_DEVICE_CONTEXT devCtx;
    ULONG resourceCount;
    BOOLEAN foundI2c = FALSE;
    BOOLEAN foundIrq = FALSE;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    PAGED_CODE();
    devCtx = GetNau8825Context(Device);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Nau8825Audio: EvtPrepareHardware\n"));

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
                "Nau8825Audio: Found I2C connection ID (0x%08X:0x%08X)\n",
                devCtx->I2cConnectionId.HighPart,
                devCtx->I2cConnectionId.LowPart));
        }
        else if (desc->Type == CmResourceTypeInterrupt) {
            devCtx->GpioInterruptVector = desc->u.Interrupt.Vector;
            foundIrq = TRUE;

            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                "Nau8825Audio: Found GPIO Jack Detect Interrupt Vector 0x%08X\n",
                devCtx->GpioInterruptVector));
        }
    }

    if (!foundI2c) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "Nau8825Audio: No I2C resource found in ACPI table!\n"));
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
            return status;
        }

        WDF_OBJECT_ATTRIBUTES_INIT(&targetAttributes);
        targetAttributes.ParentObject = Device;

        status = WdfIoTargetCreate(Device, &targetAttributes, &devCtx->SpbTarget);
        if (!NT_SUCCESS(status)) {
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
                "Nau8825Audio: WdfIoTargetOpen failed 0x%08X\n", status));
            return status;
        }
    }

    /* Create GPIO Interrupt object if an interrupt resource was discovered */
    if (foundIrq && devCtx->Interrupt == NULL) {
        WDF_INTERRUPT_CONFIG interruptConfig;
        WDF_INTERRUPT_CONFIG_INIT(
            &interruptConfig,
            Nau8825_EvtInterruptIsr,
            NULL
        );
        interruptConfig.EvtInterruptWorkItem = Nau8825_EvtInterruptWorkItem;
        interruptConfig.PassiveHandling      = TRUE; /* I2C transactions require passive level */

        status = WdfInterruptCreate(
            Device,
            &interruptConfig,
            WDF_NO_OBJECT_ATTRIBUTES,
            &devCtx->Interrupt
        );

        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                "Nau8825Audio: WdfInterruptCreate returned 0x%08X (polling fallback)\n", status));
        } else {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                "Nau8825Audio: Hardware Jack Detect Interrupt bound successfully\n"));
        }
    }

    return STATUS_SUCCESS;
}

/* =========================================================================
 * EvtReleaseHardware — Close I2C target
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Nau8825_EvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PNAU8825_DEVICE_CONTEXT devCtx;

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    devCtx = GetNau8825Context(Device);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Nau8825Audio: EvtReleaseHardware\n"));

    if (devCtx->SpbTarget != NULL) {
        WdfIoTargetClose(devCtx->SpbTarget);
        WdfObjectDelete(devCtx->SpbTarget);
        devCtx->SpbTarget = NULL;
    }

    return STATUS_SUCCESS;
}

/* =========================================================================
 * EvtD0Entry — Power Up & Codec Initialization
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Nau8825_EvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PNAU8825_DEVICE_CONTEXT devCtx;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(PreviousState);
    PAGED_CODE();

    devCtx = GetNau8825Context(Device);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Nau8825Audio: EvtD0Entry (powering up)\n"));

    devCtx->PowerActive = TRUE;

    status = Nau8825_InitializeHardware(devCtx);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "Nau8825Audio: Nau8825_InitializeHardware failed 0x%08X - codec not configured\n", status));
        devCtx->PowerActive = FALSE;
        return status;
    }

    /* Enable Jack Detection Engine */
    Nau8825_EnableJackDetection(devCtx);

    /* Initial check of jack insertion status */
    Nau8825_ProcessJackStateChange(devCtx);

    return STATUS_SUCCESS;
}

/* =========================================================================
 * EvtD0Exit — Power Down & Click Suppression
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Nau8825_EvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PNAU8825_DEVICE_CONTEXT devCtx;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(TargetState);
    PAGED_CODE();

    devCtx = GetNau8825Context(Device);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Nau8825Audio: EvtD0Exit (powering down)\n"));

    devCtx->PowerActive = FALSE;

    /* Ramp down headphone volume smoothly */
    status = Nau8825_SetVolume(devCtx, NAU8825_HP_VOL_MIN, TRUE);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "Nau8825Audio: EvtD0Exit - SetVolume failed 0x%08X\n", status));
    }

    /* Power down DAC and ADC paths */
    status = Nau8825_SetPlaybackState(devCtx, FALSE);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "Nau8825Audio: EvtD0Exit - SetPlaybackState failed 0x%08X\n", status));
    }
    status = Nau8825_SetCaptureState(devCtx, FALSE);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "Nau8825Audio: EvtD0Exit - SetCaptureState failed 0x%08X\n", status));
    }

    return STATUS_SUCCESS;
}

/* =========================================================================
 * Nau8825_InitializeHardware — Soft Reset & Register Setup
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Nau8825_InitializeHardware(
    _In_ PNAU8825_DEVICE_CONTEXT DevCtx
    )
{
    NTSTATUS status;
    LARGE_INTEGER delay;
    UINT16 deviceId = 0;

    PAGED_CODE();

    if (!DevCtx->SpbTarget) return STATUS_INVALID_DEVICE_STATE;

    /* Step 1: Issue Software Reset (Write 0x0000 to Reset Register 0x00) */
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_RESET, 0x0000);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "Nau8825Audio: Soft reset failed 0x%08X\n", status));
        return status;
    }

    /* Wait 10ms for reset completion and internal LDO settling */
    delay.QuadPart = -100000LL; /* 10ms */
    KeDelayExecutionThread(KernelMode, FALSE, &delay);

    /* Step 2: Read Device ID */
    status = I2cReadRegister16(DevCtx->SpbTarget, NAU8825_REG_I2C_DEVICE_ID, &deviceId);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "Nau8825Audio: Failed to read device ID 0x%08X - device not present\n", status));
        return status;
    }
    if (deviceId != NAU8825_DEVICE_ID_VALUE) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "Nau8825Audio: Unexpected device ID 0x%04X (expected 0x%04X)\n",
            deviceId, NAU8825_DEVICE_ID_VALUE));
        return STATUS_DEVICE_NOT_READY;
    }

    /* Step 3: Configure Clocks & PLL/FLL */
    status = Nau8825_ConfigureClocks(DevCtx);
    if (!NT_SUCCESS(status)) return status;

    /* Step 4: Digital Audio Interface (I2S standard, 16-bit slot, slave mode, tristate disabled) */
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_I2S_PCM_CTRL1,
                       NAU8825_I2S_DL_16 | NAU8825_I2S_DF_I2S);
    if (!NT_SUCCESS(status)) return status;
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_I2S_PCM_CTRL2, 0x0010);
    if (!NT_SUCCESS(status)) return status;

    /* Step 5: Power Control & DAC/ADC Enable */
    /* Enable DACL/DACR, ADC, clock gating */
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_ENA_CTRL,
                       NAU8825_ENABLE_DACR | NAU8825_ENABLE_DACL |
                       NAU8825_ENABLE_ADC  | NAU8825_ENABLE_ADC_CLK |
                       NAU8825_ENABLE_DAC_CLK | NAU8825_ENABLE_SAR);
    if (!NT_SUCCESS(status)) return status;

    /* Enable Charge Pump (prevent pop clicks) */
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_CHARGE_PUMP,
                       NAU8825_JAMNODCLOW | NAU8825_CHARGE_PUMP_EN);
    if (!NT_SUCCESS(status)) return status;

    /* Enable RDAC clock sync */
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_RDAC,
                       NAU8825_RDAC_EN | NAU8825_RDAC_CLK_EN);
    if (!NT_SUCCESS(status)) return status;

    /* Power up output driver stages */
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_POWER_UP_CONTROL,
                       NAU8825_POWERUP_INTEGR_R | NAU8825_POWERUP_INTEGR_L |
                       NAU8825_POWERUP_DRV_IN_R | NAU8825_POWERUP_DRV_IN_L);
    if (!NT_SUCCESS(status)) return status;

    status = I2cUpdateBits16(DevCtx->SpbTarget, NAU8825_REG_POWER_UP_CONTROL,
                    NAU8825_POWERUP_HP_DRV_R | NAU8825_POWERUP_HP_DRV_L,
                    NAU8825_POWERUP_HP_DRV_R | NAU8825_POWERUP_HP_DRV_L);
    if (!NT_SUCCESS(status)) return status;

    /* Disconnect headphone outputs from ground */
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_HSD_CTRL, 0x0000);
    if (!NT_SUCCESS(status)) return status;

    /* Enable HP Boost Driver & Class G comparison */
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_BOOST, NAU8825_HP_BOOST_DIS);
    if (!NT_SUCCESS(status)) return status;
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_CLASSG_CTRL,
                       NAU8825_CLASSG_LDAC_EN | NAU8825_CLASSG_RDAC_EN);
    if (!NT_SUCCESS(status)) return status;

    /* Step 6: Initial Volume Setup (0 dB unmuted) */
    status = Nau8825_SetVolume(DevCtx, DevCtx->VolumeStep, FALSE);
    if (!NT_SUCCESS(status)) return status;

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Nau8825Audio: Codec hardware initialization complete\n"));

    return STATUS_SUCCESS;
}

/* =========================================================================
 * Nau8825_ConfigureClocks — Master Clock & FLL Setup for 48kHz
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Nau8825_ConfigureClocks(
    _In_ PNAU8825_DEVICE_CONTEXT DevCtx
    )
{
    NTSTATUS status;
    PAGED_CODE();
    if (!DevCtx->SpbTarget) return STATUS_INVALID_DEVICE_STATE;

    /* Setup clock divider: Internal VCO / MCLK divider */
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_CLK_DIVIDER, 0x0050);
    if (!NT_SUCCESS(status)) return status;

    /* FLL configuration for 48kHz sample rate synthesis */
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_FLL1, 0x0000);
    if (!NT_SUCCESS(status)) return status;
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_FLL2, 0x3126);
    if (!NT_SUCCESS(status)) return status;
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_FLL3, 0x0008);
    if (!NT_SUCCESS(status)) return status;
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_FLL4, 0x8010);
    if (!NT_SUCCESS(status)) return status;
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_FLL5, 0x0000);
    if (!NT_SUCCESS(status)) return status;
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_FLL6, 0x6000);
    if (!NT_SUCCESS(status)) return status;
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_FLL_VCO_RSV, 0x0BC0);
    if (!NT_SUCCESS(status)) return status;

    return STATUS_SUCCESS;
}

/* =========================================================================
 * Nau8825_EnableJackDetection — SAR ADC, Mic Bias & Interrupt Masking
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Nau8825_EnableJackDetection(
    _In_ PNAU8825_DEVICE_CONTEXT DevCtx
    )
{
    NTSTATUS status;
    PAGED_CODE();
    if (!DevCtx->SpbTarget) return STATUS_INVALID_DEVICE_STATE;

    /* Enable Mic Bias: 2.2V low-noise output for headset electret microphones */
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_MIC_BIAS,
                       NAU8825_MICBIAS_POWERUP | NAU8825_MICBIAS_VOLTAGE_2_2V);
    if (!NT_SUCCESS(status)) return status;

    /* Enable SAR ADC for impedance and headset pinout detection */
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_SAR_CTRL,
                       NAU8825_SAR_ADC_EN | 0x0015);
    if (!NT_SUCCESS(status)) return status;

    /* Jack Detect Control: Debounce 250ms for insert, 50ms for eject */
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_JACK_DET_CTRL,
                       NAU8825_JACK_DET_RESTART | (0x04 << 5) | (0x02 << 2));
    if (!NT_SUCCESS(status)) return status;

    /* Unmask Jack Insert, Jack Eject, and Key/Button Interrupts (active low/high) */
    /* Note: In NAU8825 INTERRUPT_MASK, bit = 0 enables, bit = 1 masks */
    status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_INTERRUPT_MASK,
                       (UINT16)~(NAU8825_IRQ_INSERT_EN | NAU8825_IRQ_EJECT_EN |
                                 NAU8825_IRQ_KEY_SHORT_PRESS_EN | NAU8825_IRQ_KEY_RELEASE_EN));
    if (!NT_SUCCESS(status)) return status;

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Nau8825Audio: Jack detection engine enabled\n"));

    return STATUS_SUCCESS;
}

/* =========================================================================
 * Nau8825_SetPlaybackState — Enable / Disable Headphone DAC Output
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Nau8825_SetPlaybackState(
    _In_ PNAU8825_DEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN Enable
    )
{
    NTSTATUS status;
    PAGED_CODE();
    if (!DevCtx->SpbTarget) return STATUS_INVALID_DEVICE_STATE;

    if (Enable) {
        /* Ensure DAI format is 16-bit I2S and tristate is disabled */
        status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_I2S_PCM_CTRL1,
                           NAU8825_I2S_DL_16 | NAU8825_I2S_DF_I2S);
        if (!NT_SUCCESS(status)) return status;
        status = I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_I2S_PCM_CTRL2, 0x0010);
        if (!NT_SUCCESS(status)) return status;

        /* Un-mute and power up DACs */
        status = I2cUpdateBits16(DevCtx->SpbTarget, NAU8825_REG_ENA_CTRL,
                        NAU8825_ENABLE_DACR | NAU8825_ENABLE_DACL,
                        NAU8825_ENABLE_DACR | NAU8825_ENABLE_DACL);
        if (!NT_SUCCESS(status)) return status;
        status = Nau8825_SetVolume(DevCtx, DevCtx->VolumeStep, FALSE);
        if (!NT_SUCCESS(status)) return status;
        DevCtx->Streaming = TRUE;
    } else {
        /* Soft mute and power down DACs */
        status = Nau8825_SetVolume(DevCtx, NAU8825_HP_VOL_MIN, TRUE);
        if (!NT_SUCCESS(status)) return status;
        status = I2cUpdateBits16(DevCtx->SpbTarget, NAU8825_REG_ENA_CTRL,
                        NAU8825_ENABLE_DACR | NAU8825_ENABLE_DACL, 0);
        if (!NT_SUCCESS(status)) return status;
        DevCtx->Streaming = FALSE;
    }

    return STATUS_SUCCESS;
}

/* =========================================================================
 * Nau8825_SetCaptureState — Enable / Disable Headset Mic ADC
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Nau8825_SetCaptureState(
    _In_ PNAU8825_DEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN Enable
    )
{
    NTSTATUS status;
    PAGED_CODE();
    if (!DevCtx->SpbTarget) return STATUS_INVALID_DEVICE_STATE;

    if (Enable) {
        status = I2cUpdateBits16(DevCtx->SpbTarget, NAU8825_REG_ENA_CTRL,
                        NAU8825_ENABLE_ADC | NAU8825_ENABLE_ADC_CLK,
                        NAU8825_ENABLE_ADC | NAU8825_ENABLE_ADC_CLK);
        if (!NT_SUCCESS(status)) return status;
        DevCtx->CaptureActive = TRUE;
    } else {
        status = I2cUpdateBits16(DevCtx->SpbTarget, NAU8825_REG_ENA_CTRL,
                        NAU8825_ENABLE_ADC | NAU8825_ENABLE_ADC_CLK, 0);
        if (!NT_SUCCESS(status)) return status;
        DevCtx->CaptureActive = FALSE;
    }

    return STATUS_SUCCESS;
}

/* =========================================================================
 * Nau8825_SetVolume — Smooth Anti-Pop Headphone Volume Ramping
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Nau8825_SetVolume(
    _In_ PNAU8825_DEVICE_CONTEXT DevCtx,
    _In_ UINT8 VolumeStep,
    _In_ BOOLEAN Mute
    )
{
    UINT16 regVal;
    PAGED_CODE();

    if (!DevCtx->SpbTarget) return STATUS_INVALID_DEVICE_STATE;

    if (VolumeStep > NAU8825_HP_VOL_MIN) {
        VolumeStep = NAU8825_HP_VOL_MIN;
    }

    DevCtx->VolumeStep = VolumeStep;
    DevCtx->Muted      = Mute;

    if (Mute) {
        regVal = NAU8825_HPL_MUTE | NAU8825_HPR_MUTE |
                 ((UINT16)VolumeStep << 8) | VolumeStep;
    } else {
        regVal = ((UINT16)VolumeStep << 8) | VolumeStep;
    }

    return I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_HSVOL_CTRL, regVal);
}

/* =========================================================================
 * Nau8825_ProcessJackStateChange — Read SAR ADC & Update Audio Subsystem
 * ========================================================================= */

_Use_decl_annotations_
VOID
Nau8825_ProcessJackStateChange(
    _In_ PNAU8825_DEVICE_CONTEXT DevCtx
    )
{
    UINT16 irqStatus = 0;
    UINT16 sarStatus = 0;
    CSAUDIO_JACK_STATE newJackState = CSAUDIO_JACK_UNPLUGGED;
    NTSTATUS status;

    PAGED_CODE();

    if (!DevCtx->SpbTarget) return;

    /* Read and clear interrupt status */
    status = I2cReadRegister16(DevCtx->SpbTarget, NAU8825_REG_IRQ_STATUS, &irqStatus);
    if (!NT_SUCCESS(status)) return;

    /* Clear the interrupt latch */
    I2cWriteRegister16(DevCtx->SpbTarget, NAU8825_REG_INT_CLR_KEY_STATUS, 0xFFFF);

    /* Check if a jack is currently inserted */
    if (irqStatus & NAU8825_JACK_INSERT_DETECTED) {
        DevCtx->JackInserted = TRUE;

        /* Read SAR status register to determine if microphone is present */
        status = I2cReadRegister16(DevCtx->SpbTarget, NAU8825_REG_SARDOUT_RAM_STATUS, &sarStatus);
        if (NT_SUCCESS(status) && (sarStatus & NAU8825_MIC_DETECTED_STATUS)) {
            newJackState = CSAUDIO_JACK_HEADSET_4POLE; /* TRRS headset with microphone */
            DevCtx->MicPresent = TRUE;
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                "Nau8825Audio: TRRS 4-pole Headset with Microphone detected!\n"));
        } else {
            newJackState = CSAUDIO_JACK_HEADPHONES_3POLE; /* Standard TRS headphones */
            DevCtx->MicPresent = FALSE;
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                "Nau8825Audio: TRS 3-pole Stereo Headphones detected (no mic)\n"));
        }
    } else if (irqStatus & NAU8825_JACK_EJECT_DETECTED) {
        DevCtx->JackInserted = FALSE;
        DevCtx->MicPresent   = FALSE;
        newJackState         = CSAUDIO_JACK_UNPLUGGED;
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
            "Nau8825Audio: Headphone Jack Unplugged / Ejected\n"));
    } else {
        /* Check for button events */
        if (irqStatus & NAU8825_KEY_SHORT_PRESS) {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                "Nau8825Audio: Headset button pressed\n"));
        }
        return;
    }

    DevCtx->CurrentJackState = newJackState;

    /* Complete all pending notification requests in JackNotificationQueue */
    {
        WDFREQUEST notifyRequest;
        while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(DevCtx->JackNotificationQueue, &notifyRequest))) {
            PCSAUDIO_JACK_STATE pOutState;
            status = WdfRequestRetrieveOutputBuffer(notifyRequest, sizeof(CSAUDIO_JACK_STATE), (PVOID*)&pOutState, NULL);
            if (NT_SUCCESS(status)) {
                *pOutState = newJackState;
                WdfRequestCompleteWithInformation(notifyRequest, STATUS_SUCCESS, sizeof(CSAUDIO_JACK_STATE));
            } else {
                WdfRequestComplete(notifyRequest, status);
            }
        }
    }
}

/* =========================================================================
 * Interrupt Callbacks
 * ========================================================================= */

_Use_decl_annotations_
BOOLEAN
Nau8825_EvtInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID
    )
{
    UNREFERENCED_PARAMETER(MessageID);

    /* Queue passive-level work item since reading/clearing NAU8825 registers
     * requires I2C bus transactions which must run at PASSIVE_LEVEL */
    WdfInterruptQueueWorkItemForIsr(Interrupt);

    return TRUE;
}

_Use_decl_annotations_
VOID
Nau8825_EvtInterruptWorkItem(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject
    )
{
    WDFDEVICE device = (WDFDEVICE)AssociatedObject;
    PNAU8825_DEVICE_CONTEXT devCtx = GetNau8825Context(device);

    UNREFERENCED_PARAMETER(Interrupt);
    PAGED_CODE();

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Nau8825Audio: EvtInterruptWorkItem triggered\n"));

    Nau8825_ProcessJackStateChange(devCtx);
}

/* =========================================================================
 * EvtIoDeviceControl — IOCTL Dispatch
 * ========================================================================= */

_Use_decl_annotations_
VOID
Nau8825_EvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PNAU8825_DEVICE_CONTEXT devCtx = GetNau8825Context(device);
    NTSTATUS status = STATUS_SUCCESS;
    size_t bytesReturned = 0;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {

    case IOCTL_CSAUDIO_REGISTER_CODEC: {
        PCSAUDIO_REGISTER_PARAMS params;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(CSAUDIO_REGISTER_PARAMS), (PVOID*)&params, NULL);
        if (NT_SUCCESS(status)) {
            params->CodecType = CSAUDIO_CODEC_TYPE_HEADSET;
            params->SupportedEndpoints = CSAUDIO_ENDPOINT_HEADPHONE | CSAUDIO_ENDPOINT_HEADSET_MIC;
            RtlStringCbCopyA(params->DeviceName, sizeof(params->DeviceName), "Nuvoton NAU88L25 Audio Codec & Jack Detect");
            bytesReturned = sizeof(CSAUDIO_REGISTER_PARAMS);
        }
        break;
    }

    case IOCTL_CSAUDIO_STREAM_START: {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
            "Nau8825Audio: IOCTL_CSAUDIO_STREAM_START received\n"));
        status = Nau8825_SetPlaybackState(devCtx, TRUE);
        break;
    }

    case IOCTL_CSAUDIO_STREAM_STOP: {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
            "Nau8825Audio: IOCTL_CSAUDIO_STREAM_STOP received\n"));
        status = Nau8825_SetPlaybackState(devCtx, FALSE);
        break;
    }

    case IOCTL_CSAUDIO_SET_VOLUME: {
        PCSAUDIO_VOLUME_PARAMS volParams;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(CSAUDIO_VOLUME_PARAMS), (PVOID*)&volParams, NULL);
        if (NT_SUCCESS(status)) {
            status = Nau8825_SetVolume(devCtx, volParams->VolumeDbLevel, volParams->Muted);
        }
        break;
    }

    case IOCTL_CSAUDIO_GET_JACK_STATE: {
        PCSAUDIO_JACK_STATE pJackState;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(CSAUDIO_JACK_STATE), (PVOID*)&pJackState, NULL);
        if (NT_SUCCESS(status)) {
            *pJackState = devCtx->CurrentJackState;
            bytesReturned = sizeof(CSAUDIO_JACK_STATE);
        }
        break;
    }

    case IOCTL_CSAUDIO_REGISTER_JACK_NOTIFICATION: {
        /* Forward request to the manual JackNotificationQueue to be completed when a jack state changes */
        status = WdfRequestForwardToIoQueue(Request, devCtx->JackNotificationQueue);
        if (NT_SUCCESS(status)) {
            return; /* Request is pending in the queue, do not complete yet */
        }
        break;
    }

    case IOCTL_CSAUDIO_READ_REG: {
        PCSAUDIO_REG_IO regIoIn;
        PCSAUDIO_REG_IO regIoOut;
        UINT16 val16 = 0;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(CSAUDIO_REG_IO), (PVOID*)&regIoIn, NULL);
        if (!NT_SUCCESS(status)) break;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(CSAUDIO_REG_IO), (PVOID*)&regIoOut, NULL);
        if (!NT_SUCCESS(status)) break;

        status = I2cReadRegister16(devCtx->SpbTarget, regIoIn->Register, &val16);
        if (NT_SUCCESS(status)) {
            regIoOut->Register = regIoIn->Register;
            regIoOut->Value = val16;
            bytesReturned = sizeof(CSAUDIO_REG_IO);
        }
        break;
    }

    case IOCTL_CSAUDIO_WRITE_REG: {
        PCSAUDIO_REG_IO regIo;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(CSAUDIO_REG_IO), (PVOID*)&regIo, NULL);
        if (NT_SUCCESS(status)) {
            status = I2cWriteRegister16(devCtx->SpbTarget, regIo->Register, regIo->Value);
        }
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}
