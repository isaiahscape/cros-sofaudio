/*
 * csaudio_codec.h — Shared Audio Bus & Sidecar Interface Definitions
 *
 * Reverse engineered from ChromeOS Intel Sound Open Firmware (SOF) audio architecture
 * and designed for Windows 11 KMDF Audio Driver Interoperability.
 *
 * Compatible with CoolStar's CSAudio bus (csaudiointcsof / csaudiosstavs / sklhdaudbus)
 * and standalone Windows PortCls / WaveRT audio miniport drivers.
 *
 * Copyright (c) 2026. Licensed under the MIT License.
 */

#pragma once

#include <initguid.h>

/* =========================================================================
 * Device Interface GUIDs
 * ========================================================================= */

/* {71B1AA20-1D88-4A97-94E5-B4C254DF5741} */
DEFINE_GUID(GUID_DEVINTERFACE_CSAUDIO_CODEC,
    0x71b1aa20, 0x1d88, 0x4a97, 0x94, 0xe5, 0xb4, 0xc2, 0x54, 0xdf, 0x57, 0x41);

/* {A89746A1-9837-4D21-B457-3C98373E0001} */
DEFINE_GUID(GUID_DEVINTERFACE_MAX98373_AMP,
    0xa89746a1, 0x9837, 0x4d21, 0xb4, 0x57, 0x3c, 0x98, 0x37, 0x3e, 0x00, 0x01);

/* {88250001-8825-4B88-A882-8825NAU88L25} */
DEFINE_GUID(GUID_DEVINTERFACE_NAU8825_CODEC,
    0x88250001, 0x8825, 0x4b88, 0xa8, 0x82, 0x88, 0x25, 0xba, 0x88, 0x25, 0x01);

/* =========================================================================
 * Codec Types & Endpoints
 * ========================================================================= */

typedef enum _CSAUDIO_CODEC_TYPE {
    CSAUDIO_CODEC_TYPE_UNKNOWN   = 0,
    CSAUDIO_CODEC_TYPE_SPEAKER   = 1,   /* Maxim MAX98373 Smart Amp */
    CSAUDIO_CODEC_TYPE_HEADPHONE = 2,   /* Nuvoton NAU88L25 DAC */
    CSAUDIO_CODEC_TYPE_HEADSET   = 3,   /* NAU88L25 DAC + Mic ADC + Jack Detect */
    CSAUDIO_CODEC_TYPE_DMIC      = 4,   /* Digital Mic Array */
} CSAUDIO_CODEC_TYPE;

typedef enum {
    CSAudioEndpointTypeDSP        = 0,
    CSAudioEndpointTypeSpeaker    = 1,
    CSAudioEndpointTypeHeadphone  = 2,
    CSAudioEndpointTypeMicArray   = 3,
    CSAudioEndpointTypeMicJack    = 4
} CSAudioEndpointType;

typedef enum {
    CSAudioEndpointRegister       = 0,
    CSAudioEndpointStart          = 1,
    CSAudioEndpointStop           = 2,
    CSAudioEndpointOverrideFormat = 3
} CSAudioEndpointRequest;

typedef struct CSAUDIOFORMATOVERRIDE {
    UINT16 channels;
    UINT16 frequency;
    UINT16 bitsPerSample;
    UINT16 validBitsPerSample;
    BOOLEAN force32BitOutputContainer;
} CsAudioFormatOverride;

#pragma warning(push)
#pragma warning(disable:4201)
typedef struct CSAUDIOARG {
    UINT32 argSz;
    CSAudioEndpointType endpointType;
    CSAudioEndpointRequest endpointRequest;
    union {
        CsAudioFormatOverride formatOverride;
    };
} CsAudioArg, *PCsAudioArg;
#pragma warning(pop)

typedef enum _CSAUDIO_ENDPOINT_TYPE {
    CSAUDIO_ENDPOINT_SPEAKER     = 0x01,
    CSAUDIO_ENDPOINT_HEADPHONE   = 0x02,
    CSAUDIO_ENDPOINT_HEADSET_MIC = 0x04,
    CSAUDIO_ENDPOINT_INTERNAL_MIC= 0x08,
} CSAUDIO_ENDPOINT_TYPE;

/* =========================================================================
 * Jack Detection Flags
 * ========================================================================= */

typedef enum _CSAUDIO_JACK_STATE {
    CSAUDIO_JACK_UNPLUGGED        = 0x00,
    CSAUDIO_JACK_HEADPHONES_3POLE = 0x01,  /* Standard TRS stereo headphones (no mic) */
    CSAUDIO_JACK_HEADSET_4POLE    = 0x02,  /* TRRS headset with microphone (CTIA/OMTP) */
    CSAUDIO_JACK_LINE_OUT         = 0x04,  /* High impedance line output */
    CSAUDIO_JACK_BUTTON_PRESSED   = 0x10,  /* Hook switch / play-pause */
    CSAUDIO_JACK_VOL_UP           = 0x20,  /* Headset volume up */
    CSAUDIO_JACK_VOL_DOWN         = 0x40,  /* Headset volume down */
} CSAUDIO_JACK_STATE, *PCSAUDIO_JACK_STATE;

/* =========================================================================
 * Codec Audio Stream Configuration
 * ========================================================================= */

typedef struct _CSAUDIO_STREAM_CONFIG {
    UINT32 SampleRate;       /* 48000, 44100, 96000 */
    UINT8  BitsPerSample;    /* 16, 24, 32 */
    UINT8  Channels;         /* 1 (mono), 2 (stereo) */
    UINT8  SlotWidth;        /* 16, 24, 32 */
    UINT8  RxSlot;           /* TDM slot index for playback */
    UINT8  TxSlot;           /* TDM slot index for feedback/capture */
} CSAUDIO_STREAM_CONFIG, *PCSAUDIO_STREAM_CONFIG;

/* =========================================================================
 * IOCTL Definitions
 * ========================================================================= */

#define FILE_DEVICE_CSAUDIO  0x8825

#define IOCTL_CSAUDIO_REGISTER_CODEC \
    CTL_CODE(FILE_DEVICE_CSAUDIO, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_CSAUDIO_UNREGISTER_CODEC \
    CTL_CODE(FILE_DEVICE_CSAUDIO, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_CSAUDIO_STREAM_START \
    CTL_CODE(FILE_DEVICE_CSAUDIO, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_CSAUDIO_STREAM_STOP \
    CTL_CODE(FILE_DEVICE_CSAUDIO, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_CSAUDIO_SET_VOLUME \
    CTL_CODE(FILE_DEVICE_CSAUDIO, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_CSAUDIO_GET_VOLUME \
    CTL_CODE(FILE_DEVICE_CSAUDIO, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_CSAUDIO_SET_MUTE \
    CTL_CODE(FILE_DEVICE_CSAUDIO, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_CSAUDIO_GET_JACK_STATE \
    CTL_CODE(FILE_DEVICE_CSAUDIO, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_CSAUDIO_REGISTER_JACK_NOTIFICATION \
    CTL_CODE(FILE_DEVICE_CSAUDIO, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_CSAUDIO_READ_REG \
    CTL_CODE(FILE_DEVICE_CSAUDIO, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_CSAUDIO_WRITE_REG \
    CTL_CODE(FILE_DEVICE_CSAUDIO, 0x811, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* =========================================================================
 * IOCTL Request / Response Payloads
 * ========================================================================= */

typedef struct _CSAUDIO_REGISTER_PARAMS {
    CSAUDIO_CODEC_TYPE CodecType;
    UINT32             SupportedEndpoints; /* Bitmask of CSAUDIO_ENDPOINT_TYPE */
    CHAR               DeviceName[64];
} CSAUDIO_REGISTER_PARAMS, *PCSAUDIO_REGISTER_PARAMS;

typedef struct _CSAUDIO_VOLUME_PARAMS {
    UINT8  Channel;          /* 0 = Left / Master, 1 = Right */
    UINT8  VolumeDbLevel;    /* Attenuation in dB (e.g., 0 to 60) */
    BOOLEAN Muted;
} CSAUDIO_VOLUME_PARAMS, *PCSAUDIO_VOLUME_PARAMS;

typedef struct _CSAUDIO_REG_IO {
    UINT16 Register;
    UINT16 Value;            /* For MAX98373, only lower 8 bits used */
} CSAUDIO_REG_IO, *PCSAUDIO_REG_IO;
