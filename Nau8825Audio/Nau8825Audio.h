/*
 * Nau8825Audio.h — Nuvoton NAU88L25 KMDF Audio Codec & Jack Detection Header
 *
 * Implements driver context, jack state machine, and declarations for Windows 11.
 *
 * Copyright (c) 2026. Licensed under the MIT License.
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <spb.h>
#include <ntstrsafe.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>

#include "..\Common\csaudio_codec.h"
#include "..\Common\i2c_helper.h"
#include "nau8825_reg.h"
#include "nau8825_seq.h"

/* Ensure debug prints always emit to kernel debug output in both Release and Debug */
#ifdef KdPrintEx
#undef KdPrintEx
#endif
#define KdPrintEx(args) DbgPrintEx args

#define NAU8825_POOL_TAG '5288'

typedef struct _NAU8825_DEVICE_CONTEXT {
    /* KMDF Handles */
    WDFDEVICE               Device;
    WDFIOTARGET             SpbTarget;
    WDFINTERRUPT            Interrupt;
    WDFWORKITEM             JackWorkItem;
    WDFSPINLOCK             Lock;
    WDFQUEUE                DefaultQueue;
    WDFQUEUE                JackNotificationQueue; /* Pending IRPs awaiting jack events */

    /* Hardware Resources */
    LARGE_INTEGER           I2cConnectionId;
    ULONG                   GpioInterruptVector;

    /* Runtime State */
    BOOLEAN                 PowerActive;      /* TRUE when in D0 */
    BOOLEAN                 Streaming;        /* TRUE when headphone DAC streaming */
    BOOLEAN                 CaptureActive;    /* TRUE when mic ADC streaming */
    BOOLEAN                 Muted;            /* Headphone mute state */
    UINT8                   VolumeStep;       /* 0 (0dB) to 0x36 (-54dB) */
    UINT32                  SampleRate;       /* Default 48000 */

    /* Jack Detection State */
    CSAUDIO_JACK_STATE      CurrentJackState;
    BOOLEAN                 JackInserted;
    BOOLEAN                 MicPresent;       /* 4-pole TRRS detected */

    /* Jack polling (I2C_DEVICE_ID.GPIO2JD1 every NAU_JACK_POLL_MS) */
    WDFTIMER                JackTimer;
    WDFWAITLOCK             JackLock;
    BOOLEAN                 JackKnown;        /* first reading done */
    volatile BOOLEAN        JackPollStop;     /* set in D0Exit before stopping the timer */
    BOOLEAN                 JackActiveLow;    /* Config\JackActiveLow (default 0: pin high = plugged) */
    UINT8                   JackCandidate;    /* debounce: 0/1 = pending new state, 0xFF = none */
    ULONG                   JackChanges;

    /* Chip identification (Linux nau8825: sw_id = I2C_DEVICE_ID & 3) */
    UINT16                  DeviceIdReg;
    UINT8                   SwId;
    BOOLEAN                 HwInitDone;

} NAU8825_DEVICE_CONTEXT, *PNAU8825_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NAU8825_DEVICE_CONTEXT, GetNau8825Context)

/* Driver lifecycle routines */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD Nau8825_EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE Nau8825_EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE Nau8825_EvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY Nau8825_EvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT Nau8825_EvtD0Exit;

EVT_WDF_TIMER Nau8825_EvtJackTimer;

/* Interrupt & WorkItem Callbacks */
EVT_WDF_INTERRUPT_ISR Nau8825_EvtInterruptIsr;
EVT_WDF_INTERRUPT_WORKITEM Nau8825_EvtInterruptWorkItem;

/* Queue & IOCTL handlers */
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Nau8825_EvtIoDeviceControl;

/* Internal hardware helpers */
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
Nau8825_InitializeHardware(
    _In_ PNAU8825_DEVICE_CONTEXT DevCtx
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
Nau8825_ConfigureClocks(
    _In_ PNAU8825_DEVICE_CONTEXT DevCtx
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
Nau8825_EnableJackDetection(
    _In_ PNAU8825_DEVICE_CONTEXT DevCtx
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
Nau8825_SetPlaybackState(
    _In_ PNAU8825_DEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN Enable
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
Nau8825_SetCaptureState(
    _In_ PNAU8825_DEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN Enable
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
Nau8825_SetVolume(
    _In_ PNAU8825_DEVICE_CONTEXT DevCtx,
    _In_ UINT8 VolumeStep,
    _In_ BOOLEAN Mute
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
Nau8825_ProcessJackStateChange(
    _In_ PNAU8825_DEVICE_CONTEXT DevCtx
    );
