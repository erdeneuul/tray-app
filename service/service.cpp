#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <tlhelp32.h>
#include <strsafe.h>
#include <cstdlib>
#include <vector>
#include <string>

#include "rpc_interface_h.h"

extern "C" {
    void* __RPC_USER MIDL_user_allocate(size_t len) { return malloc(len); }
    void  __RPC_USER MIDL_user_free(void* ptr)      { free(ptr); }
}

static const wchar_t* const kServiceName = L"TrayAppService";
static const wchar_t* const kTrayAppExe  = L"TrayApp.exe";
static const wchar_t* const kRpcEndpoint = L"TrayAppService";

static SERVICE_STATUS        g_status       = {};
static SERVICE_STATUS_HANDLE g_statusHandle = NULL;
static CRITICAL_SECTION      g_lock         = {};
static std::vector<DWORD>    g_pids;
static HANDLE                g_stopEvent    = NULL;

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

static void SetSvcStatus(DWORD state, DWORD exitCode = NO_ERROR)
{
    g_status.dwCurrentState  = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = (state == SERVICE_START_PENDING ||
                           state == SERVICE_STOP_PENDING) ? 5000 : 0;
    SetServiceStatus(g_statusHandle, &g_status);
}

// ---------------------------------------------------------------------------
// Launch TrayApp.exe in a user session
// ---------------------------------------------------------------------------
static void LaunchTrayAppInSession(DWORD sessionId)
{
    HANDLE hToken = NULL;
    if (!WTSQueryUserToken(sessionId, &hToken)) return;

    HANDLE hDup = NULL;
    if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL,
                          SecurityIdentification, TokenPrimary, &hDup))
    {
        CloseHandle(hToken);
        return;
    }
    CloseHandle(hToken);

    LPVOID pEnv = NULL;
    CreateEnvironmentBlock(&pEnv, hDup, FALSE);

    std::wstring exePath = GetServiceDir() + kTrayAppExe;
    std::wstring cmdLine = L"\"" + exePath + L"\" --hidden";

    STARTUPINFOW si = { sizeof(si) };
    si.lpDesktop    = const_cast<LPWSTR>(L"winsta0\\default");
    si.dwFlags      = STARTF_USESHOWWINDOW;
    si.wShowWindow  = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessAsUserW(
        hDup, exePath.c_str(), cmdLine.data(),
        NULL, NULL, FALSE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
        pEnv, NULL, &si, &pi);

    if (pEnv) DestroyEnvironmentBlock(pEnv);
    CloseHandle(hDup);

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
// Enumerate active sessions (except session 0) and launch TrayApp in each
// ---------------------------------------------------------------------------
static void EnumerateAndLaunchAll()
{
    PWTS_SESSION_INFOW pSess = NULL;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSess, &count))
        return;

    for (DWORD i = 0; i < count; i++)
    {
        if (pSess[i].SessionId == 0) continue;
        if (pSess[i].State == WTSActive || pSess[i].State == WTSConnected)
            LaunchTrayAppInSession(pSess[i].SessionId);
    }
    WTSFreeMemory(pSess);
}

// ---------------------------------------------------------------------------
// Terminate all TrayApp processes launched by this service
// ---------------------------------------------------------------------------
static void TerminateAllLaunched()
{
    EnterCriticalSection(&g_lock);
    std::vector<DWORD> pids = g_pids;
    g_pids.clear();
    LeaveCriticalSection(&g_lock);

    for (DWORD pid : pids)
    {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h) { TerminateProcess(h, 0); CloseHandle(h); }
    }
}

// ---------------------------------------------------------------------------
// RPC server-side implementation — signals ServiceMain to begin shutdown
// ---------------------------------------------------------------------------
extern "C" void StopService(handle_t /*hBinding*/)
{
    SetEvent(g_stopEvent);
}

// ---------------------------------------------------------------------------
// Service control handler
// ---------------------------------------------------------------------------
static DWORD WINAPI ServiceCtrlHandler(DWORD control, DWORD eventType,
                                        LPVOID eventData, LPVOID)
{
    switch (control)
    {
    case SERVICE_CONTROL_INTERROGATE:
        SetSvcStatus(g_status.dwCurrentState);
        return NO_ERROR;

    case SERVICE_CONTROL_SESSIONCHANGE:
        if (eventType == WTS_SESSION_LOGON)
        {
            auto* info = reinterpret_cast<WTSSESSION_NOTIFICATION*>(eventData);
            if (info && info->dwSessionId != 0)
                LaunchTrayAppInSession(info->dwSessionId);
        }
        return NO_ERROR;

    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        return NO_ERROR;   // intentionally ignored — stop only via RPC

    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// ---------------------------------------------------------------------------
// ServiceMain
// ---------------------------------------------------------------------------
static void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    InitializeCriticalSection(&g_lock);
    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    g_status.dwServiceType      = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState     = SERVICE_START_PENDING;
    g_status.dwControlsAccepted = SERVICE_ACCEPT_SESSIONCHANGE;
    g_status.dwWin32ExitCode    = NO_ERROR;
    g_status.dwWaitHint         = 5000;

    g_statusHandle = RegisterServiceCtrlHandlerExW(
        kServiceName, ServiceCtrlHandler, NULL);
    if (!g_statusHandle)
    {
        DeleteCriticalSection(&g_lock);
        return;
    }

    SetSvcStatus(SERVICE_START_PENDING);

    RpcServerUseProtseqEpW(
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(L"ncalrpc")),
        RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcEndpoint)),
        NULL);
    RpcServerRegisterIf(ITrayAppService_v1_0_s_ifspec, NULL, NULL);
    RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE);

    EnumerateAndLaunchAll();
    SetSvcStatus(SERVICE_RUNNING);

    WaitForSingleObject(g_stopEvent, INFINITE);

    SetSvcStatus(SERVICE_STOP_PENDING);
    TerminateAllLaunched();
    RpcMgmtStopServerListening(NULL);
    RpcServerUnregisterIf(ITrayAppService_v1_0_s_ifspec, NULL, FALSE);

    CloseHandle(g_stopEvent);
    DeleteCriticalSection(&g_lock);

    SetSvcStatus(SERVICE_STOPPED);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int wmain()
{
    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(kServiceName), ServiceMain },
        { NULL, NULL }
    };
    StartServiceCtrlDispatcherW(table);
    return 0;
}
