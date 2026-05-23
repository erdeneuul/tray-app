#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <tlhelp32.h>
#include <strsafe.h>
#include <winhttp.h>
#include <cstdlib>
#include <vector>
#include <string>
#include <thread>
#include <atomic>

#include "rpc_interface_h.h"

extern "C" {
    void* __RPC_USER MIDL_user_allocate(size_t len) { return malloc(len); }
    void  __RPC_USER MIDL_user_free(void* ptr)      { free(ptr); }
}

static const wchar_t* const kServiceName = L"TrayAppService";
static const wchar_t* const kTrayAppExe  = L"TrayApp.exe";
static const wchar_t* const kRpcEndpoint = L"TrayAppService";
static const wchar_t* const kApiHost     = L"localhost";
static const INTERNET_PORT  kApiPort     = 8443;

static SERVICE_STATUS        g_status       = {};
static SERVICE_STATUS_HANDLE g_statusHandle = NULL;
static CRITICAL_SECTION      g_lock         = {};
static std::vector<DWORD>    g_pids;
static HANDLE                g_stopEvent    = NULL;

// ---------------------------------------------------------------------------
// Auth globals (never persisted to disk)
// ---------------------------------------------------------------------------
static CRITICAL_SECTION  g_authLock     = {};
static std::wstring      g_accessToken;
static std::wstring      g_refreshToken;
static std::wstring      g_userEmail;
static bool              g_isLicensed   = false;
static std::wstring      g_licenseExpiry;

static std::thread       g_refreshThread;
static std::atomic<bool> g_refreshStop  { false };

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
// String conversion
// ---------------------------------------------------------------------------
static std::string WstrToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, NULL, NULL);
    return s;
}

static std::wstring Utf8ToWstr(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// ---------------------------------------------------------------------------
// HTTP client using WinHTTP
// Returns response body on 2xx, empty string on error or non-2xx.
// For ActivateProduct we need the status code, so we have a variant below.
// ---------------------------------------------------------------------------
static std::wstring HttpRequest(const wchar_t* method, const wchar_t* path,
                                const std::wstring& jsonBody,
                                const std::wstring& accessToken,
                                DWORD* pStatusCode = nullptr)
{
    HINTERNET hSession = WinHttpOpen(L"TrayAppService/1.0",
                                     WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};

    HINTERNET hConnect = WinHttpConnect(hSession, kApiHost, kApiPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return {}; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method, path,
                                             NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA       |
                     SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS,
                     &secFlags, sizeof(secFlags));

    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!accessToken.empty())
        headers += L"Authorization: Bearer " + accessToken + L"\r\n";

    std::string bodyUtf8 = WstrToUtf8(jsonBody);
    BOOL ok = WinHttpSendRequest(hRequest,
                                  headers.c_str(), (DWORD)headers.size(),
                                  bodyUtf8.empty() ? NULL : (LPVOID)bodyUtf8.c_str(),
                                  (DWORD)bodyUtf8.size(),
                                  (DWORD)bodyUtf8.size(),
                                  0);
    if (!ok || !WinHttpReceiveResponse(hRequest, NULL))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (pStatusCode) *pStatusCode = statusCode;

    std::string body;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0)
    {
        std::string chunk(avail, '\0');
        DWORD read = 0;
        WinHttpReadData(hRequest, chunk.data(), avail, &read);
        chunk.resize(read);
        body += chunk;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (statusCode < 200 || statusCode >= 300) return {};
    return Utf8ToWstr(body);
}

static std::wstring HttpPost(const wchar_t* path, const std::wstring& jsonBody,
                              const std::wstring& accessToken = {})
{
    return HttpRequest(L"POST", path, jsonBody, accessToken);
}

static std::wstring HttpGet(const wchar_t* path, const std::wstring& accessToken)
{
    return HttpRequest(L"GET", path, {}, accessToken);
}

// ---------------------------------------------------------------------------
// Simple JSON field extraction (wcsstr-based, no library needed)
// ---------------------------------------------------------------------------
static std::wstring JsonGetString(const std::wstring& json, const wchar_t* key)
{
    std::wstring needle = std::wstring(L"\"") + key + L"\"";
    const wchar_t* p = wcsstr(json.c_str(), needle.c_str());
    if (!p) return {};
    p += needle.size();
    while (*p == L' ' || *p == L':') ++p;
    if (*p != L'"') return {};
    ++p;
    const wchar_t* end = wcschr(p, L'"');
    if (!end) return {};
    return std::wstring(p, end);
}

// ---------------------------------------------------------------------------
// JWT exp parsing (base64url decode, no library needed)
// ---------------------------------------------------------------------------
static std::string Base64UrlDecode(const std::string& in)
{
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string src = in;
    for (auto& c : src) { if (c == '-') c = '+'; if (c == '_') c = '/'; }
    while (src.size() % 4) src += '=';

    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : src)
    {
        if (c == '=') break;
        const char* pos = strchr(tbl, c);
        if (!pos) continue;
        val = (val << 6) + (int)(pos - tbl);
        valb += 6;
        if (valb >= 0) { out += (char)((val >> valb) & 0xFF); valb -= 8; }
    }
    return out;
}

static long long ParseJwtExp(const std::wstring& token)
{
    std::string tok = WstrToUtf8(token);
    size_t d1 = tok.find('.');
    if (d1 == std::string::npos) return 0;
    size_t d2 = tok.find('.', d1 + 1);
    if (d2 == std::string::npos) return 0;
    std::string payload = Base64UrlDecode(tok.substr(d1 + 1, d2 - d1 - 1));
    const char* p = strstr(payload.c_str(), "\"exp\"");
    if (!p) return 0;
    p += 5;
    while (*p == ' ' || *p == ':') ++p;
    return _atoi64(p);
}

// ---------------------------------------------------------------------------
// License globals update — call with g_authLock held
// ---------------------------------------------------------------------------
static void UpdateLicenseFromResponse(const std::wstring& resp)
{
    if (resp.empty()) { g_isLicensed = false; return; }
    std::wstring status = JsonGetString(resp, L"status");
    std::wstring expiry = JsonGetString(resp, L"expiryDate");
    if (expiry.empty()) expiry = JsonGetString(resp, L"expiry_date");
    g_isLicensed    = (status == L"ACTIVE");
    g_licenseExpiry = expiry;
}

// ---------------------------------------------------------------------------
// Token refresh thread
// ---------------------------------------------------------------------------
static void RefreshThreadProc()
{
    while (!g_refreshStop.load())
    {
        std::wstring tok;
        {
            EnterCriticalSection(&g_authLock);
            tok = g_accessToken;
            LeaveCriticalSection(&g_authLock);
        }

        long long exp      = ParseJwtExp(tok);
        long long now      = (long long)time(nullptr);
        long long sleepSec = (exp > 0) ? (exp - now - 60) : 240;
        if (sleepSec < 10) sleepSec = 10;

        for (long long i = 0; i < sleepSec && !g_refreshStop.load(); ++i)
            Sleep(1000);
        if (g_refreshStop.load()) break;

        std::wstring refreshTok;
        {
            EnterCriticalSection(&g_authLock);
            refreshTok = g_refreshToken;
            LeaveCriticalSection(&g_authLock);
        }
        if (refreshTok.empty()) break;

        std::wstring body = L"{\"refreshToken\":\"" + refreshTok + L"\"}";
        std::wstring resp = HttpPost(L"/auth/refresh", body);
        if (!resp.empty())
        {
            std::wstring newAccess  = JsonGetString(resp, L"accessToken");
            std::wstring newRefresh = JsonGetString(resp, L"refreshToken");
            EnterCriticalSection(&g_authLock);
            if (!newAccess.empty())  g_accessToken  = newAccess;
            if (!newRefresh.empty()) g_refreshToken = newRefresh;
            LeaveCriticalSection(&g_authLock);
        }
    }
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
// RPC implementations
// ---------------------------------------------------------------------------
extern "C" void StopService(handle_t /*hBinding*/)
{
    SetEvent(g_stopEvent);
}

extern "C" long LoginUser(handle_t /*hBinding*/, wchar_t* email, wchar_t* password)
{
    std::wstring body = std::wstring(L"{\"email\":\"") + email +
                        L"\",\"password\":\"" + password + L"\"}";
    std::wstring resp = HttpPost(L"/auth/login", body);
    if (resp.empty()) return 1;

    std::wstring access  = JsonGetString(resp, L"accessToken");
    std::wstring refresh = JsonGetString(resp, L"refreshToken");
    if (access.empty()) return 1;

    std::wstring meResp    = HttpGet(L"/auth/me", access);
    std::wstring userEmail = JsonGetString(meResp, L"email");
    std::wstring licResp   = HttpGet(L"/api/license/status", access);

    EnterCriticalSection(&g_authLock);
    g_accessToken  = access;
    g_refreshToken = refresh;
    g_userEmail    = userEmail.empty() ? email : userEmail;
    UpdateLicenseFromResponse(licResp);
    LeaveCriticalSection(&g_authLock);

    g_refreshStop.store(false);
    if (g_refreshThread.joinable()) g_refreshThread.join();
    g_refreshThread = std::thread(RefreshThreadProc);

    return 0;
}

extern "C" long LogoutUser(handle_t /*hBinding*/)
{
    std::wstring tok;
    {
        EnterCriticalSection(&g_authLock);
        tok = g_accessToken;
        LeaveCriticalSection(&g_authLock);
    }

    if (!tok.empty())
        HttpPost(L"/auth/logout", {}, tok);

    g_refreshStop.store(true);
    if (g_refreshThread.joinable()) g_refreshThread.join();

    EnterCriticalSection(&g_authLock);
    g_accessToken.clear();
    g_refreshToken.clear();
    g_userEmail.clear();
    g_isLicensed    = false;
    g_licenseExpiry.clear();
    LeaveCriticalSection(&g_authLock);

    return 0;
}

extern "C" long GetCurrentUser(handle_t /*hBinding*/, wchar_t** email)
{
    EnterCriticalSection(&g_authLock);
    std::wstring e = g_userEmail;
    LeaveCriticalSection(&g_authLock);

    if (e.empty())
    {
        *email = (wchar_t*)MIDL_user_allocate(sizeof(wchar_t));
        (*email)[0] = L'\0';
        return 1;
    }
    size_t bytes = (e.size() + 1) * sizeof(wchar_t);
    *email = (wchar_t*)MIDL_user_allocate(bytes);
    wcscpy_s(*email, e.size() + 1, e.c_str());
    return 0;
}

extern "C" long GetLicenseInfo(handle_t /*hBinding*/, LICENSE_INFO* info)
{
    EnterCriticalSection(&g_authLock);
    std::wstring email    = g_userEmail;
    bool         licensed = g_isLicensed;
    std::wstring expiry   = g_licenseExpiry;
    std::wstring tok      = g_accessToken;
    LeaveCriticalSection(&g_authLock);

    if (email.empty())
    {
        info->is_licensed    = 0;
        info->error_code     = 1;
        info->expiry_date[0] = L'\0';
        StringCchCopyW(info->error_message, 256, L"Not logged in");
        return 0;
    }

    if (!tok.empty())
    {
        std::wstring licResp = HttpGet(L"/api/license/status", tok);
        EnterCriticalSection(&g_authLock);
        UpdateLicenseFromResponse(licResp);
        licensed = g_isLicensed;
        expiry   = g_licenseExpiry;
        LeaveCriticalSection(&g_authLock);
    }

    if (licensed)
    {
        info->is_licensed      = 1;
        info->error_code       = 0;
        info->error_message[0] = L'\0';
        StringCchCopyW(info->expiry_date, 64,
                       expiry.empty() ? L"" : expiry.c_str());
    }
    else
    {
        info->is_licensed    = 0;
        info->error_code     = 2;
        info->expiry_date[0] = L'\0';
        StringCchCopyW(info->error_message, 256, L"No active license");
    }
    return 0;
}

extern "C" long ActivateProduct(handle_t /*hBinding*/, wchar_t* activationCode)
{
    std::wstring tok;
    {
        EnterCriticalSection(&g_authLock);
        tok = g_accessToken;
        LeaveCriticalSection(&g_authLock);
    }
    if (tok.empty()) return 1;

    std::wstring body = std::wstring(L"{\"activationCode\":\"") + activationCode + L"\"}";
    DWORD statusCode  = 0;
    std::wstring resp = HttpRequest(L"POST", L"/api/license/activate", body, tok, &statusCode);

    if (statusCode == 402) return 3;
    if (statusCode == 403) return 4;
    if (statusCode == 404) return 2;
    if (statusCode == 409) return 5;
    if (statusCode < 200 || statusCode >= 300) return 1;

    EnterCriticalSection(&g_authLock);
    UpdateLicenseFromResponse(resp);
    LeaveCriticalSection(&g_authLock);
    return 0;
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
    InitializeCriticalSection(&g_authLock);
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
        DeleteCriticalSection(&g_authLock);
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

    g_refreshStop.store(true);
    if (g_refreshThread.joinable()) g_refreshThread.join();

    TerminateAllLaunched();
    RpcMgmtStopServerListening(NULL);
    RpcServerUnregisterIf(ITrayAppService_v1_0_s_ifspec, NULL, FALSE);

    CloseHandle(g_stopEvent);
    DeleteCriticalSection(&g_authLock);
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
