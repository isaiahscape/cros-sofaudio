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

static ULONG NauReadConfigDword(_In_ PCWSTR Name, _In_ ULONG Default);

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

    /* Jack polling timer (passive level: it does I2C reads) */
    {
        WDF_OBJECT_ATTRIBUTES attr;
        WDF_TIMER_CONFIG timerConfig;

        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = device;
        status = WdfWaitLockCreate(&attr, &devCtx->JackLock);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        /* one-shot, re-armed by the callback (passive-level timers are kept non-periodic) */
        WDF_TIMER_CONFIG_INIT(&timerConfig, Nau8825_EvtJackTimer);
        timerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = device;
        attr.ExecutionLevel = WdfExecutionLevelPassive;
        status = WdfTimerCreate(&timerConfig, &attr, &devCtx->JackTimer);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                "Nau8825Audio: jack timer creation failed 0x%08X\n", status));
            devCtx->JackTimer = NULL;
        }
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

    PAGED_CODE();
    devCtx = GetNau8825Context(Device);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "Nau8825Audio: EvtPrepareHardware\n"));

    /* Step 1: Scan ResourcesTranslated */
    if (ResourcesTranslated != NULL) {
        resourceCount = WdfCmResourceListGetCount(ResourcesTranslated);
        for (ULONG i = 0; i < resourceCount; i++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
            if (!desc) continue;

            if (desc->Type == CmResourceTypeConnection) {
                devCtx->I2cConnectionId.LowPart  = desc->u.Connection.IdLowPart;
                devCtx->I2cConnectionId.HighPart = desc->u.Connection.IdHighPart;
                foundI2c = TRUE;
                KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                    "Nau8825Audio: Found I2C connection ID in translated list (0x%08X:0x%08X)\n",
                    devCtx->I2cConnectionId.HighPart,
                    devCtx->I2cConnectionId.LowPart));
            } else if (desc->Type == CmResourceTypeInterrupt) {
                devCtx->GpioInterruptVector = desc->u.Interrupt.Vector;
                foundIrq = TRUE;
                KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                    "Nau8825Audio: Found GPIO Jack Detect Interrupt Vector 0x%08X\n",
                    devCtx->GpioInterruptVector));
            }
        }
    }

    /* Step 2: Fallback scan ResourcesRaw if I2C connection not found */
    if (!foundI2c && ResourcesRaw != NULL) {
        resourceCount = WdfCmResourceListGetCount(ResourcesRaw);
        for (ULONG i = 0; i < resourceCount; i++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR desc = WdfCmResourceListGetDescriptor(ResourcesRaw, i);
            if (!desc) continue;

            if (desc->Type == CmResourceTypeConnection) {
                devCtx->I2cConnectionId.LowPart  = desc->u.Connection.IdLowPart;
                devCtx->I2cConnectionId.HighPart = desc->u.Connection.IdHighPart;
                foundI2c = TRUE;
                KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                    "Nau8825Audio: Found I2C connection ID in raw list (0x%08X:0x%08X)\n",
                    devCtx->I2cConnectionId.HighPart,
                    devCtx->I2cConnectionId.LowPart));
                break;
            }
        }
    }

    if (!foundI2c) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "Nau8825Audio: No I2C connection resource discovered (device idle)\n"));
        return STATUS_SUCCESS;
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
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                "Nau8825Audio: RESOURCE_HUB_CREATE_PATH_FROM_ID returned 0x%08X\n", status));
            return STATUS_SUCCESS;
        }

        WDF_OBJECT_ATTRIBUTES_INIT(&targetAttributes);
        targetAttributes.ParentObject = Device;

        status = WdfIoTargetCreate(Device, &targetAttributes, &devCtx->SpbTarget);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                "Nau8825Audio: WdfIoTargetCreate returned 0x%08X\n", status));
            return STATUS_SUCCESS;
        }

        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
            &openParams,
            &devicePath,
            (GENERIC_READ | GENERIC_WRITE)
        );
        openParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;

        status = WdfIoTargetOpen(devCtx->SpbTarget, &openParams);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                "Nau8825Audio: WdfIoTargetOpen returned 0x%08X (deferred)\n", status));
            return STATUS_SUCCESS;
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

    if (!devCtx->SpbTarget) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
            "Nau8825Audio: EvtD0Entry - No I2C target, skipping hardware initialization\n"));
        return STATUS_SUCCESS;
    }

    status = Nau8825_InitializeHardware(devCtx);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "Nau8825Audio: Nau8825_InitializeHardware returned 0x%08X (deferred initialization)\n", status));
        return STATUS_SUCCESS;
    }

    /* Jack detection: polled (the old interrupt set-up used guessed register
     * bits and is no longer enabled). First reading is applied immediately. */
    devCtx->JackActiveLow = NauReadConfigDword(L"JackActiveLow", 0) ? TRUE : FALSE;
    devCtx->JackKnown = FALSE;
    devCtx->JackCandidate = 0xFF;
    Nau8825_ProcessJackStateChange(devCtx);
    devCtx->JackPollStop = FALSE;
    if (devCtx->JackTimer) {
        WdfTimerStart(devCtx->JackTimer, WDF_REL_TIMEOUT_IN_MS(NAU_JACK_POLL_MS));
    }

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

    devCtx->JackPollStop = TRUE;
    if (devCtx->JackTimer) {
        WdfTimerStop(devCtx->JackTimer, TRUE);
        WdfTimerStop(devCtx->JackTimer, TRUE);   /* in case the callback re-armed meanwhile */
    }
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
 * Codec register log: HKLM\SOFTWARE\CrosSofAudio\Codec
 * ========================================================================= */

#define NAUL_REG_ROOT  L"\\Registry\\Machine\\SOFTWARE\\CrosSofAudio"
#define NAUL_REG_LOG   L"\\Registry\\Machine\\SOFTWARE\\CrosSofAudio\\Codec"

static VOID NauLog(_In_ PCWSTR Name, _In_ ULONG Value)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, NAUL_REG_ROOT);
    RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, NAUL_REG_LOG);
    RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, NAUL_REG_LOG, Name, REG_DWORD, &Value, sizeof(Value));
}

static VOID NauLogReg(_In_ PNAU8825_DEVICE_CONTEXT DevCtx, _In_ PCWSTR Name, _In_ UINT16 Reg)
{
    UINT16 v = 0;
    if (NT_SUCCESS(I2cReadRegister16(DevCtx->SpbTarget, Reg, &v))) {
        NauLog(Name, v);
    } else {
        NauLog(Name, 0xDEAD0000u | Reg);
    }
}

static VOID NauSnapshot(_In_ PNAU8825_DEVICE_CONTEXT DevCtx)
{
    NauLogReg(DevCtx, L"R01_ENA_CTRL",      NAUL_ENA_CTRL);
    NauLogReg(DevCtx, L"R03_CLK_DIVIDER",   NAUL_CLK_DIVIDER);
    NauLogReg(DevCtx, L"R04_FLL1",          NAUL_FLL1);
    NauLogReg(DevCtx, L"R05_FLL2",          NAUL_FLL2);
    NauLogReg(DevCtx, L"R06_FLL3",          NAUL_FLL3);
    NauLogReg(DevCtx, L"R07_FLL4",          NAUL_FLL4);
    NauLogReg(DevCtx, L"R08_FLL5",          NAUL_FLL5);
    NauLogReg(DevCtx, L"R09_FLL6",          NAUL_FLL6);
    NauLogReg(DevCtx, L"R0C_HSD_CTRL",      NAUL_HSD_CTRL);
    NauLogReg(DevCtx, L"R0D_JACK_DET_CTRL", NAUL_JACK_DET_CTRL);
    NauLogReg(DevCtx, L"R1C_I2S_CTRL1",     NAUL_I2S_PCM_CTRL1);
    NauLogReg(DevCtx, L"R1D_I2S_CTRL2",     NAUL_I2S_PCM_CTRL2);
    NauLogReg(DevCtx, L"R2C_DAC_CTRL1",     NAUL_DAC_CTRL1);
    NauLogReg(DevCtx, L"R31_MUTE_CTRL",     NAUL_MUTE_CTRL);
    NauLogReg(DevCtx, L"R32_HSVOL_CTRL",    NAUL_HSVOL_CTRL);
    NauLogReg(DevCtx, L"R33_DACL_CTRL",     NAUL_DACL_CTRL);
    NauLogReg(DevCtx, L"R34_DACR_CTRL",     NAUL_DACR_CTRL);
    NauLogReg(DevCtx, L"R50_CLASSG_CTRL",   NAUL_CLASSG_CTRL);
    NauLogReg(DevCtx, L"R58_DEVICE_ID",     NAUL_I2C_DEVICE_ID);
    NauLogReg(DevCtx, L"R66_BIAS_ADJ",      NAUL_BIAS_ADJ);
    NauLogReg(DevCtx, L"R73_RDAC",          NAUL_RDAC);
    NauLogReg(DevCtx, L"R76_BOOST",         NAUL_BOOST);
    NauLogReg(DevCtx, L"R7F_POWER_UP",      NAUL_POWER_UP_CONTROL);
    NauLogReg(DevCtx, L"R80_CHARGE_PUMP",   NAUL_CHARGE_PUMP);
}

static VOID NauSleepMs(_In_ ULONG Ms)
{
    LARGE_INTEGER d;
    d.QuadPart = -10000LL * (LONGLONG)Ms;
    KeDelayExecutionThread(KernelMode, FALSE, &d);
}

#define NAU_UPD(reg, mask, val) \
    do { status = I2cUpdateBits16(DevCtx->SpbTarget, (UINT16)(reg), (UINT16)(mask), (UINT16)(val)); \
         if (!NT_SUCCESS(status)) { NauLog(L"LastFailedReg", (reg)); return status; } } while (0)
#define NAU_WR(reg, val) \
    do { status = I2cWriteRegister16(DevCtx->SpbTarget, (UINT16)(reg), (UINT16)(val)); \
         if (!NT_SUCCESS(status)) { NauLog(L"LastFailedReg", (reg)); return status; } } while (0)

/*
 * Jack pin level: I2C_DEVICE_ID.GPIO2JD1 (what Linux nau8825_is_jack_inserted()
 * reads). On volmar the pin reads HIGH with a plug inserted (seen in the
 * 18.2.0.3 logs: 0x1a14 at boot without plug, 0x1a94 while playing to
 * headphones), so "high = inserted" is the default; the registry value
 * HKLM\SOFTWARE\CrosSofAudio\JackActiveLow = 1 flips it for boards
 * wired the other way.
 */
static BOOLEAN NauJackRead(_In_ PNAU8825_DEVICE_CONTEXT DevCtx, _Out_ BOOLEAN* Inserted, _Out_opt_ UINT16* Raw)
{
    UINT16 id = 0;
    *Inserted = FALSE;
    if (!DevCtx->SpbTarget) return FALSE;
    if (!NT_SUCCESS(I2cReadRegister16(DevCtx->SpbTarget, NAUL_I2C_DEVICE_ID, &id))) return FALSE;
    if (Raw) *Raw = id;
    BOOLEAN high = (id & NAUL_GPIO2JD1) ? TRUE : FALSE;
    *Inserted = DevCtx->JackActiveLow ? !high : high;
    return TRUE;
}

static BOOLEAN NauJackInserted(_In_ PNAU8825_DEVICE_CONTEXT DevCtx)
{
    BOOLEAN ins = FALSE;
    (void)NauJackRead(DevCtx, &ins, NULL);
    return ins;
}

static ULONG NauReadConfigDword(_In_ PCWSTR Name, _In_ ULONG Default)
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

/* nau8825_configure_sysclk(NAU8825_CLK_DIS / NAU8825_CLK_INTERNAL) as done
 * by system_clock_control() when a stream ends. */
static NTSTATUS NauSysclkIdle(_In_ PNAU8825_DEVICE_CONTEXT DevCtx)
{
    NTSTATUS status;
    if (NauJackInserted(DevCtx)) {
        /* CLK_INTERNAL: free-running DCO keeps jack/button detection alive */
        NAU_UPD(NAUL_FLL6, NAUL_DCO_EN, NAUL_DCO_EN);
        NAU_UPD(NAUL_CLK_DIVIDER, NAUL_CLK_SRC_MASK, NAUL_CLK_SRC_VCO);
        NAU_UPD(NAUL_CLK_DIVIDER, NAUL_CLK_MCLK_SRC_MASK, 0xF);
        NAU_UPD(NAUL_FLL1, NAUL_ICTRL_LATCH_MASK | NAUL_FLL_RATIO_MASK, 0x10);
        NAU_UPD(NAUL_FLL6, NAUL_SDM_EN, NAUL_SDM_EN);
    } else {
        /* CLK_DIS: nau8825_configure_mclk_as_sysclk() */
        NAU_UPD(NAUL_CLK_DIVIDER, NAUL_CLK_SRC_MASK, NAUL_CLK_SRC_MCLK);
        NAU_UPD(NAUL_FLL6, NAUL_DCO_EN, 0);
        NAU_UPD(NAUL_FLL1, NAUL_ICTRL_LATCH_MASK, 0);
    }
    return STATUS_SUCCESS;
}

/* nau8825_init_regs() with the volmar coreboot properties. */
static NTSTATUS NauInitRegs(_In_ PNAU8825_DEVICE_CONTEXT DevCtx)
{
    NTSTATUS status;

    NAU_WR(NAUL_IIC_ADDR_SET, 0x0001);                         /* latch IIC LSB */
    NAU_UPD(NAUL_BIAS_ADJ, NAUL_BIAS_VMID, NAUL_BIAS_VMID);   /* bias / VMID */
    NAU_UPD(NAUL_BOOST, NAUL_GLOBAL_BIAS_EN, NAUL_GLOBAL_BIAS_EN);
    NAU_UPD(NAUL_BIAS_ADJ, NAUL_BIAS_VMID_SEL_MASK, VOLMAR_VREF_IMPEDANCE << 4);
    /* boost driver off, automatic short-circuit protection on */
    NAU_UPD(NAUL_BOOST,
        NAUL_PRECHARGE_DIS | NAUL_HP_BOOST_DIS | NAUL_HP_BOOST_G_DIS | NAUL_SHORT_SHUTDOWN_EN,
        NAUL_PRECHARGE_DIS | NAUL_HP_BOOST_DIS | NAUL_HP_BOOST_G_DIS | NAUL_SHORT_SHUTDOWN_EN);

    /* jack detect pin: input, internal pull disabled, active low */
    NAU_UPD(NAUL_GPIO12_CTRL, NAUL_JKDET_OUTPUT_EN, 0);
    NAU_UPD(NAUL_GPIO12_CTRL, NAUL_JKDET_PULL_EN, NAUL_JKDET_PULL_EN);
    NAU_UPD(NAUL_GPIO12_CTRL, NAUL_JKDET_PULL_UP, 0);
    NAU_UPD(NAUL_JACK_DET_CTRL, NAUL_JACK_POLARITY, 0);
    NAU_UPD(NAUL_JACK_DET_CTRL, NAUL_JACK_INSERT_DEB_MASK, VOLMAR_JACK_INSERT_DEB << 5);
    NAU_UPD(NAUL_JACK_DET_CTRL, NAUL_JACK_EJECT_DEB_MASK, VOLMAR_JACK_EJECT_DEB << 2);

    NAU_UPD(NAUL_INTERRUPT_MASK, NAUL_IRQ_PIN_PULLUP | NAUL_IRQ_PIN_PULL_EN,
            NAUL_IRQ_PIN_PULLUP | NAUL_IRQ_PIN_PULL_EN);
    NAU_UPD(NAUL_INTERRUPT_MASK, 0x7FF, 0x7FF);   /* jack detection unmasks what it needs */

    NAU_UPD(NAUL_MIC_BIAS, NAUL_MICBIAS_VOLTAGE_MASK, VOLMAR_MICBIAS_VOLTAGE);

    NAU_UPD(NAUL_ADC_RATE, NAUL_ADC_SYNC_DOWN_MASK | NAUL_ADC_SINC4_EN, NAUL_ADC_SYNC_DOWN_64);
    NAU_UPD(NAUL_DAC_CTRL1, NAUL_DAC_OVERSAMPLE_MASK, NAUL_DAC_OVERSAMPLE_64);
    if (DevCtx->SwId == NAUL_SW_ID_NAU8825) {
        NAU_UPD(NAUL_CHARGE_PUMP, NAUL_POWER_DOWN_DACR | NAUL_POWER_DOWN_DACL,
                NAUL_POWER_DOWN_DACR | NAUL_POWER_DOWN_DACL);
    }
    NAU_UPD(NAUL_BIAS_ADJ, NAUL_BIAS_TESTDAC_EN, NAUL_BIAS_TESTDAC_EN);   /* DAC inputs held at 0 */
    NAU_UPD(NAUL_DAC_CTRL1, NAUL_DAC_CLIP_OFF, NAUL_DAC_CLIP_OFF);
    NAU_UPD(NAUL_ANALOG_CONTROL_2,
        NAUL_HP_NON_CLASSG_2X | NAUL_DAC_CAP_MSB | NAUL_DAC_CAP_LSB,
        NAUL_HP_NON_CLASSG_2X | NAUL_DAC_CAP_MSB | NAUL_DAC_CAP_LSB);
    NAU_UPD(NAUL_CLASSG_CTRL, NAUL_CLASSG_TIMER_MASK, NAUL_CLASSG_TIMER_64MS);
    NAU_UPD(NAUL_RDAC, NAUL_RDAC_CLK_DELAY_MASK | NAUL_RDAC_VREF_MASK, (0x2 << 4) | (0x3 << 2));
    NAU_UPD(NAUL_DACL_CTRL, NAUL_DAC_CH_SEL_MASK, NAUL_DAC_CH_SEL_L);
    NAU_UPD(NAUL_DACR_CTRL, NAUL_DAC_CH_SEL_MASK, NAUL_DAC_CH_SEL_R);
    NAU_UPD(NAUL_LEFT_TIME_SLOT, NAUL_DIS_FS_SHORT_DET, NAUL_DIS_FS_SHORT_DET);
    NAU_UPD(NAUL_CHARGE_PUMP, NAUL_ADCOUT_DS_MASK, 0);

    return STATUS_SUCCESS;
}

/* =========================================================================
 * Nau8825_InitializeHardware — reset, identify, Linux init_regs, clock idle.
 * The headphone path stays powered down until a stream starts.
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Nau8825_InitializeHardware(
    _In_ PNAU8825_DEVICE_CONTEXT DevCtx
    )
{
    NTSTATUS status;
    UINT16 deviceId = 0;

    PAGED_CODE();

    if (!DevCtx->SpbTarget) return STATUS_INVALID_DEVICE_STATE;

    DevCtx->HwInitDone = FALSE;
    DevCtx->Streaming = FALSE;

    /* nau8825_reset_chip(): write the reset register twice */
    NAU_WR(NAUL_RESET, 0x0000);
    NAU_WR(NAUL_RESET, 0x0000);
    NauSleepMs(10);

    status = I2cReadRegister16(DevCtx->SpbTarget, NAUL_I2C_DEVICE_ID, &deviceId);
    if (!NT_SUCCESS(status)) {
        NauLog(L"InitStatus", (ULONG)status);
        return status;
    }
    DevCtx->DeviceIdReg = deviceId;
    DevCtx->SwId = (UINT8)(deviceId & NAUL_SOFTWARE_ID_MASK);
    NauLog(L"DeviceIdReg", deviceId);
    NauLog(L"SwId", DevCtx->SwId);
    if (DevCtx->SwId != NAUL_SW_ID_NAU8825 && DevCtx->SwId != NAUL_SW_ID_NAU8825C) {
        /* Linux refuses other ids; keep going as plain NAU8825 but record it */
        NauLog(L"SwIdUnknown", 1);
        DevCtx->SwId = NAUL_SW_ID_NAU8825;
    }

    status = NauInitRegs(DevCtx);
    if (!NT_SUCCESS(status)) { NauLog(L"InitStatus", (ULONG)status); return status; }

    status = NauSysclkIdle(DevCtx);
    if (!NT_SUCCESS(status)) { NauLog(L"InitStatus", (ULONG)status); return status; }

    DevCtx->HwInitDone = TRUE;
    NauLog(L"InitStatus", 0);
    return STATUS_SUCCESS;
}

/* Kept for reference; clocks are now configured per stream (NauPlaybackOn). */
_Use_decl_annotations_
NTSTATUS
Nau8825_ConfigureClocks(
    _In_ PNAU8825_DEVICE_CONTEXT DevCtx
    )
{
    UNREFERENCED_PARAMETER(DevCtx);
    PAGED_CODE();
    return STATUS_SUCCESS;
}

/* Stream start: dai_fmt + hw_params + sysclk FLL_BLK + set_pll + DAPM up. */
static NTSTATUS NauPlaybackOn(_In_ PNAU8825_DEVICE_CONTEXT DevCtx)
{
    NTSTATUS status;

    /* set_dai_fmt(I2S | NB_NF | CBC_CFC) and hw_params(32-bit) */
    NAU_UPD(NAUL_I2S_PCM_CTRL1,
        NAUL_I2S_DL_MASK | NAUL_I2S_DF_MASK | NAUL_I2S_BP_MASK | NAUL_I2S_PCMB_MASK,
        NAUL_I2S_DL_32 | NAUL_I2S_DF_I2S);
    NAU_UPD(NAUL_I2S_PCM_CTRL2, NAUL_I2S_MS_MASK, 0);          /* clock consumer */
    NAU_UPD(NAUL_CLK_DIVIDER, NAUL_CLK_DAC_SRC_MASK, VOLMAR_DAC_CLK_SRC);

    /* sysclk NAU8825_CLK_FLL_BLK: FLL reference = BCLK, max gain error */
    NAU_UPD(NAUL_FLL3, NAUL_FLL_CLK_SRC_MASK | NAUL_GAIN_ERR_MASK,
            NAUL_FLL_CLK_SRC_BLK | (0xF << 12));

    /* set_pll(BCLK 3.072 MHz -> 12.288 MHz): nau8825_fll_apply() */
    NAU_UPD(NAUL_CLK_DIVIDER, NAUL_CLK_SRC_MASK | NAUL_CLK_MCLK_SRC_MASK,
            NAUL_CLK_SRC_MCLK | VOLMAR_FLL_MCLK_SRC);
    NAU_UPD(NAUL_FLL1, NAUL_FLL_RATIO_MASK | NAUL_ICTRL_LATCH_MASK,
            VOLMAR_FLL_RATIO | (0x6 << 10));
    if (DevCtx->SwId == NAUL_SW_ID_NAU8825) {
        NAU_WR(NAUL_FLL2, 0x0000);                               /* 16-bit fraction = 0 */
    } else {
        NAU_WR(NAUL_FLL2_LOWER, 0x0000);                         /* 24-bit fraction = 0 */
        NAU_WR(NAUL_FLL2_UPPER, 0x0000);
    }
    NAU_UPD(NAUL_FLL3, NAUL_FLL_INTEGER_MASK, VOLMAR_FLL_INT);
    NAU_UPD(NAUL_FLL4, NAUL_FLL_REF_DIV_MASK, VOLMAR_FLL_REF_DIV << 10);
    NAU_UPD(NAUL_FLL5, NAUL_FLL_CLK_SW_MASK, 0);                 /* divided VCO input */
    NAU_UPD(NAUL_FLL6, NAUL_DCO_EN, 0);                          /* no free running */
    /* fraction == 0: loop filter off, accumulator mode */
    NAU_UPD(NAUL_FLL5, NAUL_FLL_PDB_DAC_EN | NAUL_FLL_LOOP_FTR_EN | NAUL_FLL_FTR_SW_MASK,
            NAUL_FLL_FTR_SW_ACCU);
    NAU_UPD(NAUL_FLL6, NAUL_SDM_EN | NAUL_CUTOFF500, 0);
    NauSleepMs(2);
    NAU_UPD(NAUL_CLK_DIVIDER, NAUL_CLK_SRC_MASK, NAUL_CLK_SRC_VCO);

    /* DAPM power-up order (supplies, DACs, then PGA_S subsequences 0..10) */
    NAU_UPD(NAUL_ENA_CTRL, NAUL_ENABLE_DAC_CLK, NAUL_ENABLE_DAC_CLK);         /* DDAC Clock */
    NAU_UPD(NAUL_ENA_CTRL, NAUL_ENABLE_DACL | NAUL_ENABLE_DACR,
            NAUL_ENABLE_DACL | NAUL_ENABLE_DACR);                             /* DDACL/R */
    NAU_UPD(NAUL_CLASSG_CTRL, NAUL_CLASSG_LDAC_EN | NAUL_CLASSG_RDAC_EN,
            NAUL_CLASSG_LDAC_EN | NAUL_CLASSG_RDAC_EN);                       /* 0: HP amp L/R */
    NAU_UPD(NAUL_CHARGE_PUMP, NAUL_CHARGE_PUMP_EN, NAUL_CHARGE_PUMP_EN);      /* 1: Charge Pump */
    NauSleepMs(10);                                                           /*    let it ramp */
    NAU_UPD(NAUL_CHARGE_PUMP, NAUL_JAMNODCLOW, NAUL_JAMNODCLOW);
    NAU_UPD(NAUL_RDAC, NAUL_RDAC_EN, NAUL_RDAC_EN);                           /* 2: ADACL/R */
    NAU_UPD(NAUL_RDAC, NAUL_RDAC_CLK_EN, NAUL_RDAC_CLK_EN);                   /* 3: ADAC clocks */
    NAU_UPD(NAUL_POWER_UP_CONTROL, NAUL_PU_OUT_STAGE1, NAUL_PU_OUT_STAGE1);   /* 4 */
    NAU_UPD(NAUL_POWER_UP_CONTROL, NAUL_PU_OUT_STAGE2, NAUL_PU_OUT_STAGE2);   /* 5 */
    NAU_UPD(NAUL_POWER_UP_CONTROL, NAUL_PU_OUT_STAGE3, NAUL_PU_OUT_STAGE3);   /* 6 */
    /* 7: Output DAC PRE_PMU - let the DAC signal through */
    NAU_UPD(NAUL_BIAS_ADJ, NAUL_BIAS_TESTDAC_EN, 0);
    NAU_UPD(NAUL_CHARGE_PUMP, NAUL_POWER_DOWN_DACR | NAUL_POWER_DOWN_DACL,
            (DevCtx->SwId == NAUL_SW_ID_NAU8825) ? 0 : (NAUL_POWER_DOWN_DACR | NAUL_POWER_DOWN_DACL));
    NAU_UPD(NAUL_HSD_CTRL, NAUL_HSD_HPL_GND | NAUL_HSD_HPR_GND, 0);           /* 8: unground HP */
    NAU_UPD(NAUL_BOOST, NAUL_HP_BOOST_DIS, 0);                                /* 9: HP boost on */
    NAU_UPD(NAUL_CLASSG_CTRL, NAUL_CLASSG_EN, NAUL_CLASSG_EN);                /* 10: Class G */

    /* 0 dB headphone volume, unmuted (reset default); DSP volume is -20 dB */
    NAU_UPD(NAUL_HSVOL_CTRL, NAUL_HP_MUTE | NAUL_HPL_MUTE | NAUL_HPR_MUTE |
            (NAUL_HP_VOL_MASK << NAUL_HPL_VOL_SFT) | NAUL_HP_VOL_MASK, 0);

    return STATUS_SUCCESS;
}

/* Stream stop: DAPM power-down in reverse order, then idle clock. */
static NTSTATUS NauPlaybackOff(_In_ PNAU8825_DEVICE_CONTEXT DevCtx)
{
    NTSTATUS status;

    NAU_UPD(NAUL_CLASSG_CTRL, NAUL_CLASSG_EN, 0);                             /* 10 */
    NAU_UPD(NAUL_BOOST, NAUL_HP_BOOST_DIS, NAUL_HP_BOOST_DIS);                /* 9 */
    NAU_UPD(NAUL_HSD_CTRL, NAUL_HSD_HPL_GND | NAUL_HSD_HPR_GND,
            NAUL_HSD_HPL_GND | NAUL_HSD_HPR_GND);                             /* 8 */
    NAU_UPD(NAUL_POWER_UP_CONTROL, NAUL_PU_OUT_STAGE3, 0);                    /* 6 */
    NAU_UPD(NAUL_POWER_UP_CONTROL, NAUL_PU_OUT_STAGE2, 0);                    /* 5 */
    NAU_UPD(NAUL_POWER_UP_CONTROL, NAUL_PU_OUT_STAGE1, 0);                    /* 4 */
    /* 7: Output DAC POST_PMD */
    NAU_UPD(NAUL_BIAS_ADJ, NAUL_BIAS_TESTDAC_EN, NAUL_BIAS_TESTDAC_EN);
    NAU_UPD(NAUL_CHARGE_PUMP, NAUL_POWER_DOWN_DACR | NAUL_POWER_DOWN_DACL,
            (DevCtx->SwId == NAUL_SW_ID_NAU8825) ? (NAUL_POWER_DOWN_DACR | NAUL_POWER_DOWN_DACL) : 0);
    NAU_UPD(NAUL_RDAC, NAUL_RDAC_CLK_EN, 0);                                  /* 3 */
    NAU_UPD(NAUL_RDAC, NAUL_RDAC_EN, 0);                                      /* 2 */
    NAU_UPD(NAUL_CHARGE_PUMP, NAUL_JAMNODCLOW, 0);                            /* 1: PRE_PMD */
    NAU_UPD(NAUL_CHARGE_PUMP, NAUL_CHARGE_PUMP_EN, 0);
    NAU_UPD(NAUL_CLASSG_CTRL, NAUL_CLASSG_LDAC_EN | NAUL_CLASSG_RDAC_EN, 0);  /* 0 */
    NAU_UPD(NAUL_ENA_CTRL, NAUL_ENABLE_DACL | NAUL_ENABLE_DACR, 0);
    NAU_UPD(NAUL_ENA_CTRL, NAUL_ENABLE_DAC_CLK, 0);

    return NauSysclkIdle(DevCtx);                                             /* System Clock PMD */
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

    /* JACK_DET_CTRL (polarity, debounce) is set by NauInitRegs() exactly as
     * Linux does; the old fixed write here overwrote it and is gone. */

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
 * Nau8825_SetPlaybackState — Linux-order headphone path power up / down
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

    if (!DevCtx->HwInitDone) {
        status = Nau8825_InitializeHardware(DevCtx);
        if (!NT_SUCCESS(status)) return status;
    }

    if (Enable) {
        if (DevCtx->Streaming) return STATUS_SUCCESS;
        status = NauPlaybackOn(DevCtx);
        NauLog(L"PlaybackOnStatus", (ULONG)status);
        NauLog(L"JackInserted", NauJackInserted(DevCtx));
        NauSnapshot(DevCtx);                 /* register state while playing */
        if (NT_SUCCESS(status)) DevCtx->Streaming = TRUE;
    } else {
        if (!DevCtx->Streaming) return STATUS_SUCCESS;
        status = NauPlaybackOff(DevCtx);
        NauLog(L"PlaybackOffStatus", (ULONG)status);
        DevCtx->Streaming = FALSE;
    }

    return status;
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
 * Nau8825_SetVolume — headphone mute only for now
 *
 * The old code wrote the level into the wrong bit fields (it could set the
 * HPL/HPR mute bits by accident). Until the Windows volume mapping is
 * verified, the codec stays at 0 dB and only mute is honoured; loudness is
 * set by the DSP volume (-20 dB).
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
Nau8825_SetVolume(
    _In_ PNAU8825_DEVICE_CONTEXT DevCtx,
    _In_ UINT8 VolumeStep,
    _In_ BOOLEAN Mute
    )
{
    PAGED_CODE();

    if (!DevCtx->SpbTarget) return STATUS_INVALID_DEVICE_STATE;

    DevCtx->VolumeStep = VolumeStep;
    DevCtx->Muted      = Mute;

    return I2cUpdateBits16(DevCtx->SpbTarget, NAUL_HSVOL_CTRL,
        (UINT16)(NAUL_HPL_MUTE | NAUL_HPR_MUTE),
        (UINT16)(Mute ? (NAUL_HPL_MUTE | NAUL_HPR_MUTE) : 0));
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
    BOOLEAN inserted = FALSE;
    UINT16 raw = 0;
    BOOLEAN changed = FALSE;
    CSAUDIO_JACK_STATE newJackState;

    PAGED_CODE();

    if (!DevCtx->SpbTarget || !DevCtx->JackLock) return;

    WdfWaitLockAcquire(DevCtx->JackLock, NULL);

    if (!NauJackRead(DevCtx, &inserted, &raw)) {
        WdfWaitLockRelease(DevCtx->JackLock);
        return;
    }

    if (!DevCtx->JackKnown) {
        /* first reading after power-up: take it as is */
        DevCtx->JackKnown = TRUE;
        DevCtx->JackInserted = inserted;
        DevCtx->JackCandidate = 0xFF;
        changed = TRUE;
    } else if (inserted == DevCtx->JackInserted) {
        DevCtx->JackCandidate = 0xFF;          /* bounce went away */
    } else if (DevCtx->JackCandidate == (UINT8)inserted) {
        /* same new level on two polls in a row -> accept */
        DevCtx->JackInserted = inserted;
        DevCtx->JackCandidate = 0xFF;
        changed = TRUE;
    } else {
        DevCtx->JackCandidate = (UINT8)inserted;
    }

    if (!changed) {
        WdfWaitLockRelease(DevCtx->JackLock);
        return;
    }

    newJackState = inserted ? CSAUDIO_JACK_HEADPHONES_3POLE : CSAUDIO_JACK_UNPLUGGED;
    DevCtx->MicPresent = FALSE;
    DevCtx->CurrentJackState = newJackState;
    DevCtx->JackChanges++;
    WdfWaitLockRelease(DevCtx->JackLock);

    NauLog(L"JackRaw", raw);
    NauLog(L"JackInserted", inserted);
    NauLog(L"JackChanges", DevCtx->JackChanges);
    NauLog(L"JackActiveLow", DevCtx->JackActiveLow);

    /* Complete all pending notification requests (CsAudioEndpointSwitcher) */
    if (DevCtx->JackNotificationQueue) {
        WDFREQUEST notifyRequest;
        while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(DevCtx->JackNotificationQueue, &notifyRequest))) {
            PCSAUDIO_JACK_STATE pOutState;
            NTSTATUS status = WdfRequestRetrieveOutputBuffer(notifyRequest, sizeof(CSAUDIO_JACK_STATE), (PVOID*)&pOutState, NULL);
            if (NT_SUCCESS(status)) {
                *pOutState = newJackState;
                WdfRequestCompleteWithInformation(notifyRequest, STATUS_SUCCESS, sizeof(CSAUDIO_JACK_STATE));
            } else {
                WdfRequestComplete(notifyRequest, status);
            }
        }
    }
}

/* Periodic jack poll (passive level). */
VOID
Nau8825_EvtJackTimer(
    _In_ WDFTIMER Timer
    )
{
    WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PNAU8825_DEVICE_CONTEXT devCtx = GetNau8825Context(device);
    if (devCtx->JackPollStop) return;
    if (devCtx->PowerActive) {
        Nau8825_ProcessJackStateChange(devCtx);
    }
    if (!devCtx->JackPollStop) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(NAU_JACK_POLL_MS));
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

    if (devCtx->SpbTarget) {
        I2cWriteRegister16(devCtx->SpbTarget, NAU8825_REG_INT_CLR_KEY_STATUS, 0xFFFF);
    }
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
