#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <tlhelp32.h>
#include <strsafe.h>
#include <vector>
#include <string>

// Generated RPC header (produced by MIDL from TrayService.idl)
#include "TrayService_h.h"

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "advapi32.lib")

#define SERVICE_NAME    L"TrayService"
#define TRAY_APP_EXE    L"TrayApp.exe"

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static SERVICE_STATUS        g_status       = {};
static SERVICE_STATUS_HANDLE g_statusHandle = NULL;

static CRITICAL_SECTION      g_lock;
static std::vector<DWORD>    g_pids;   // PIDs of TrayApp processes we launched

static HANDLE g_rpcStopEvent = NULL;   // signalled by StopService() RPC call

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::wstring GetServiceDir()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    return path;
}

static void SetStatus(DWORD state, DWORD exitCode = NO_ERROR)
{
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING)
        g_status.dwWaitHint = 5000;
    else
        g_status.dwWaitHint = 0;
    SetServiceStatus(g_statusHandle, &g_status);
}

// ---------------------------------------------------------------------------
// Launch TrayApp.exe inside a given user session
// ---------------------------------------------------------------------------
static void LaunchTrayAppInSession(DWORD sessionId)
{
    HANDLE hToken = NULL;
    if (!WTSQueryUserToken(sessionId, &hToken))
        return;

    HANDLE hDupToken = NULL;
    if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL,
                          SecurityIdentification, TokenPrimary, &hDupToken))
    {
        CloseHandle(hToken);
        return;
    }
    CloseHandle(hToken);

    // Build environment block for this user
    LPVOID pEnv = NULL;
    CreateEnvironmentBlock(&pEnv, hDupToken, FALSE);

    std::wstring exePath = GetServiceDir() + TRAY_APP_EXE;
    std::wstring cmdLine = L"\"" + exePath + L"\" --hidden";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;

    BOOL ok = CreateProcessAsUserW(
        hDupToken,
        exePath.c_str(),
        cmdLine.data(),
        NULL, NULL,
        FALSE,
        flags,
        pEnv,
        NULL,
        &si,
        &pi);

    if (pEnv) DestroyEnvironmentBlock(pEnv);
    CloseHandle(hDupToken);

    if (ok)
    {
        EnterCriticalSection(&g_lock);
        g_pids.push_back(pi.dwProcessId);
        LeaveCriticalSection(&g_lock);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// ---------------------------------------------------------------------------
// Enumerate all active non-session-0 terminal sessions and launch TrayApp
// ---------------------------------------------------------------------------
static void EnumerateAndLaunchAll()
{
    PWTS_SESSION_INFOW pSessions = NULL;
    DWORD count = 0;

    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessions, &count))
        return;

    for (DWORD i = 0; i < count; i++)
    {
        if (pSessions[i].SessionId == 0)
            continue;
        if (pSessions[i].State == WTSActive || pSessions[i].State == WTSConnected)
            LaunchTrayAppInSession(pSessions[i].SessionId);
    }

    WTSFreeMemory(pSessions);
}

// ---------------------------------------------------------------------------
// Terminate all TrayApp processes we launched
// ---------------------------------------------------------------------------
static void TerminateLaunchedProcesses()
{
    EnterCriticalSection(&g_lock);
    std::vector<DWORD> pids = g_pids;
    g_pids.clear();
    LeaveCriticalSection(&g_lock);

    for (DWORD pid : pids)
    {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h)
        {
            TerminateProcess(h, 0);
            CloseHandle(h);
        }
    }
}

// ---------------------------------------------------------------------------
// RPC server thread
// ---------------------------------------------------------------------------
static DWORD WINAPI RpcServerThread(LPVOID)
{
    RPC_STATUS status;

    status = RpcServerUseProtseqEpW(
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
        RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"TrayServiceEndpoint")),
        NULL);
    if (status != RPC_S_OK)
    {
        SetEvent(g_rpcStopEvent);
        return status;
    }

    status = RpcServerRegisterIf(ITrayService_v1_0_s_ifspec, NULL, NULL);
    if (status != RPC_S_OK)
    {
        SetEvent(g_rpcStopEvent);
        return status;
    }

    // Start listening asynchronously (TRUE = don't wait)
    status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE);
    if (status != RPC_S_OK)
    {
        SetEvent(g_rpcStopEvent);
        return status;
    }

    // Block here until StopService() RPC call signals the event
    WaitForSingleObject(g_rpcStopEvent, INFINITE);

    RpcMgmtStopServerListening(NULL);
    RpcServerUnregisterIf(ITrayService_v1_0_s_ifspec, NULL, FALSE);

    return 0;
}

// ---------------------------------------------------------------------------
// RPC interface implementation — called by generated server stub (C linkage)
// ---------------------------------------------------------------------------
extern "C" void StopService(handle_t /*hBinding*/)
{
    SetEvent(g_rpcStopEvent);
}

// ---------------------------------------------------------------------------
// Service control handler
// ---------------------------------------------------------------------------
static DWORD WINAPI ServiceCtrlHandler(DWORD control, DWORD eventType,
                                        LPVOID eventData, LPVOID /*ctx*/)
{
    switch (control)
    {
    case SERVICE_CONTROL_INTERROGATE:
        SetStatus(g_status.dwCurrentState);
        return NO_ERROR;

    case SERVICE_CONTROL_SESSIONCHANGE:
        if (eventType == WTS_SESSION_LOGON)
        {
            auto* pInfo = reinterpret_cast<WTSSESSION_NOTIFICATION*>(eventData);
            if (pInfo && pInfo->dwSessionId != 0)
                LaunchTrayAppInSession(pInfo->dwSessionId);
        }
        return NO_ERROR;

    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        // Explicitly disabled — service stops only via RPC StopService()
        return NO_ERROR;

    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// ---------------------------------------------------------------------------
// ServiceMain
// ---------------------------------------------------------------------------
static void WINAPI ServiceMain(DWORD /*argc*/, LPWSTR* /*argv*/)
{
    InitializeCriticalSection(&g_lock);

    g_rpcStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    g_status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState            = SERVICE_START_PENDING;
    g_status.dwControlsAccepted        = SERVICE_ACCEPT_SESSIONCHANGE;
    g_status.dwWin32ExitCode           = NO_ERROR;
    g_status.dwServiceSpecificExitCode = 0;
    g_status.dwCheckPoint              = 0;
    g_status.dwWaitHint                = 5000;

    g_statusHandle = RegisterServiceCtrlHandlerExW(SERVICE_NAME, ServiceCtrlHandler, NULL);
    if (!g_statusHandle)
        return;

    SetStatus(SERVICE_START_PENDING);

    // Launch existing sessions
    EnumerateAndLaunchAll();

    SetStatus(SERVICE_RUNNING);

    // Run the RPC server on this thread (blocks until StopService is called)
    RpcServerThread(NULL);

    // Cleanup
    SetStatus(SERVICE_STOP_PENDING);
    TerminateLaunchedProcesses();

    if (g_rpcStopEvent) CloseHandle(g_rpcStopEvent);
    DeleteCriticalSection(&g_lock);

    SetStatus(SERVICE_STOPPED);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int wmain()
{
    SERVICE_TABLE_ENTRYW table[] =
    {
        { const_cast<LPWSTR>(SERVICE_NAME), ServiceMain },
        { NULL, NULL }
    };
    StartServiceCtrlDispatcherW(table);
    return 0;
}
