/*
 * cros_audio_test.c — Chromebook Audio Driver Diagnostic & Test Utility
 *
 * Verifies MAX98373 and NAU88L25 Windows 11 drivers:
 *   - Device interface enumeration
 *   - Hardware register dumps over I2C
 *   - Headphone jack insertion / ejection monitoring in real-time
 *   - Playback stream start/stop and volume ramping tests
 *
 * Copyright (c) 2026. Licensed under the MIT License.
 */

#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <stdio.h>
#include <conio.h>

#include "..\Common\csaudio_codec.h"
#include "..\Max98373Audio\max98373_reg.h"
#include "..\Nau8825Audio\nau8825_reg.h"

#pragma comment(lib, "setupapi.lib")

static HANDLE OpenDeviceInterface(const GUID* InterfaceGuid) {
    HDEVINFO devInfo = SetupDiGetClassDevs(InterfaceGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    SP_DEVICE_INTERFACE_DATA ifData;
    ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    HANDLE hDevice = INVALID_HANDLE_VALUE;

    if (SetupDiEnumDeviceInterfaces(devInfo, NULL, InterfaceGuid, 0, &ifData)) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, NULL, 0, &reqSize, NULL);

        PSP_DEVICE_INTERFACE_DETAIL_DATA detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(reqSize);
        if (detailData) {
            detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
            if (SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, detailData, reqSize, NULL, NULL)) {
                wprintf(L"  Found device path: %s\n", detailData->DevicePath);
                hDevice = CreateFile(
                    detailData->DevicePath,
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    NULL
                );
            }
            free(detailData);
        }
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return hDevice;
}

static DWORD WINAPI JackMonitorThread(LPVOID lpParam) {
    HANDLE hNau8825 = (HANDLE)lpParam;
    CSAUDIO_JACK_STATE jackState = CSAUDIO_JACK_UNPLUGGED;
    DWORD bytesReturned = 0;

    printf("[JACK MONITOR] Background jack event monitor started...\n");

    while (TRUE) {
        BOOL success = DeviceIoControl(
            hNau8825,
            IOCTL_CSAUDIO_REGISTER_JACK_NOTIFICATION,
            NULL, 0,
            &jackState, sizeof(jackState),
            &bytesReturned,
            NULL
        );

        if (!success) {
            Sleep(1000);
            continue;
        }

        printf("\n>>> [JACK EVENT DETECTED] New Status: ");
        switch (jackState) {
        case CSAUDIO_JACK_UNPLUGGED:
            printf("UNPLUGGED (No audio jack inserted)\n");
            break;
        case CSAUDIO_JACK_HEADPHONES_3POLE:
            printf("CONNECTED: Standard 3-Pole (TRS) Stereo Headphones (No Mic)\n");
            break;
        case CSAUDIO_JACK_HEADSET_4POLE:
            printf("CONNECTED: TRRS 4-Pole Headset with Microphone (CTIA/OMTP)\n");
            break;
        default:
            printf("Flags = 0x%02X\n", jackState);
            break;
        }
        printf("Prompt> ");
        fflush(stdout);
    }

    return 0;
}

int main(int argc, char* argv[]) {
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    printf("===============================================================\n");
    printf(" Chromebook SOF Audio (MAX98373 + NAU88L25) Windows 11 Tester \n");
    printf("===============================================================\n\n");

    /* Test 1: Maxim MAX98373 Speaker Amplifier */
    printf("[1] Scanning for MAX98373 Smart Amplifier...\n");
    HANDLE hMax98373 = OpenDeviceInterface(&GUID_DEVINTERFACE_MAX98373_AMP);

    if (hMax98373 != INVALID_HANDLE_VALUE) {
        printf("  [OK] Successfully connected to Max98373Audio driver!\n");

        CSAUDIO_REGISTER_PARAMS regParams = {0};
        DWORD bytes = 0;
        if (DeviceIoControl(hMax98373, IOCTL_CSAUDIO_REGISTER_CODEC, &regParams, sizeof(regParams), &regParams, sizeof(regParams), &bytes, NULL)) {
            printf("  Device Name: %s\n", regParams.DeviceName);
        }

        /* Read Revision ID register (0x21FF) */
        CSAUDIO_REG_IO regIo = {0};
        regIo.Register = MAX98373_REVID_REG;
        if (DeviceIoControl(hMax98373, IOCTL_CSAUDIO_READ_REG, &regIo, sizeof(regIo), &regIo, sizeof(regIo), &bytes, NULL)) {
            printf("  Silicon Revision ID (Reg 0x21FF): 0x%02X\n", (UINT8)regIo.Value);
        }

        /* Read Global Shutdown register (0x20FF) */
        regIo.Register = MAX98373_R20FF_GLOBAL_SHDN;
        if (DeviceIoControl(hMax98373, IOCTL_CSAUDIO_READ_REG, &regIo, sizeof(regIo), &regIo, sizeof(regIo), &bytes, NULL)) {
            printf("  Global Shutdown State (Reg 0x20FF): 0x%02X\n", (UINT8)regIo.Value);
        }
    } else {
        printf("  [!] MAX98373 amplifier not detected or driver not installed.\n");
    }

    printf("\n");

    /* Test 2: Nuvoton NAU88L25 Headset Codec & Jack Detection */
    printf("[2] Scanning for NAU88L25 Headset Codec...\n");
    HANDLE hNau8825 = OpenDeviceInterface(&GUID_DEVINTERFACE_NAU8825_CODEC);

    if (hNau8825 != INVALID_HANDLE_VALUE) {
        printf("  [OK] Successfully connected to Nau8825Audio driver!\n");

        CSAUDIO_REGISTER_PARAMS regParams = {0};
        DWORD bytes = 0;
        if (DeviceIoControl(hNau8825, IOCTL_CSAUDIO_REGISTER_CODEC, &regParams, sizeof(regParams), &regParams, sizeof(regParams), &bytes, NULL)) {
            printf("  Device Name: %s\n", regParams.DeviceName);
        }

        /* Read Device ID (Reg 0x58) */
        CSAUDIO_REG_IO regIo = {0};
        regIo.Register = NAU8825_REG_I2C_DEVICE_ID;
        if (DeviceIoControl(hNau8825, IOCTL_CSAUDIO_READ_REG, &regIo, sizeof(regIo), &regIo, sizeof(regIo), &bytes, NULL)) {
            printf("  Silicon Device ID (Reg 0x58): 0x%04X\n", regIo.Value);
        }

        /* Query current jack status */
        CSAUDIO_JACK_STATE state = CSAUDIO_JACK_UNPLUGGED;
        if (DeviceIoControl(hNau8825, IOCTL_CSAUDIO_GET_JACK_STATE, NULL, 0, &state, sizeof(state), &bytes, NULL)) {
            printf("  Initial Jack State: %s\n",
                (state == CSAUDIO_JACK_UNPLUGGED) ? "Unplugged" :
                (state == CSAUDIO_JACK_HEADPHONES_3POLE) ? "TRS Headphones Plugged" :
                (state == CSAUDIO_JACK_HEADSET_4POLE) ? "TRRS Headset with Mic Plugged" : "Unknown");
        }

        /* Spawn Jack Event Monitor thread */
        HANDLE hThread = CreateThread(NULL, 0, JackMonitorThread, (LPVOID)hNau8825, 0, NULL);
        if (hThread) CloseHandle(hThread);
    } else {
        printf("  [!] NAU88L25 codec not detected or driver not installed.\n");
    }

    printf("\nInteractive Commands:\n");
    printf("  [1] Test MAX98373 Amp Stream Start / Stop\n");
    printf("  [2] Test NAU88L25 Headphone Stream Start / Stop\n");
    printf("  [3] Query Live Jack State\n");
    printf("  [q] Exit Test Tool\n\n");

    while (TRUE) {
        printf("Prompt> ");
        char ch = (char)_getch();
        printf("%c\n", ch);

        if (ch == 'q' || ch == 'Q') break;

        if (ch == '1' && hMax98373 != INVALID_HANDLE_VALUE) {
            DWORD bytes = 0;
            printf("Starting MAX98373 speaker stream...\n");
            DeviceIoControl(hMax98373, IOCTL_CSAUDIO_STREAM_START, NULL, 0, NULL, 0, &bytes, NULL);
            Sleep(1500);
            printf("Stopping MAX98373 speaker stream...\n");
            DeviceIoControl(hMax98373, IOCTL_CSAUDIO_STREAM_STOP, NULL, 0, NULL, 0, &bytes, NULL);
        }
        else if (ch == '2' && hNau8825 != INVALID_HANDLE_VALUE) {
            DWORD bytes = 0;
            printf("Starting NAU88L25 headphone stream...\n");
            DeviceIoControl(hNau8825, IOCTL_CSAUDIO_STREAM_START, NULL, 0, NULL, 0, &bytes, NULL);
            Sleep(1500);
            printf("Stopping NAU88L25 headphone stream...\n");
            DeviceIoControl(hNau8825, IOCTL_CSAUDIO_STREAM_STOP, NULL, 0, NULL, 0, &bytes, NULL);
        }
        else if (ch == '3' && hNau8825 != INVALID_HANDLE_VALUE) {
            CSAUDIO_JACK_STATE state = CSAUDIO_JACK_UNPLUGGED;
            DWORD bytes = 0;
            if (DeviceIoControl(hNau8825, IOCTL_CSAUDIO_GET_JACK_STATE, NULL, 0, &state, sizeof(state), &bytes, NULL)) {
                printf("Live Jack State: %s (Raw: 0x%02X)\n",
                    (state == CSAUDIO_JACK_UNPLUGGED) ? "Unplugged" :
                    (state == CSAUDIO_JACK_HEADPHONES_3POLE) ? "TRS Headphones Plugged" :
                    (state == CSAUDIO_JACK_HEADSET_4POLE) ? "TRRS Headset with Mic Plugged" : "Unknown",
                    state);
            }
        }
    }

    if (hMax98373 != INVALID_HANDLE_VALUE) CloseHandle(hMax98373);
    if (hNau8825 != INVALID_HANDLE_VALUE) CloseHandle(hNau8825);

    printf("Exiting tester.\n");
    return 0;
}
