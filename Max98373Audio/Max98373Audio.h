/*
 * Max98373Audio.h — Maxim MAX98373 KMDF Audio Driver Header
 *
 * Implements driver context, hardware state, and declarations for Windows 11.
 *
 * Copyright (c) 2026. Licensed under the MIT License.
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <spb.h>
#include <ntstrsafe.h>
#include <initguid.h>
#include <devpkey.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>

#include "..\Common\csaudio_codec.h"
#include "..\Common\i2c_helper.h"
#include "max98373_reg.h"

#define MAX98373_POOL_TAG '3789'

typedef enum _SPEAKER_CHANNEL_ROLE {
    SPEAKER_ROLE_MONO       = 0,
    SPEAKER_ROLE_LEFT       = 1,
    SPEAKER_ROLE_RIGHT      = 2,
    SPEAKER_ROLE_STEREO     = 3,
} SPEAKER_CHANNEL_ROLE;

typedef struct _MAX98373_DEVICE_CONTEXT {
    /* KMDF Handles */
    WDFDEVICE               Device;
    WDFIOTARGET             SpbTarget;
    WDFSPINLOCK             Lock;
    WDFQUEUE                DefaultQueue;

    /* Hardware Identification & Configuration */
    LARGE_INTEGER           I2cConnectionId;
    UINT8                   I2cAddress;
    SPEAKER_CHANNEL_ROLE    SpeakerRole;
    UINT8                   RevisionId;

    /* Audio Runtime State */
    BOOLEAN                 PowerActive;      /* TRUE when in D0 */
    BOOLEAN                 Streaming;        /* TRUE when audio is playing */
    BOOLEAN                 Muted;            /* Software mute state */
    UINT8                   VolumeDb;         /* Attenuation: 0 (0dB) to 0x7F (-63.5dB) */
    UINT8                   HardwareGain;     /* Gain setting (default 0x08 = +15.5dB) */
    UINT32                  SampleRate;       /* Current PCM sample rate (default 48000) */

    /* ACPI Settings */
    BOOLEAN                 InterleaveSlot;
    UINT8                   TdmRxSlot;        /* TDM playback channel */
    UINT8                   TdmTxSlot;        /* TDM feedback channel (I/V sense) */
    UINT8                   VmonSlot;         /* Voltage sense slot (0 for right, 2 for left) */
    UINT8                   ImonSlot;         /* Current sense slot (1 for right, 3 for left) */

} MAX98373_DEVICE_CONTEXT, *PMAX98373_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MAX98373_DEVICE_CONTEXT, GetMax98373Context)

/* Driver lifecycle routines */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD Max98373_EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE Max98373_EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE Max98373_EvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY Max98373_EvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT Max98373_EvtD0Exit;

/* Queue & IOCTL handlers */
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Max98373_EvtIoDeviceControl;

/* Internal hardware helpers */
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
Max98373_InitializeHardware(
    _In_ PMAX98373_DEVICE_CONTEXT DevCtx
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
Max98373_SetPowerState(
    _In_ PMAX98373_DEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN PowerOn
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
Max98373_SetStreamState(
    _In_ PMAX98373_DEVICE_CONTEXT DevCtx,
    _In_ BOOLEAN Start
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
Max98373_SetVolume(
    _In_ PMAX98373_DEVICE_CONTEXT DevCtx,
    _In_ UINT8 VolumeDb,
    _In_ BOOLEAN Mute
    );
