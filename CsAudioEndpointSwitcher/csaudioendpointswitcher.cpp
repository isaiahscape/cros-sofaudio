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
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <winioctl.h>
#include <mmreg.h>
#include <setupapi.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
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
static BOOL  g_HideHeadphoneWhenIdle   = FALSE;
static BOOL  g_HideHeadsetMicWhenIdle  = FALSE;
static ULONGLONG g_HideHeadphoneAfter  = 0;
static ULONGLONG g_HideHeadsetMicAfter = 0;
static CSAUDIO_JACK_STATE g_LastRoutingState = CSAUDIO_JACK_UNPLUGGED;
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
        // Clear a flow only after its enumeration succeeds. A transient Core
        // Audio failure must not erase otherwise valid cached endpoint IDs.
        g_SpeakerId[0] = g_HeadphoneId[0] = L'\0';
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
                        if (pId && IsOurDevice(pv.pwszVal)) {
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
        g_MicArrayId[0] = g_HeadsetMicId[0] = L'\0';
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
                        if (pId && IsOurDevice(pv.pwszVal)) {
                            if (wcsstr(pv.pwszVal, L"Headset")) {
                                StringCchCopyW(g_HeadsetMicId, ARRAYSIZE(g_HeadsetMicId), pId);
                                LogMsg(L"[SWITCHER] Found Headset Mic endpoint: %s (%s)", pv.pwszVal, pId);
                            } else if (wcsstr(pv.pwszVal, L"Internal") ||
                                       wcsstr(pv.pwszVal, L"Array")) {
                                StringCchCopyW(g_MicArrayId, ARRAYSIZE(g_MicArrayId), pId);
                                LogMsg(L"[SWITCHER] Found Mic Array endpoint: %s (%s)", pv.pwszVal, pId);
                            } else if (wcsstr(pv.pwszVal, L"Microphone")) {
                                // Windows can collapse the jack endpoint name to
                                // just "Microphone (...)"; it is the remaining
                                // non-array capture endpoint.
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

static BOOL SetEndpointVisible(IPolicyConfig* pConfig, PCWSTR deviceId, BOOL visible, PCWSTR label) {
    if (!deviceId || !deviceId[0]) return FALSE;

    HRESULT hr = pConfig->SetEndpointVisibility(deviceId, visible);
    if (FAILED(hr)) {
        LogMsg(L"[SWITCHER] ERROR: SetEndpointVisibility(%s, %s) failed (0x%08X)",
            label, visible ? L"visible" : L"hidden", hr);
        return FALSE;
    }
    return TRUE;
}

static PCWSTR RoleName(ERole role) {
    switch (role) {
    case eConsole:        return L"Console";
    case eMultimedia:     return L"Multimedia";
    case eCommunications: return L"Communications";
    default:              return L"Unknown";
    }
}

static BOOL SetEndpointAsDefault(IPolicyConfig* pConfig, PCWSTR deviceId, PCWSTR label) {
    if (!deviceId || !deviceId[0]) return FALSE;

    // Move the communications role first. Discord and other VoIP clients watch
    // that role and can otherwise reopen the endpoint that is about to go away.
    const ERole roles[] = { eCommunications, eMultimedia, eConsole };
    BOOL allSucceeded = TRUE;

    for (int i = 0; i < ARRAYSIZE(roles); i++) {
        HRESULT hr = E_FAIL;
        for (int attempt = 0; attempt < 3; attempt++) {
            hr = pConfig->SetDefaultEndpoint(deviceId, roles[i]);
            if (SUCCEEDED(hr)) break;
            Sleep(100);
        }
        if (FAILED(hr)) {
            LogMsg(L"[SWITCHER] ERROR: Setting %s as the %s default failed (0x%08X)",
                label, RoleName(roles[i]), hr);
            allSucceeded = FALSE;
        }
    }

    return allSucceeded;
}

/*
 * Hiding an endpoint tears down every WASAPI session attached to it. That is
 * especially visible in Discord, which keeps a communications stream open.
 * Only hide a routed-away endpoint once all of its sessions have migrated.
 */
static BOOL EndpointIsIdle(PCWSTR deviceId) {
    if (!deviceId || !deviceId[0]) return FALSE;

    IMMDeviceEnumerator* pEnum = NULL;
    IMMDevice* pDevice = NULL;
    IAudioSessionManager2* pManager = NULL;
    IAudioSessionEnumerator* pSessions = NULL;
    BOOL idle = FALSE; // Be conservative if the session state cannot be queried.
    int count = 0;

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (FAILED(hr) || !pEnum) goto Cleanup;

    hr = pEnum->GetDevice(deviceId, &pDevice);
    if (FAILED(hr) || !pDevice) goto Cleanup;

    hr = pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&pManager);
    if (FAILED(hr) || !pManager) goto Cleanup;

    hr = pManager->GetSessionEnumerator(&pSessions);
    if (FAILED(hr) || !pSessions) goto Cleanup;

    if (FAILED(pSessions->GetCount(&count))) goto Cleanup;

    idle = TRUE;
    for (int i = 0; i < count; i++) {
        IAudioSessionControl* pSession = NULL;
        if (SUCCEEDED(pSessions->GetSession(i, &pSession)) && pSession) {
            AudioSessionState sessionState = AudioSessionStateInactive;
            if (SUCCEEDED(pSession->GetState(&sessionState)) && sessionState == AudioSessionStateActive) {
                idle = FALSE;
                pSession->Release();
                break;
            }
            pSession->Release();
        }
    }

Cleanup:
    if (pSessions) pSessions->Release();
    if (pManager) pManager->Release();
    if (pDevice) pDevice->Release();
    if (pEnum) pEnum->Release();
    return idle;
}

static void TryDeferredHidesLocked(IPolicyConfig* pConfig) {
    ULONGLONG now = GetTickCount64();

    if (g_HideHeadphoneWhenIdle && now >= g_HideHeadphoneAfter &&
        g_HeadphoneId[0] && EndpointIsIdle(g_HeadphoneId)) {
        if (SetEndpointVisible(pConfig, g_HeadphoneId, FALSE, L"Headphones")) {
            g_HideHeadphoneWhenIdle = FALSE;
            g_HideHeadphoneAfter = 0;
            LogMsg(L"[SWITCHER] Headphones are idle and are now hidden.");
        }
    }

    if (g_HideHeadsetMicWhenIdle && now >= g_HideHeadsetMicAfter &&
        g_HeadsetMicId[0] && EndpointIsIdle(g_HeadsetMicId)) {
        if (SetEndpointVisible(pConfig, g_HeadsetMicId, FALSE, L"Headset Microphone")) {
            g_HideHeadsetMicWhenIdle = FALSE;
            g_HideHeadsetMicAfter = 0;
            LogMsg(L"[SWITCHER] Headset microphone is idle and is now hidden.");
        }
    }
}

static void TryDeferredHides() {
    EnterCriticalSection(&g_Lock);
    if (!g_HideHeadphoneWhenIdle && !g_HideHeadsetMicWhenIdle) {
        LeaveCriticalSection(&g_Lock);
        return;
    }

    IPolicyConfig* pConfig = NULL;
    HRESULT hr = CoCreateInstance(
        CLSID_PolicyConfigClient, NULL, CLSCTX_ALL,
        IID_IPolicyConfig, (void**)&pConfig);
    if (SUCCEEDED(hr) && pConfig) {
        TryDeferredHidesLocked(pConfig);
        pConfig->Release();
    }
    LeaveCriticalSection(&g_Lock);
}

static void ApplyJackState(CSAUDIO_JACK_STATE state) {
    EnterCriticalSection(&g_Lock);

    UINT physicalState = ((UINT)state) &
        (CSAUDIO_JACK_HEADPHONES_3POLE | CSAUDIO_JACK_HEADSET_4POLE | CSAUDIO_JACK_LINE_OUT);
    if (state == CSAUDIO_JACK_UNPLUGGED) {
        g_LastRoutingState = CSAUDIO_JACK_UNPLUGGED;
    } else if (physicalState == CSAUDIO_JACK_HEADPHONES_3POLE ||
        physicalState == CSAUDIO_JACK_HEADSET_4POLE ||
        physicalState == CSAUDIO_JACK_LINE_OUT) {
        g_LastRoutingState = (CSAUDIO_JACK_STATE)physicalState;
    }
    state = g_LastRoutingState; // Ignore button-only notifications for routing.

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
        LogMsg(L"[SWITCHER] -> Jack UNPLUGGED: Routing to Speakers + Mic Array");

        BOOL renderRouted = FALSE;
        if (g_SpeakerId[0] != L'\0') {
            SetEndpointVisible(pConfig, g_SpeakerId, TRUE, L"Speakers");
            renderRouted = SetEndpointAsDefault(pConfig, g_SpeakerId, L"Speakers");
        }

        BOOL captureRouted = FALSE;
        if (g_MicArrayId[0] != L'\0') {
            SetEndpointVisible(pConfig, g_MicArrayId, TRUE, L"Internal Microphone");
            captureRouted = SetEndpointAsDefault(pConfig, g_MicArrayId, L"Internal Microphone");
        }

        // Never invalidate an active Discord/WASAPI stream. The monitor loop
        // retries these hides after active sessions have migrated.
        g_HideHeadphoneWhenIdle = renderRouted && g_HeadphoneId[0] != L'\0';
        g_HideHeadsetMicWhenIdle = captureRouted && g_HeadsetMicId[0] != L'\0';
        g_HideHeadphoneAfter = GetTickCount64() + 5000;
        g_HideHeadsetMicAfter = GetTickCount64() + 5000;
        TryDeferredHidesLocked(pConfig);
    } else {
        BOOL is4Pole = (state == CSAUDIO_JACK_HEADSET_4POLE);
        LogMsg(L"[SWITCHER] -> Jack DETECTED (%s): Routing to Headphones; Making visible",
            is4Pole ? L"4-Pole Headset with Mic" : L"3-Pole Stereo Headphones");

        g_HideHeadphoneWhenIdle = FALSE;
        g_HideHeadphoneAfter = 0;
        if (g_HeadphoneId[0] != L'\0') {
            SetEndpointVisible(pConfig, g_HeadphoneId, TRUE, L"Headphones");
            SetEndpointAsDefault(pConfig, g_HeadphoneId, L"Headphones");
        }

        if (is4Pole && g_HeadsetMicId[0] != L'\0') {
            g_HideHeadsetMicWhenIdle = FALSE;
            g_HideHeadsetMicAfter = 0;
            SetEndpointVisible(pConfig, g_HeadsetMicId, TRUE, L"Headset Microphone");
            SetEndpointAsDefault(pConfig, g_HeadsetMicId, L"Headset Microphone");
        } else {
            BOOL captureRouted = FALSE;
            if (g_MicArrayId[0] != L'\0') {
                SetEndpointVisible(pConfig, g_MicArrayId, TRUE, L"Internal Microphone");
                captureRouted = SetEndpointAsDefault(pConfig, g_MicArrayId, L"Internal Microphone");
            }
            g_HideHeadsetMicWhenIdle = captureRouted && g_HeadsetMicId[0] != L'\0';
            g_HideHeadsetMicAfter = GetTickCount64() + 5000;
            TryDeferredHidesLocked(pConfig);
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

static BOOL StopRequested() {
    return WaitForSingleObject(g_StopEvent, 0) == WAIT_OBJECT_0;
}

static void CloseNau8825Device() {
    if (g_Nau8825Device != INVALID_HANDLE_VALUE) {
        CloseHandle(g_Nau8825Device);
        g_Nau8825Device = INVALID_HANDLE_VALUE;
    }
}

static BOOL ConnectNau8825Device() {
    DWORD attempts = 0;
    while (!StopRequested()) {
        g_Nau8825Device = OpenNau8825Device();
        if (g_Nau8825Device != INVALID_HANDLE_VALUE) return TRUE;

        // Avoid growing the persistent log every three seconds while drivers
        // are still being installed or the codec is temporarily unavailable.
        if ((attempts++ % 10) == 0) {
            LogMsg(L"[SWITCHER] Waiting for NAU8825 driver interface to arrive...");
        }
        if (WaitForSingleObject(g_StopEvent, 3000) == WAIT_OBJECT_0) return FALSE;
    }
    return FALSE;
}

static BOOL WaitForPendingIo(
    HANDLE device,
    OVERLAPPED* overlapped,
    DWORD* bytesReturned,
    BOOL runMaintenance,
    BOOL* stopped
) {
    HANDLE waitHandles[2] = { g_StopEvent, overlapped->hEvent };
    *stopped = FALSE;

    for (;;) {
        DWORD timeout = INFINITE;
        if (runMaintenance && (g_HideHeadphoneWhenIdle || g_HideHeadsetMicWhenIdle)) {
            timeout = 2000;
        }
        DWORD waitResult = WaitForMultipleObjects(
            ARRAYSIZE(waitHandles), waitHandles, FALSE, timeout);

        if (waitResult == WAIT_OBJECT_0) {
            *stopped = TRUE;
            CancelIoEx(device, overlapped);
            // The caller's output buffer must remain alive until cancellation
            // has completed, so drain the OVERLAPPED before returning.
            GetOverlappedResult(device, overlapped, bytesReturned, TRUE);
            return FALSE;
        }
        if (waitResult == WAIT_OBJECT_0 + 1) {
            return GetOverlappedResult(device, overlapped, bytesReturned, FALSE);
        }
        if (waitResult == WAIT_TIMEOUT && runMaintenance) {
            TryDeferredHides();
            continue;
        }

        LogMsg(L"[SWITCHER] ERROR: Waiting for codec I/O failed (%lu)", GetLastError());
        CancelIoEx(device, overlapped);
        GetOverlappedResult(device, overlapped, bytesReturned, TRUE);
        return FALSE;
    }
}

static BOOL QueryJackState(CSAUDIO_JACK_STATE* state, BOOL* stopped) {
    *stopped = FALSE;
    DWORD bytesReturned = 0;
    OVERLAPPED overlapped = { 0 };
    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!overlapped.hEvent) return FALSE;

    BOOL success = DeviceIoControl(
        g_Nau8825Device,
        IOCTL_CSAUDIO_GET_JACK_STATE,
        NULL, 0,
        state, sizeof(*state),
        &bytesReturned,
        &overlapped);

    if (!success) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            success = WaitForPendingIo(
                g_Nau8825Device, &overlapped, &bytesReturned, FALSE, stopped);
        } else {
            LogMsg(L"[SWITCHER] IOCTL_CSAUDIO_GET_JACK_STATE failed: %lu", error);
        }
    }

    CloseHandle(overlapped.hEvent);
    return success && bytesReturned == sizeof(*state);
}

static BOOL WaitForJackNotification(CSAUDIO_JACK_STATE* state, BOOL* stopped) {
    *stopped = FALSE;
    DWORD bytesReturned = 0;
    OVERLAPPED overlapped = { 0 };
    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!overlapped.hEvent) return FALSE;

    BOOL success = DeviceIoControl(
        g_Nau8825Device,
        IOCTL_CSAUDIO_REGISTER_JACK_NOTIFICATION,
        NULL, 0,
        state, sizeof(*state),
        &bytesReturned,
        &overlapped);

    if (!success) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            success = WaitForPendingIo(
                g_Nau8825Device, &overlapped, &bytesReturned, TRUE, stopped);
            if (!success && !*stopped) {
                LogMsg(L"[SWITCHER] Jack notification completed with error: %lu", GetLastError());
            }
        } else {
            LogMsg(L"[SWITCHER] IOCTL_CSAUDIO_REGISTER_JACK_NOTIFICATION failed: %lu", error);
        }
    }

    CloseHandle(overlapped.hEvent);
    return success && bytesReturned == sizeof(*state);
}

static PCWSTR JackStateName(CSAUDIO_JACK_STATE state) {
    switch (state) {
    case CSAUDIO_JACK_UNPLUGGED:        return L"UNPLUGGED";
    case CSAUDIO_JACK_HEADPHONES_3POLE: return L"3-POLE HEADPHONES";
    case CSAUDIO_JACK_HEADSET_4POLE:    return L"4-POLE HEADSET";
    case CSAUDIO_JACK_LINE_OUT:         return L"LINE OUT";
    default:                            return L"BUTTON/UNKNOWN";
    }
}

static DWORD WINAPI MonitorWorkerThread(LPVOID lpParam) {
    UNREFERENCED_PARAMETER(lpParam);

    HRESULT coResult = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(coResult)) {
        LogMsg(L"[SWITCHER] ERROR: COM initialization failed (0x%08X)", coResult);
        SetEvent(g_StopEvent);
        return (DWORD)coResult;
    }

    LogMsg(L"[SWITCHER] Monitor worker thread started.");
    RefreshEndpoints();

    // Reconnect forever. A codec power/PnP transition must not leave the SCM
    // service marked running while its only worker has already exited.
    while (!StopRequested()) {
        if (!ConnectNau8825Device()) break;

        BOOL stopped = FALSE;
        CSAUDIO_JACK_STATE currentState = CSAUDIO_JACK_UNPLUGGED;
        if (!QueryJackState(&currentState, &stopped)) {
            CloseNau8825Device();
            if (stopped) break;
            WaitForSingleObject(g_StopEvent, 1000);
            continue;
        }

        LogMsg(L"[SWITCHER] Initial NAU8825 Jack State: 0x%02X (%s)",
            currentState, JackStateName(currentState));

        // At boot, endpoint creation can lag the codec interface.
        for (int i = 0; i < 30 && g_SpeakerId[0] == L'\0' && !StopRequested(); i++) {
            if (WaitForSingleObject(g_StopEvent, 2000) == WAIT_OBJECT_0) break;
            RefreshEndpoints();
        }
        if (StopRequested()) {
            CloseNau8825Device();
            break;
        }

        ApplyJackState(currentState);

        while (!StopRequested()) {
            CSAUDIO_JACK_STATE nextState = CSAUDIO_JACK_UNPLUGGED;
            stopped = FALSE;
            if (!WaitForJackNotification(&nextState, &stopped)) {
                if (!stopped) {
                    LogMsg(L"[SWITCHER] Codec notification channel lost; re-enumerating.");
                }
                break;
            }

            LogMsg(L"[SWITCHER] >>> JACK EVENT RECEIVED! New State: 0x%02X (%s)",
                nextState, JackStateName(nextState));
            RefreshEndpoints();
            ApplyJackState(nextState);
        }

        CloseNau8825Device();
        if (!StopRequested()) WaitForSingleObject(g_StopEvent, 1000);
    }

    CloseNau8825Device();
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
        if (g_StopEvent) SetEvent(g_StopEvent);
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
    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 3000;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    InitializeCriticalSection(&g_Lock);
    g_StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_StopEvent) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        DeleteCriticalSection(&g_Lock);
        return;
    }

    HANDLE hThread = CreateThread(NULL, 0, MonitorWorkerThread, NULL, 0, NULL);
    if (!hThread) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        CloseHandle(g_StopEvent);
        g_StopEvent = NULL;
        DeleteCriticalSection(&g_Lock);
        return;
    }

    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    WaitForSingleObject(g_StopEvent, INFINITE);

    // The worker cancels its outstanding overlapped request when this event is
    // set. Wait for it before destroying the lock or event it still references.
    WaitForSingleObject(hThread, INFINITE);
    DWORD workerExitCode = 0;
    if (GetExitCodeThread(hThread, &workerExitCode) && workerExitCode != 0) {
        g_ServiceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        g_ServiceStatus.dwServiceSpecificExitCode = workerExitCode;
    }
    CloseHandle(hThread);

    CloseHandle(g_StopEvent);
    g_StopEvent = NULL;
    DeleteCriticalSection(&g_Lock);

    g_ServiceStatus.dwControlsAccepted = 0;
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
        if (!g_StopEvent) {
            DeleteCriticalSection(&g_Lock);
            return (int)GetLastError();
        }

        HANDLE hThread = CreateThread(NULL, 0, MonitorWorkerThread, NULL, 0, NULL);
        if (!hThread) {
            DWORD error = GetLastError();
            CloseHandle(g_StopEvent);
            g_StopEvent = NULL;
            DeleteCriticalSection(&g_Lock);
            return (int)error;
        }

        wprintf(L"Running. Press ENTER to stop...\n");
        getchar();

        SetEvent(g_StopEvent);
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);

        CloseHandle(g_StopEvent);
        g_StopEvent = NULL;
        DeleteCriticalSection(&g_Lock);
        return 0;
    }

    // Windows Service Mode
    SERVICE_TABLE_ENTRYW ServiceTable[] = {
        { (LPWSTR)SERVICE_NAME, (LPSERVICE_MAIN_FUNCTIONW)ServiceMain },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcherW(ServiceTable)) {
        return (int)GetLastError();
    }
    return 0;
}
