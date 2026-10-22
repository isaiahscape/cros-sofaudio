/*
 * csaudioendpointswitcher.cpp — Chromebook Audio Endpoint Switcher Service
 *
 * Automatically monitors NAU88L25 3.5mm jack insertion/removal events and
 * switches Windows 11 default endpoints and endpoint visibility.
 *
 * Replaces legacy CoolStar HID-polling switcher with zero-overhead native
 * IOCTL notifications over GUID_DEVINTERFACE_NAU8825_CODEC.
 *
 * Copyright (c) 2026. Licensed under the MIT License.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <mmreg.h>
#include <setupapi.h>
#include <mmdeviceapi.h>
#include <initguid.h>                       /* must precede the PKEY header so the key is defined here */
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>

#include "..\Common\csaudio_codec.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

#define SERVICE_NAME TEXT("csaudioswitcher")

/* =========================================================================
 * Undocumented Windows PolicyConfig COM Interface
 * ========================================================================= */

// {870AF99C-171D-4F9E-AF0D-E63DF40C2BC9}
DEFINE_GUID(CLSID_PolicyConfigClient,
    0x870af99c, 0x171d, 0x4f9e, 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9);

// {F8679F50-850A-41CF-9C72-430F290290C8}
DEFINE_GUID(IID_IPolicyConfig,
    0xf8679f50, 0x850a, 0x41cf, 0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8);

interface IPolicyConfig : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX *, WAVEFORMATEX *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, struct DeviceShareMode *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, struct DeviceShareMode *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole eRole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR wszDeviceId, INT isVisible) = 0;
};

/* =========================================================================
 * Global State & Context
 * ========================================================================= */

static SERVICE_STATUS        g_ServiceStatus;
static SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
static HANDLE                g_StopEvent = NULL;
static HANDLE                g_Nau8825Device = INVALID_HANDLE_VALUE;
static BOOL                  g_IsConsoleMode = FALSE;

// Cached Endpoint Device IDs
static WCHAR g_SpeakerId[MAX_PATH]     = { 0 };
static WCHAR g_HeadphoneId[MAX_PATH]   = { 0 };
static WCHAR g_MicArrayId[MAX_PATH]    = { 0 };
static WCHAR g_HeadsetMicId[MAX_PATH]  = { 0 };
static CRITICAL_SECTION g_Lock;

static void LogMsg(const WCHAR* fmt, ...) {
    WCHAR buf[1024];
    va_list args;
    va_start(args, fmt);
    StringCchVPrintfW(buf, ARRAYSIZE(buf), fmt, args);
    va_end(args);

    OutputDebugStringW(buf);

    /* also append to C:\\ProgramData\\CrosSofAudio\\switcher.log */
    {
        WCHAR path[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"ProgramData", path, MAX_PATH);
        if (n > 0 && n < MAX_PATH - 40) {
            StringCchCatW(path, MAX_PATH, L"\\CrosSofAudio");
            CreateDirectoryW(path, NULL);
            StringCchCatW(path, MAX_PATH, L"\\switcher.log");
            HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                SYSTEMTIME st; GetLocalTime(&st);
                char line[1200];
                int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u %ls\r\n",
                                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, buf);
                if (len > 0) { DWORD w; WriteFile(h, line, (DWORD)len, &w, NULL); }
                CloseHandle(h);
            }
        }
    }

    if (g_IsConsoleMode) {
        wprintf(L"%s\n", buf);
        fflush(stdout);
    }
}

/* =========================================================================
 * Endpoint Discovery & Management
 * ========================================================================= */

static BOOL IsOurDevice(const WCHAR* name) {
    if (!name) return FALSE;
    return (wcsstr(name, L"SOF Audio") != NULL ||
            wcsstr(name, L"Chromebook") != NULL ||
            wcsstr(name, L"CoolStar") != NULL);
}

static void RefreshEndpoints() {
    EnterCriticalSection(&g_Lock);
    g_SpeakerId[0] = g_HeadphoneId[0] = g_MicArrayId[0] = g_HeadsetMicId[0] = L'\0';

    IMMDeviceEnumerator* pEnum = NULL;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        NULL,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)&pEnum
    );

    if (FAILED(hr) || !pEnum) {
        LeaveCriticalSection(&g_Lock);
        return;
    }

    // 1. Scan Render Endpoints (Speakers vs Headphones)
    IMMDeviceCollection* pRenderCol = NULL;
    if (SUCCEEDED(pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_UNPLUGGED, &pRenderCol)) && pRenderCol) {
        UINT count = 0;
        pRenderCol->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* pDev = NULL;
            if (SUCCEEDED(pRenderCol->Item(i, &pDev)) && pDev) {
                LPWSTR pId = NULL;
                pDev->GetId(&pId);
                IPropertyStore* pProps = NULL;
                pDev->OpenPropertyStore(STGM_READ, &pProps);
                if (pProps) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.pwszVal) {
                        if (IsOurDevice(pv.pwszVal)) {
                            if (wcsstr(pv.pwszVal, L"Headphone")) {
                                StringCchCopyW(g_HeadphoneId, ARRAYSIZE(g_HeadphoneId), pId);
                                LogMsg(L"[SWITCHER] Found Headphones endpoint: %s (%s)", pv.pwszVal, pId);
                            } else if (wcsstr(pv.pwszVal, L"Speaker")) {
                                StringCchCopyW(g_SpeakerId, ARRAYSIZE(g_SpeakerId), pId);
                                LogMsg(L"[SWITCHER] Found Speakers endpoint: %s (%s)", pv.pwszVal, pId);
                            }
                        }
                    }
                    PropVariantClear(&pv);
                    pProps->Release();
                }
                if (pId) CoTaskMemFree(pId);
                pDev->Release();
            }
        }
        pRenderCol->Release();
    }

    // 2. Scan Capture Endpoints (Mic Array vs Headset Mic)
    IMMDeviceCollection* pCaptureCol = NULL;
    if (SUCCEEDED(pEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_UNPLUGGED, &pCaptureCol)) && pCaptureCol) {
        UINT count = 0;
        pCaptureCol->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* pDev = NULL;
            if (SUCCEEDED(pCaptureCol->Item(i, &pDev)) && pDev) {
                LPWSTR pId = NULL;
                pDev->GetId(&pId);
                IPropertyStore* pProps = NULL;
                pDev->OpenPropertyStore(STGM_READ, &pProps);
                if (pProps) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.pwszVal) {
                        if (IsOurDevice(pv.pwszVal)) {
                            if (wcsstr(pv.pwszVal, L"Array")) {
                                StringCchCopyW(g_MicArrayId, ARRAYSIZE(g_MicArrayId), pId);
                                LogMsg(L"[SWITCHER] Found Mic Array endpoint: %s (%s)", pv.pwszVal, pId);
                            } else if (wcsstr(pv.pwszVal, L"Microphone")) {
                                StringCchCopyW(g_HeadsetMicId, ARRAYSIZE(g_HeadsetMicId), pId);
                                LogMsg(L"[SWITCHER] Found Headset Mic endpoint: %s (%s)", pv.pwszVal, pId);
                            }
                        }
                    }
                    PropVariantClear(&pv);
                    pProps->Release();
                }
                if (pId) CoTaskMemFree(pId);
                pDev->Release();
            }
        }
        pCaptureCol->Release();
    }

    pEnum->Release();
    LeaveCriticalSection(&g_Lock);
}

/* =========================================================================
 * Jack State Application Logic
 * ========================================================================= */

static void ApplyJackState(CSAUDIO_JACK_STATE state) {
    EnterCriticalSection(&g_Lock);

    IPolicyConfig* pConfig = NULL;
    HRESULT hr = CoCreateInstance(
        CLSID_PolicyConfigClient,
        NULL,
        CLSCTX_ALL,
        IID_IPolicyConfig,
        (void**)&pConfig
    );

    if (FAILED(hr) || !pConfig) {
        LogMsg(L"[SWITCHER] ERROR: Failed to instantiate IPolicyConfig (0x%08X)", hr);
        LeaveCriticalSection(&g_Lock);
        return;
    }

    if (state == CSAUDIO_JACK_UNPLUGGED) {
        LogMsg(L"[SWITCHER] -> Jack UNPLUGGED: Routing to Speakers + Mic Array; Hiding Headphones");

        // 1. Hide Headphones endpoint so it does not appear in Windows volume flyout
        if (g_HeadphoneId[0] != L'\0') {
            pConfig->SetEndpointVisibility(g_HeadphoneId, FALSE);
        }

        // 2. Set default playback endpoint to Speakers
        if (g_SpeakerId[0] != L'\0') {
            pConfig->SetEndpointVisibility(g_SpeakerId, TRUE);
            pConfig->SetDefaultEndpoint(g_SpeakerId, eConsole);
            pConfig->SetDefaultEndpoint(g_SpeakerId, eMultimedia);
            pConfig->SetDefaultEndpoint(g_SpeakerId, eCommunications);
        }

        // 3. Hide Headset Mic endpoint
        if (g_HeadsetMicId[0] != L'\0') {
            pConfig->SetEndpointVisibility(g_HeadsetMicId, FALSE);
        }

        // 4. Set default capture endpoint to Microphone Array
        if (g_MicArrayId[0] != L'\0') {
            pConfig->SetEndpointVisibility(g_MicArrayId, TRUE);
            pConfig->SetDefaultEndpoint(g_MicArrayId, eConsole);
            pConfig->SetDefaultEndpoint(g_MicArrayId, eMultimedia);
            pConfig->SetDefaultEndpoint(g_MicArrayId, eCommunications);
        }
    } else {
        BOOL is4Pole = (state == CSAUDIO_JACK_HEADSET_4POLE);
        LogMsg(L"[SWITCHER] -> Jack DETECTED (%s): Routing to Headphones; Making visible",
            is4Pole ? L"4-Pole Headset with Mic" : L"3-Pole Stereo Headphones");

        // 1. Make Headphones endpoint visible in Windows volume flyout
        if (g_HeadphoneId[0] != L'\0') {
            pConfig->SetEndpointVisibility(g_HeadphoneId, TRUE);
            // 2. Set default playback endpoint to Headphones
            pConfig->SetDefaultEndpoint(g_HeadphoneId, eConsole);
            pConfig->SetDefaultEndpoint(g_HeadphoneId, eMultimedia);
            pConfig->SetDefaultEndpoint(g_HeadphoneId, eCommunications);
        }

        if (is4Pole && g_HeadsetMicId[0] != L'\0') {
            // 3. Make Headset Mic visible and set default capture
            pConfig->SetEndpointVisibility(g_HeadsetMicId, TRUE);
            pConfig->SetDefaultEndpoint(g_HeadsetMicId, eConsole);
            pConfig->SetDefaultEndpoint(g_HeadsetMicId, eMultimedia);
            pConfig->SetDefaultEndpoint(g_HeadsetMicId, eCommunications);
        } else {
            // Standard TRS 3-pole has no mic, keep Mic Array default and Headset Mic hidden
            if (g_HeadsetMicId[0] != L'\0') {
                pConfig->SetEndpointVisibility(g_HeadsetMicId, FALSE);
            }
            if (g_MicArrayId[0] != L'\0') {
                pConfig->SetEndpointVisibility(g_MicArrayId, TRUE);
                pConfig->SetDefaultEndpoint(g_MicArrayId, eConsole);
                pConfig->SetDefaultEndpoint(g_MicArrayId, eMultimedia);
                pConfig->SetDefaultEndpoint(g_MicArrayId, eCommunications);
            }
        }
    }

    pConfig->Release();
    LeaveCriticalSection(&g_Lock);
}

/* =========================================================================
 * Hardware Monitor Worker Thread
 * ========================================================================= */

static HANDLE OpenNau8825Device() {
    HDEVINFO hDevInfo = SetupDiGetClassDevs(
        &GUID_DEVINTERFACE_NAU8825_CODEC,
        NULL,
        NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    );

    if (hDevInfo == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    SP_DEVICE_INTERFACE_DATA ifData;
    ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    HANDLE hDevice = INVALID_HANDLE_VALUE;

    if (SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &GUID_DEVINTERFACE_NAU8825_CODEC, 0, &ifData)) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetail(hDevInfo, &ifData, NULL, 0, &reqSize, NULL);
        PSP_DEVICE_INTERFACE_DETAIL_DATA detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(reqSize);
        if (detail) {
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
            if (SetupDiGetDeviceInterfaceDetail(hDevInfo, &ifData, detail, reqSize, NULL, NULL)) {
                LogMsg(L"[SWITCHER] Connecting to NAU8825 device: %s", detail->DevicePath);
                hDevice = CreateFile(
                    detail->DevicePath,
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL,
                    OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED,
                    NULL
                );
            }
            free(detail);
        }
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return hDevice;
}

static DWORD WINAPI MonitorWorkerThread(LPVOID lpParam) {
    UNREFERENCED_PARAMETER(lpParam);

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    LogMsg(L"[SWITCHER] Monitor worker thread started.");

    // Initial discovery of endpoints
    RefreshEndpoints();

    // Loop until NAU8825 device is discovered (waiting quietly without busy spin)
    while (WaitForSingleObject(g_StopEvent, 0) != WAIT_OBJECT_0) {
        g_Nau8825Device = OpenNau8825Device();
        if (g_Nau8825Device != INVALID_HANDLE_VALUE) {
            break;
        }
        LogMsg(L"[SWITCHER] Waiting for NAU8825 driver interface to arrive...");
        if (WaitForSingleObject(g_StopEvent, 3000) == WAIT_OBJECT_0) {
            CoUninitialize();
            return 0;
        }
    }

    // Query initial jack state
    CSAUDIO_JACK_STATE currentState = CSAUDIO_JACK_UNPLUGGED;
    DWORD bytesReturned = 0;
    OVERLAPPED ovInitial = { 0 };
    ovInitial.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (DeviceIoControl(
        g_Nau8825Device,
        IOCTL_CSAUDIO_GET_JACK_STATE,
        NULL, 0,
        &currentState, sizeof(currentState),
        &bytesReturned,
        &ovInitial
    ) || GetLastError() == ERROR_IO_PENDING) {
        GetOverlappedResult(g_Nau8825Device, &ovInitial, &bytesReturned, TRUE);
    }
    CloseHandle(ovInitial.hEvent);

    LogMsg(L"[SWITCHER] Initial NAU8825 Jack State: 0x%02X (%s)",
        currentState,
        currentState == CSAUDIO_JACK_UNPLUGGED ? L"UNPLUGGED" :
        (currentState == CSAUDIO_JACK_HEADPHONES_3POLE ? L"3-POLE HEADPHONES" : L"4-POLE HEADSET"));

    // At boot the audio endpoints can appear after this service starts:
    // wait (up to ~60 s) until our Speakers endpoint is known.
    for (int i = 0; i < 30 && g_SpeakerId[0] == L'\0'; i++) {
        if (WaitForSingleObject(g_StopEvent, 2000) == WAIT_OBJECT_0) break;
        RefreshEndpoints();
    }

    ApplyJackState(currentState);

    // Continuous async notification loop
    OVERLAPPED ovNotify = { 0 };
    ovNotify.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    HANDLE waitHandles[2] = { g_StopEvent, ovNotify.hEvent };

    while (TRUE) {
        ResetEvent(ovNotify.hEvent);
        CSAUDIO_JACK_STATE nextState = CSAUDIO_JACK_UNPLUGGED;

        BOOL ok = DeviceIoControl(
            g_Nau8825Device,
            IOCTL_CSAUDIO_REGISTER_JACK_NOTIFICATION,
            NULL, 0,
            &nextState, sizeof(nextState),
            &bytesReturned,
            &ovNotify
        );

        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            LogMsg(L"[SWITCHER] IOCTL_CSAUDIO_REGISTER_JACK_NOTIFICATION failed: %lu. Re-enumerating...", GetLastError());
            CloseHandle(g_Nau8825Device);
            g_Nau8825Device = INVALID_HANDLE_VALUE;

            while (WaitForSingleObject(g_StopEvent, 3000) != WAIT_OBJECT_0) {
                g_Nau8825Device = OpenNau8825Device();
                if (g_Nau8825Device != INVALID_HANDLE_VALUE) break;
            }
            if (g_Nau8825Device == INVALID_HANDLE_VALUE) break;
            continue;
        }

        // Wait for jack notification event or stop request
        DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (waitRes == WAIT_OBJECT_0) {
            // g_StopEvent signaled
            CancelIo(g_Nau8825Device);
            break;
        } else if (waitRes == WAIT_OBJECT_0 + 1) {
            // Notification received!
            if (GetOverlappedResult(g_Nau8825Device, &ovNotify, &bytesReturned, FALSE)) {
                LogMsg(L"[SWITCHER] >>> JACK EVENT RECEIVED! New State: 0x%02X", nextState);
                // In case devices were recently reconfigured, refresh cached IDs
                RefreshEndpoints();
                ApplyJackState(nextState);
            }
        }
    }

    CloseHandle(ovNotify.hEvent);

    if (g_Nau8825Device != INVALID_HANDLE_VALUE) {
        CloseHandle(g_Nau8825Device);
        g_Nau8825Device = INVALID_HANDLE_VALUE;
    }

    LogMsg(L"[SWITCHER] Monitor worker thread exiting.");
    CoUninitialize();
    return 0;
}

/* =========================================================================
 * Service Control Implementation
 * ========================================================================= */

VOID WINAPI ServiceCtrlHandler(DWORD request) {
    switch (request) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        SetEvent(g_StopEvent);
        break;
    default:
        break;
    }
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

VOID WINAPI ServiceMain(DWORD argc, LPTSTR* argv) {
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    g_StatusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);
    if (!g_StatusHandle) return;

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 3000;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    InitializeCriticalSection(&g_Lock);
    g_StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    HANDLE hThread = CreateThread(NULL, 0, MonitorWorkerThread, NULL, 0, NULL);

    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    WaitForSingleObject(g_StopEvent, INFINITE);

    if (hThread) {
        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
    }

    CloseHandle(g_StopEvent);
    DeleteCriticalSection(&g_Lock);

    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

/* =========================================================================
 * Main Entry Point (Supports Console & Service Modes)
 * ========================================================================= */

int main(int argc, char* argv[]) {
    // Check if user requested console / interactive execution
    for (int i = 1; i < argc; i++) {
        if (_stricmp(argv[i], "--console") == 0 ||
            _stricmp(argv[i], "-c") == 0 ||
            _stricmp(argv[i], "/c") == 0) {
            g_IsConsoleMode = TRUE;
            break;
        }
    }

    // Also auto-detect if attached to a console
    if (!g_IsConsoleMode) {
        DWORD consoleProcessList[2];
        if (GetConsoleProcessList(consoleProcessList, 2) > 1) {
            g_IsConsoleMode = TRUE;
        }
    }

    if (g_IsConsoleMode) {
        wprintf(L"==========================================================\n");
        wprintf(L"  Chromebook SOF Audio Endpoint Switcher (Interactive Mode)\n");
        wprintf(L"==========================================================\n");

        InitializeCriticalSection(&g_Lock);
        g_StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

        HANDLE hThread = CreateThread(NULL, 0, MonitorWorkerThread, NULL, 0, NULL);

        wprintf(L"Running. Press ENTER to stop...\n");
        getchar();

        SetEvent(g_StopEvent);
        if (hThread) {
            WaitForSingleObject(hThread, 5000);
            CloseHandle(hThread);
        }

        CloseHandle(g_StopEvent);
        DeleteCriticalSection(&g_Lock);
        return 0;
    }

    // Windows Service Mode
    SERVICE_TABLE_ENTRYW ServiceTable[] = {
        { (LPWSTR)SERVICE_NAME, (LPSERVICE_MAIN_FUNCTIONW)ServiceMain },
        { NULL, NULL }
    };

    StartServiceCtrlDispatcherW(ServiceTable);
    return 0;
}
