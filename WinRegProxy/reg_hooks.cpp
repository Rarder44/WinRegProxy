// reg_hooks.cpp
// Fase 1: pass-through. Ogni funzione Reg* viene inoltrata al registro vero e
// la chiamata viene scritta in reg_hook.log (accanto alla DLL).
// Il comportamento del programma NON cambia.
#include "pch.h"
#include <intrin.h>
#include <sddl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>


// ======================================================================= opzioni
// false = non registrare le chiamate fatte dalle DLL interne di Windows
// (KERNELBASE, advapi32...): sono quasi sempre duplicati, ad esempio RegOpenKeyExA
// che chiama RegOpenKeyExW. Metti true per vedere proprio tutto.
static const bool kLogInternalCallers = false;

// RegCloseKey e' molto frequente e poco utile: lasciala a false.
static const bool kLogClose = false;

// ======================================================================= stato
static HMODULE g_self = nullptr;
static HANDLE  g_log = INVALID_HANDLE_VALUE;
static DWORD   g_pid = 0;
static wchar_t g_userSid[128] = L"";
static size_t  g_userSidLen = 0;

typedef LONG(NTAPI* NtQueryKey_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef DWORD(WINAPI* GetMappedFileNameW_t)(HANDLE, LPVOID, LPWSTR, DWORD);
static NtQueryKey_t         pNtQueryKey = nullptr;
static GetMappedFileNameW_t pGetMappedFileNameW = nullptr;

// true mentre stiamo scrivendo il log: evita ricorsioni se il logger stesso
// dovesse toccare il registro.
static thread_local bool t_busy = false;

// ======================================================================= buffer temporanei
static const size_t SCR = 600;

// Anello di 8 buffer per thread: gli helper restituiscono un puntatore valido
// fino alla fine dell'espressione di LogCall(...).
static wchar_t* Scratch()
{
    static thread_local wchar_t bufs[8][SCR];
    static thread_local unsigned idx = 0;
    wchar_t* b = bufs[idx++ & 7];
    b[0] = 0;
    return b;
}

// ======================================================================= file di log
static void LogWrite(const wchar_t* line)
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    char utf8[4700];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, (int)sizeof(utf8), nullptr, nullptr);
    if (n <= 1) return;
    DWORD written = 0;
    WriteFile(g_log, utf8, (DWORD)(n - 1), &written, nullptr);   // FILE_APPEND_DATA: append atomico
}

static void LogText(const wchar_t* fmt, ...)
{
    wchar_t msg[600];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[800];
    _snwprintf_s(line, _countof(line), _TRUNCATE,
        L"%04d-%02d-%02d %02d:%02d:%02d.%03d P%-5lu [hook] %s\r\n",
        (int)t.wYear, (int)t.wMonth, (int)t.wDay, (int)t.wHour, (int)t.wMinute,
        (int)t.wSecond, (int)t.wMilliseconds, (unsigned long)g_pid, msg);
    LogWrite(line);
}

static void OpenLog()
{
    wchar_t path[MAX_PATH] = L"";
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash)
        {
            *(slash + 1) = 0;
            wcsncat_s(path, MAX_PATH, L"reg_hook.log", _TRUNCATE);
            g_log = CreateFileW(path, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
    if (g_log == INVALID_HANDLE_VALUE)   // cartella non scrivibile: ripiego su %TEMP%
    {
        wchar_t tmp[MAX_PATH] = L"";
        DWORD m = GetTempPathW(MAX_PATH, tmp);
        if (m > 0 && m < MAX_PATH)
        {
            wcsncat_s(tmp, MAX_PATH, L"reg_hook.log", _TRUNCATE);
            g_log = CreateFileW(tmp, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
    if (g_log != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER size;
        if (GetFileSizeEx(g_log, &size) && size.QuadPart == 0)   // file nuovo: BOM UTF-8
        {
            const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
            DWORD w = 0;
            WriteFile(g_log, bom, 3, &w, nullptr);
        }
    }
}

// ======================================================================= helper di formattazione
static const wchar_t* ResultName(LSTATUS r)
{
    switch (r)
    {
    case ERROR_SUCCESS:           return L"OK";
    case ERROR_FILE_NOT_FOUND:    return L"NOT_FOUND";
    case ERROR_PATH_NOT_FOUND:    return L"PATH_NOT_FOUND";
    case ERROR_ACCESS_DENIED:     return L"ACCESS_DENIED";
    case ERROR_MORE_DATA:         return L"MORE_DATA";
    case ERROR_NO_MORE_ITEMS:     return L"NO_MORE_ITEMS";
    case ERROR_INVALID_HANDLE:    return L"INVALID_HANDLE";
    case ERROR_INVALID_PARAMETER: return L"INVALID_PARAM";
    default:
    {
        wchar_t* b = Scratch();
        _snwprintf_s(b, SCR, _TRUNCATE, L"%ld", (long)r);
        return b;
    }
    }
}

static const wchar_t* TypeName(DWORD t)
{
    switch (t)
    {
    case REG_NONE:                return L"NONE";
    case REG_SZ:                  return L"SZ";
    case REG_EXPAND_SZ:           return L"EXPAND_SZ";
    case REG_BINARY:              return L"BINARY";
    case REG_DWORD:               return L"DWORD";
    case REG_DWORD_BIG_ENDIAN:    return L"DWORD_BE";
    case REG_LINK:                return L"LINK";
    case REG_MULTI_SZ:            return L"MULTI_SZ";
    case REG_RESOURCE_LIST:       return L"RESOURCE_LIST";
    case REG_QWORD:               return L"QWORD";
    default:
    {
        wchar_t* b = Scratch();
        _snwprintf_s(b, SCR, _TRUNCATE, L"%u", (unsigned)t);
        return b;
    }
    }
}

static DWORD D(const DWORD* p) { return p ? *p : 0; }

static const wchar_t* WS(const wchar_t* s) { return s ? s : L"(null)"; }

static const wchar_t* AS(const char* s)
{
    wchar_t* out = Scratch();
    if (!s) { wcsncpy_s(out, SCR, L"(null)", _TRUNCATE); return out; }
    size_t len = strnlen(s, SCR - 1);
    int n = len ? MultiByteToWideChar(CP_ACP, 0, s, (int)len, out, (int)(SCR - 1)) : 0;
    out[n > 0 ? n : 0] = 0;
    return out;
}

// nome del valore: vuoto o null = valore predefinito della chiave
static const wchar_t* VN(const wchar_t* s) { return (s && *s) ? s : L"(Default)"; }
static const wchar_t* VNA(const char* s) { return (s && *s) ? AS(s) : L"(Default)"; }

// Rappresentazione breve di un dato del registro
static const wchar_t* VD(DWORD type, const void* data, DWORD cb, bool ansi)
{
    wchar_t* out = Scratch();
    if (!data) { wcsncpy_s(out, SCR, L"-", _TRUNCATE); return out; }

    switch (type)
    {
    case REG_SZ:
    case REG_EXPAND_SZ:
    case REG_MULTI_SZ:   // mostra solo la prima stringa
    {
        const size_t kMax = 120;
        wchar_t tmp[kMax + 1];
        size_t n = 0;
        size_t total = ansi ? (size_t)cb : (size_t)cb / sizeof(wchar_t);
        if (ansi)
        {
            size_t avail = total < kMax ? total : kMax;
            int c = avail ? MultiByteToWideChar(CP_ACP, 0, (const char*)data, (int)avail, tmp, (int)kMax) : 0;
            n = c > 0 ? (size_t)c : 0;
        }
        else
        {
            size_t avail = total < kMax ? total : kMax;
            memcpy(tmp, data, avail * sizeof(wchar_t));
            n = avail;
        }
        size_t len = 0;
        for (; len < n && tmp[len]; ++len)
            if (tmp[len] < 0x20) tmp[len] = L' ';   // una riga di log per chiamata
        tmp[len] = 0;
        _snwprintf_s(out, SCR, _TRUNCATE, L"\"%s\"%s", tmp, (total > kMax && len == n) ? L"..." : L"");
        return out;
    }
    case REG_DWORD:
        if (cb >= 4)
        {
            unsigned v;
            memcpy(&v, data, 4);
            _snwprintf_s(out, SCR, _TRUNCATE, L"0x%08X (%u)", v, v);
            return out;
        }
        break;
    case REG_QWORD:
        if (cb >= 8)
        {
            unsigned long long v;
            memcpy(&v, data, 8);
            _snwprintf_s(out, SCR, _TRUNCATE, L"0x%016llX (%llu)", v, v);
            return out;
        }
        break;
    default:
        break;
    }

    // binario o tipo sconosciuto: primi 24 byte in esadecimale
    static const wchar_t hexd[] = L"0123456789ABCDEF";
    const BYTE* p = (const BYTE*)data;
    size_t n = cb < 24 ? (size_t)cb : 24;
    size_t pos = 0;
    for (size_t i = 0; i < n; ++i)
    {
        out[pos++] = hexd[p[i] >> 4];
        out[pos++] = hexd[p[i] & 15];
        out[pos++] = L' ';
    }
    if (pos) --pos;
    out[pos] = 0;
    _snwprintf_s(out + pos, SCR - pos, _TRUNCATE, L"%s [%u byte]", cb > n ? L"..." : L"", (unsigned)cb);
    return out;
}

// dati dopo una Query/Enum/GetValue: validi solo se la chiamata e' riuscita
static const wchar_t* OutData(LSTATUS r, const DWORD* type, const void* data, const DWORD* cb, bool ansi)
{
    if (r != ERROR_SUCCESS || !data || !cb) return L"-";
    return VD(type ? *type : REG_NONE, data, *cb, ansi);
}

static const wchar_t* OutStr(LSTATUS r, const void* data, const LONG* cb, bool ansi)
{
    if (r != ERROR_SUCCESS || !data || !cb || *cb <= 0) return L"-";
    return VD(REG_SZ, data, (DWORD)*cb, ansi);
}

// dati di una Set: validi solo se la chiamata e' riuscita
static const wchar_t* SetData(LSTATUS r, DWORD type, const void* data, DWORD cb, bool ansi)
{
    if (r != ERROR_SUCCESS || !data) return L"-";
    return VD(type, data, cb, ansi);
}

static const wchar_t* TypeR(LSTATUS r, const DWORD* type)
{
    return (r == ERROR_SUCCESS && type) ? TypeName(*type) : L"-";
}

// ======================================================================= nome di una chiave (HKEY -> percorso)
static void ShortenKeyPath(const wchar_t* nt, wchar_t* out, size_t cch)
{
    static const wchar_t kMachine[] = L"\\REGISTRY\\MACHINE";
    static const wchar_t kUser[] = L"\\REGISTRY\\USER";
    const size_t lenMachine = _countof(kMachine) - 1;
    const size_t lenUser = _countof(kUser) - 1;

    if (_wcsnicmp(nt, kMachine, lenMachine) == 0)
    {
        _snwprintf_s(out, cch, _TRUNCATE, L"HKLM%s", nt + lenMachine);
        return;
    }
    if (_wcsnicmp(nt, kUser, lenUser) == 0)
    {
        const wchar_t* rest = nt + lenUser;   // "\S-1-5-21-..." oppure vuoto
        if (*rest == L'\\' && g_userSidLen && _wcsnicmp(rest + 1, g_userSid, g_userSidLen) == 0)
        {
            const wchar_t* after = rest + 1 + g_userSidLen;
            if (_wcsnicmp(after, L"_Classes", 8) == 0)
            {
                _snwprintf_s(out, cch, _TRUNCATE, L"HKCU\\Software\\Classes%s", after + 8);
                return;
            }
            if (*after == 0 || *after == L'\\')
            {
                _snwprintf_s(out, cch, _TRUNCATE, L"HKCU%s", after);
                return;
            }
        }
        _snwprintf_s(out, cch, _TRUNCATE, L"HKU%s", rest);
        return;
    }
    wcsncpy_s(out, cch, nt, _TRUNCATE);
}

static const wchar_t* KN(HKEY h)
{
    wchar_t* out = Scratch();
    const wchar_t* pre = nullptr;
    if (h == HKEY_CLASSES_ROOT)     pre = L"HKCR";
    else if (h == HKEY_CURRENT_USER)     pre = L"HKCU";
    else if (h == HKEY_LOCAL_MACHINE)    pre = L"HKLM";
    else if (h == HKEY_USERS)            pre = L"HKU";
    else if (h == HKEY_PERFORMANCE_DATA) pre = L"HKPD";
    else if (h == HKEY_CURRENT_CONFIG)   pre = L"HKCC";
    else if (h == HKEY_DYN_DATA)         pre = L"HKDD";
    else if (!h)                         pre = L"(null)";
    if (pre) { wcsncpy_s(out, SCR, pre, _TRUNCATE); return out; }

    if (pNtQueryKey)
    {
        struct { ULONG NameLength; WCHAR Name[1024]; } info;   // KEY_NAME_INFORMATION
        ULONG len = 0;
        LONG st = pNtQueryKey((HANDLE)h, 3 /* KeyNameInformation */, &info, (ULONG)sizeof(info), &len);
        if (st >= 0)
        {
            size_t nch = info.NameLength / sizeof(WCHAR);
            if (nch > 1023) nch = 1023;
            info.Name[nch] = 0;
            ShortenKeyPath(info.Name, out, SCR);
            return out;
        }
    }
    _snwprintf_s(out, SCR, _TRUNCATE, L"<hkey %p>", (void*)h);
    return out;
}

// chiave restituita da un Open/Create (solo se riuscito)
static const wchar_t* OutKN(LSTATUS r, const HKEY* out)
{
    return (r == ERROR_SUCCESS && out && *out) ? KN(*out) : L"-";
}

// ======================================================================= chi ha fatto la chiamata
struct CallerEntry { PVOID base; wchar_t name[40]; };
static CallerEntry g_callers[64];
static unsigned    g_callerCount = 0;
static SRWLOCK     g_callerLock = SRWLOCK_INIT;

static void CallerName(void* ra, wchar_t* out, size_t cch)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(ra, &mbi, sizeof(mbi)) || !mbi.AllocationBase)
    {
        wcsncpy_s(out, cch, L"?", _TRUNCATE);
        return;
    }
    PVOID base = mbi.AllocationBase;

    AcquireSRWLockShared(&g_callerLock);
    for (unsigned i = 0; i < g_callerCount; ++i)
    {
        if (g_callers[i].base == base)
        {
            wcsncpy_s(out, cch, g_callers[i].name, _TRUNCATE);
            ReleaseSRWLockShared(&g_callerLock);
            return;
        }
    }
    ReleaseSRWLockShared(&g_callerLock);

    wchar_t full[MAX_PATH] = L"";
    const wchar_t* shortName = L"?";
    if (pGetMappedFileNameW && pGetMappedFileNameW(GetCurrentProcess(), base, full, MAX_PATH))
    {
        const wchar_t* slash = wcsrchr(full, L'\\');
        shortName = slash ? slash + 1 : full;
    }
    wcsncpy_s(out, cch, shortName, _TRUNCATE);

    AcquireSRWLockExclusive(&g_callerLock);
    if (g_callerCount < _countof(g_callers))
    {
        g_callers[g_callerCount].base = base;
        wcsncpy_s(g_callers[g_callerCount].name, _countof(g_callers[g_callerCount].name), out, _TRUNCATE);
        ++g_callerCount;
    }
    ReleaseSRWLockExclusive(&g_callerLock);
}

static bool IsInternalModule(const wchar_t* n)
{
    return _wcsicmp(n, L"kernelbase.dll") == 0 || _wcsicmp(n, L"advapi32.dll") == 0 ||
        _wcsicmp(n, L"kernel32.dll") == 0 || _wcsicmp(n, L"ntdll.dll") == 0 ||
        _wcsicmp(n, L"sechost.dll") == 0;
}

static bool ShouldLog(void* ra, wchar_t* caller /* [40] */)
{
    CallerName(ra, caller, 40);
    return kLogInternalCallers || !IsInternalModule(caller);
}

// ======================================================================= riga di log di una chiamata
static void LogCall(const wchar_t* caller, const char* fn, LSTATUS r, const wchar_t* fmt, ...)
{
    wchar_t detail[1100];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(detail, _countof(detail), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1536];
    _snwprintf_s(line, _countof(line), _TRUNCATE,
        L"%04d-%02d-%02d %02d:%02d:%02d.%03d P%-5lu T%-5lu %-18s %-24hs %s => %s\r\n",
        (int)t.wYear, (int)t.wMonth, (int)t.wDay, (int)t.wHour, (int)t.wMinute,
        (int)t.wSecond, (int)t.wMilliseconds,
        (unsigned long)g_pid, (unsigned long)GetCurrentThreadId(),
        caller, fn, detail, ResultName(r));
    LogWrite(line);
}

// ======================================================================= macro che genera i detour
// NAME   = nome della funzione        PARAMS = firma (con le parentesi)
// ARGS   = argomenti da passare       ...    = formato + argomenti del log
// Dentro il log si puo' usare r_ (il codice restituito).
#define HOOK(NAME, PARAMS, ARGS, ...)                                           \
    static LSTATUS WINAPI D_##NAME PARAMS                                       \
    {                                                                           \
        void* ra_ = _ReturnAddress();                                           \
        LSTATUS r_ = Real_##NAME ARGS;                                          \
        if (!t_busy)                                                            \
        {                                                                       \
            DWORD le_ = GetLastError();                                         \
            t_busy = true;                                                      \
            wchar_t caller_[40];                                                \
            if (ShouldLog(ra_, caller_))                                        \
                LogCall(caller_, #NAME, r_, __VA_ARGS__);                       \
            t_busy = false;                                                     \
            SetLastError(le_);                                                  \
        }                                                                       \
        return r_;                                                              \
    }

// ----------------------------------------------------------------------- Open
HOOK(RegOpenKeyExA, (HKEY h, LPCSTR sub, DWORD opt, REGSAM sam, PHKEY out), (h, sub, opt, sam, out),
    L"key=%s sub=\"%s\" opt=0x%X sam=0x%X -> %s", KN(h), AS(sub), (unsigned)opt, (unsigned)sam, OutKN(r_, out))
    HOOK(RegOpenKeyExW, (HKEY h, LPCWSTR sub, DWORD opt, REGSAM sam, PHKEY out), (h, sub, opt, sam, out),
        L"key=%s sub=\"%s\" opt=0x%X sam=0x%X -> %s", KN(h), WS(sub), (unsigned)opt, (unsigned)sam, OutKN(r_, out))

    HOOK(RegOpenKeyA, (HKEY h, LPCSTR sub, PHKEY out), (h, sub, out),
        L"key=%s sub=\"%s\" -> %s", KN(h), AS(sub), OutKN(r_, out))
    HOOK(RegOpenKeyW, (HKEY h, LPCWSTR sub, PHKEY out), (h, sub, out),
        L"key=%s sub=\"%s\" -> %s", KN(h), WS(sub), OutKN(r_, out))

    HOOK(RegOpenKeyTransactedA, (HKEY h, LPCSTR sub, DWORD opt, REGSAM sam, PHKEY out, HANDLE trans, PVOID ext),
        (h, sub, opt, sam, out, trans, ext),
        L"key=%s sub=\"%s\" opt=0x%X sam=0x%X -> %s", KN(h), AS(sub), (unsigned)opt, (unsigned)sam, OutKN(r_, out))
    HOOK(RegOpenKeyTransactedW, (HKEY h, LPCWSTR sub, DWORD opt, REGSAM sam, PHKEY out, HANDLE trans, PVOID ext),
        (h, sub, opt, sam, out, trans, ext),
        L"key=%s sub=\"%s\" opt=0x%X sam=0x%X -> %s", KN(h), WS(sub), (unsigned)opt, (unsigned)sam, OutKN(r_, out))

    HOOK(RegOpenCurrentUser, (REGSAM sam, PHKEY out), (sam, out),
        L"sam=0x%X -> %s", (unsigned)sam, OutKN(r_, out))
    HOOK(RegOpenUserClassesRoot, (HANDLE tok, DWORD opt, REGSAM sam, PHKEY out), (tok, opt, sam, out),
        L"opt=0x%X sam=0x%X -> %s", (unsigned)opt, (unsigned)sam, OutKN(r_, out))
    HOOK(RegOverridePredefKey, (HKEY h, HKEY hNew), (h, hNew),
        L"key=%s new=%s", KN(h), KN(hNew))

    // ----------------------------------------------------------------------- Create
    HOOK(RegCreateKeyExA, (HKEY h, LPCSTR sub, DWORD res, LPSTR cls, DWORD opt, REGSAM sam,
        LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp),
        (h, sub, res, cls, opt, sam, sa, out, disp),
        L"key=%s sub=\"%s\" opt=0x%X sam=0x%X disp=%u -> %s", KN(h), AS(sub), (unsigned)opt, (unsigned)sam,
        (unsigned)(r_ == ERROR_SUCCESS ? D(disp) : 0), OutKN(r_, out))
    HOOK(RegCreateKeyExW, (HKEY h, LPCWSTR sub, DWORD res, LPWSTR cls, DWORD opt, REGSAM sam,
        LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp),
        (h, sub, res, cls, opt, sam, sa, out, disp),
        L"key=%s sub=\"%s\" opt=0x%X sam=0x%X disp=%u -> %s", KN(h), WS(sub), (unsigned)opt, (unsigned)sam,
        (unsigned)(r_ == ERROR_SUCCESS ? D(disp) : 0), OutKN(r_, out))

    HOOK(RegCreateKeyA, (HKEY h, LPCSTR sub, PHKEY out), (h, sub, out),
        L"key=%s sub=\"%s\" -> %s", KN(h), AS(sub), OutKN(r_, out))
    HOOK(RegCreateKeyW, (HKEY h, LPCWSTR sub, PHKEY out), (h, sub, out),
        L"key=%s sub=\"%s\" -> %s", KN(h), WS(sub), OutKN(r_, out))

    HOOK(RegCreateKeyTransactedA, (HKEY h, LPCSTR sub, DWORD res, LPSTR cls, DWORD opt, REGSAM sam,
        LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp, HANDLE trans, PVOID ext),
        (h, sub, res, cls, opt, sam, sa, out, disp, trans, ext),
        L"key=%s sub=\"%s\" opt=0x%X sam=0x%X -> %s", KN(h), AS(sub), (unsigned)opt, (unsigned)sam, OutKN(r_, out))
    HOOK(RegCreateKeyTransactedW, (HKEY h, LPCWSTR sub, DWORD res, LPWSTR cls, DWORD opt, REGSAM sam,
        LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp, HANDLE trans, PVOID ext),
        (h, sub, res, cls, opt, sam, sa, out, disp, trans, ext),
        L"key=%s sub=\"%s\" opt=0x%X sam=0x%X -> %s", KN(h), WS(sub), (unsigned)opt, (unsigned)sam, OutKN(r_, out))

    // ----------------------------------------------------------------------- Query
    HOOK(RegQueryValueExA, (HKEY h, LPCSTR name, LPDWORD rsv, LPDWORD type, LPBYTE data, LPDWORD cb),
        (h, name, rsv, type, data, cb),
        L"key=%s value=\"%s\" type=%s size=%u data=%s", KN(h), VNA(name), TypeR(r_, type), (unsigned)D(cb),
        OutData(r_, type, data, cb, true))
    HOOK(RegQueryValueExW, (HKEY h, LPCWSTR name, LPDWORD rsv, LPDWORD type, LPBYTE data, LPDWORD cb),
        (h, name, rsv, type, data, cb),
        L"key=%s value=\"%s\" type=%s size=%u data=%s", KN(h), VN(name), TypeR(r_, type), (unsigned)D(cb),
        OutData(r_, type, data, cb, false))

    HOOK(RegQueryValueA, (HKEY h, LPCSTR sub, LPSTR data, PLONG cb), (h, sub, data, cb),
        L"key=%s sub=\"%s\" size=%ld data=%s", KN(h), AS(sub), (long)((r_ == ERROR_SUCCESS && cb) ? *cb : 0),
        OutStr(r_, data, cb, true))
    HOOK(RegQueryValueW, (HKEY h, LPCWSTR sub, LPWSTR data, PLONG cb), (h, sub, data, cb),
        L"key=%s sub=\"%s\" size=%ld data=%s", KN(h), WS(sub), (long)((r_ == ERROR_SUCCESS && cb) ? *cb : 0),
        OutStr(r_, data, cb, false))

    HOOK(RegGetValueA, (HKEY h, LPCSTR sub, LPCSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD cb),
        (h, sub, name, flags, type, data, cb),
        L"key=%s sub=\"%s\" value=\"%s\" flags=0x%X type=%s size=%u data=%s", KN(h), AS(sub), VNA(name),
        (unsigned)flags, TypeR(r_, type), (unsigned)D(cb), OutData(r_, type, data, cb, true))
    HOOK(RegGetValueW, (HKEY h, LPCWSTR sub, LPCWSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD cb),
        (h, sub, name, flags, type, data, cb),
        L"key=%s sub=\"%s\" value=\"%s\" flags=0x%X type=%s size=%u data=%s", KN(h), WS(sub), VN(name),
        (unsigned)flags, TypeR(r_, type), (unsigned)D(cb), OutData(r_, type, data, cb, false))

    HOOK(RegQueryInfoKeyA, (HKEY h, LPSTR cls, LPDWORD ccls, LPDWORD rsv, LPDWORD nsub, LPDWORD maxsub,
        LPDWORD maxcls, LPDWORD nval, LPDWORD maxvn, LPDWORD maxvl, LPDWORD secd, PFILETIME ft),
        (h, cls, ccls, rsv, nsub, maxsub, maxcls, nval, maxvn, maxvl, secd, ft),
        L"key=%s subkeys=%u values=%u", KN(h),
        (unsigned)(r_ == ERROR_SUCCESS ? D(nsub) : 0), (unsigned)(r_ == ERROR_SUCCESS ? D(nval) : 0))
    HOOK(RegQueryInfoKeyW, (HKEY h, LPWSTR cls, LPDWORD ccls, LPDWORD rsv, LPDWORD nsub, LPDWORD maxsub,
        LPDWORD maxcls, LPDWORD nval, LPDWORD maxvn, LPDWORD maxvl, LPDWORD secd, PFILETIME ft),
        (h, cls, ccls, rsv, nsub, maxsub, maxcls, nval, maxvn, maxvl, secd, ft),
        L"key=%s subkeys=%u values=%u", KN(h),
        (unsigned)(r_ == ERROR_SUCCESS ? D(nsub) : 0), (unsigned)(r_ == ERROR_SUCCESS ? D(nval) : 0))

    // ----------------------------------------------------------------------- Set
    HOOK(RegSetValueExA, (HKEY h, LPCSTR name, DWORD rsv, DWORD type, const BYTE* data, DWORD cb),
        (h, name, rsv, type, data, cb),
        L"key=%s value=\"%s\" type=%s size=%u data=%s", KN(h), VNA(name), TypeName(type), (unsigned)cb,
        SetData(r_, type, data, cb, true))
    HOOK(RegSetValueExW, (HKEY h, LPCWSTR name, DWORD rsv, DWORD type, const BYTE* data, DWORD cb),
        (h, name, rsv, type, data, cb),
        L"key=%s value=\"%s\" type=%s size=%u data=%s", KN(h), VN(name), TypeName(type), (unsigned)cb,
        SetData(r_, type, data, cb, false))

    HOOK(RegSetValueA, (HKEY h, LPCSTR sub, DWORD type, LPCSTR data, DWORD cb), (h, sub, type, data, cb),
        L"key=%s sub=\"%s\" type=%s data=\"%s\"", KN(h), AS(sub), TypeName(type),
        (r_ == ERROR_SUCCESS ? AS(data) : L"-"))
    HOOK(RegSetValueW, (HKEY h, LPCWSTR sub, DWORD type, LPCWSTR data, DWORD cb), (h, sub, type, data, cb),
        L"key=%s sub=\"%s\" type=%s data=\"%s\"", KN(h), WS(sub), TypeName(type),
        (r_ == ERROR_SUCCESS ? WS(data) : L"-"))

    HOOK(RegSetKeyValueA, (HKEY h, LPCSTR sub, LPCSTR name, DWORD type, LPCVOID data, DWORD cb),
        (h, sub, name, type, data, cb),
        L"key=%s sub=\"%s\" value=\"%s\" type=%s size=%u data=%s", KN(h), AS(sub), VNA(name), TypeName(type),
        (unsigned)cb, SetData(r_, type, data, cb, true))
    HOOK(RegSetKeyValueW, (HKEY h, LPCWSTR sub, LPCWSTR name, DWORD type, LPCVOID data, DWORD cb),
        (h, sub, name, type, data, cb),
        L"key=%s sub=\"%s\" value=\"%s\" type=%s size=%u data=%s", KN(h), WS(sub), VN(name), TypeName(type),
        (unsigned)cb, SetData(r_, type, data, cb, false))

    // ----------------------------------------------------------------------- Delete
    HOOK(RegDeleteKeyA, (HKEY h, LPCSTR sub), (h, sub), L"key=%s sub=\"%s\"", KN(h), AS(sub))
    HOOK(RegDeleteKeyW, (HKEY h, LPCWSTR sub), (h, sub), L"key=%s sub=\"%s\"", KN(h), WS(sub))
    HOOK(RegDeleteKeyExA, (HKEY h, LPCSTR sub, REGSAM sam, DWORD rsv), (h, sub, sam, rsv),
        L"key=%s sub=\"%s\" sam=0x%X", KN(h), AS(sub), (unsigned)sam)
    HOOK(RegDeleteKeyExW, (HKEY h, LPCWSTR sub, REGSAM sam, DWORD rsv), (h, sub, sam, rsv),
        L"key=%s sub=\"%s\" sam=0x%X", KN(h), WS(sub), (unsigned)sam)
    HOOK(RegDeleteValueA, (HKEY h, LPCSTR name), (h, name), L"key=%s value=\"%s\"", KN(h), VNA(name))
    HOOK(RegDeleteValueW, (HKEY h, LPCWSTR name), (h, name), L"key=%s value=\"%s\"", KN(h), VN(name))
    HOOK(RegDeleteTreeA, (HKEY h, LPCSTR sub), (h, sub), L"key=%s sub=\"%s\"", KN(h), AS(sub))
    HOOK(RegDeleteTreeW, (HKEY h, LPCWSTR sub), (h, sub), L"key=%s sub=\"%s\"", KN(h), WS(sub))
    HOOK(RegDeleteKeyValueA, (HKEY h, LPCSTR sub, LPCSTR name), (h, sub, name),
        L"key=%s sub=\"%s\" value=\"%s\"", KN(h), AS(sub), VNA(name))
    HOOK(RegDeleteKeyValueW, (HKEY h, LPCWSTR sub, LPCWSTR name), (h, sub, name),
        L"key=%s sub=\"%s\" value=\"%s\"", KN(h), WS(sub), VN(name))

    // ----------------------------------------------------------------------- Enumerate
    HOOK(RegEnumKeyExA, (HKEY h, DWORD idx, LPSTR name, LPDWORD cch, LPDWORD rsv, LPSTR cls, LPDWORD ccls, PFILETIME ft),
        (h, idx, name, cch, rsv, cls, ccls, ft),
        L"key=%s idx=%u name=\"%s\"", KN(h), (unsigned)idx, (r_ == ERROR_SUCCESS ? AS(name) : L"-"))
    HOOK(RegEnumKeyExW, (HKEY h, DWORD idx, LPWSTR name, LPDWORD cch, LPDWORD rsv, LPWSTR cls, LPDWORD ccls, PFILETIME ft),
        (h, idx, name, cch, rsv, cls, ccls, ft),
        L"key=%s idx=%u name=\"%s\"", KN(h), (unsigned)idx, (r_ == ERROR_SUCCESS ? WS(name) : L"-"))

    HOOK(RegEnumKeyA, (HKEY h, DWORD idx, LPSTR name, DWORD cch), (h, idx, name, cch),
        L"key=%s idx=%u name=\"%s\"", KN(h), (unsigned)idx, (r_ == ERROR_SUCCESS ? AS(name) : L"-"))
    HOOK(RegEnumKeyW, (HKEY h, DWORD idx, LPWSTR name, DWORD cch), (h, idx, name, cch),
        L"key=%s idx=%u name=\"%s\"", KN(h), (unsigned)idx, (r_ == ERROR_SUCCESS ? WS(name) : L"-"))

    HOOK(RegEnumValueA, (HKEY h, DWORD idx, LPSTR name, LPDWORD cch, LPDWORD rsv, LPDWORD type, LPBYTE data, LPDWORD cb),
        (h, idx, name, cch, rsv, type, data, cb),
        L"key=%s idx=%u name=\"%s\" type=%s data=%s", KN(h), (unsigned)idx, (r_ == ERROR_SUCCESS ? AS(name) : L"-"),
        TypeR(r_, type), OutData(r_, type, data, cb, true))
    HOOK(RegEnumValueW, (HKEY h, DWORD idx, LPWSTR name, LPDWORD cch, LPDWORD rsv, LPDWORD type, LPBYTE data, LPDWORD cb),
        (h, idx, name, cch, rsv, type, data, cb),
        L"key=%s idx=%u name=\"%s\" type=%s data=%s", KN(h), (unsigned)idx, (r_ == ERROR_SUCCESS ? WS(name) : L"-"),
        TypeR(r_, type), OutData(r_, type, data, cb, false))

    // ----------------------------------------------------------------------- Flush / notify
    HOOK(RegFlushKey, (HKEY h), (h), L"key=%s", KN(h))
    HOOK(RegNotifyChangeKeyValue, (HKEY h, BOOL subtree, DWORD filter, HANDLE evt, BOOL async),
        (h, subtree, filter, evt, async),
        L"key=%s subtree=%d filter=0x%X async=%d", KN(h), (int)subtree, (unsigned)filter, (int)async)

    // ----------------------------------------------------------------------- Copy / replace / save / restore
    HOOK(RegCopyTreeA, (HKEY src, LPCSTR sub, HKEY dst), (src, sub, dst),
        L"src=%s sub=\"%s\" dst=%s", KN(src), AS(sub), KN(dst))
    HOOK(RegCopyTreeW, (HKEY src, LPCWSTR sub, HKEY dst), (src, sub, dst),
        L"src=%s sub=\"%s\" dst=%s", KN(src), WS(sub), KN(dst))
    HOOK(RegReplaceKeyA, (HKEY h, LPCSTR sub, LPCSTR newFile, LPCSTR oldFile), (h, sub, newFile, oldFile),
        L"key=%s sub=\"%s\" new=\"%s\" old=\"%s\"", KN(h), AS(sub), AS(newFile), AS(oldFile))
    HOOK(RegReplaceKeyW, (HKEY h, LPCWSTR sub, LPCWSTR newFile, LPCWSTR oldFile), (h, sub, newFile, oldFile),
        L"key=%s sub=\"%s\" new=\"%s\" old=\"%s\"", KN(h), WS(sub), WS(newFile), WS(oldFile))
    HOOK(RegRestoreKeyA, (HKEY h, LPCSTR file, DWORD flags), (h, file, flags),
        L"key=%s file=\"%s\" flags=0x%X", KN(h), AS(file), (unsigned)flags)
    HOOK(RegRestoreKeyW, (HKEY h, LPCWSTR file, DWORD flags), (h, file, flags),
        L"key=%s file=\"%s\" flags=0x%X", KN(h), WS(file), (unsigned)flags)
    HOOK(RegSaveKeyA, (HKEY h, LPCSTR file, LPSECURITY_ATTRIBUTES sa), (h, file, sa),
        L"key=%s file=\"%s\"", KN(h), AS(file))
    HOOK(RegSaveKeyW, (HKEY h, LPCWSTR file, LPSECURITY_ATTRIBUTES sa), (h, file, sa),
        L"key=%s file=\"%s\"", KN(h), WS(file))
    HOOK(RegSaveKeyExA, (HKEY h, LPCSTR file, LPSECURITY_ATTRIBUTES sa, DWORD flags), (h, file, sa, flags),
        L"key=%s file=\"%s\" flags=0x%X", KN(h), AS(file), (unsigned)flags)
    HOOK(RegSaveKeyExW, (HKEY h, LPCWSTR file, LPSECURITY_ATTRIBUTES sa, DWORD flags), (h, file, sa, flags),
        L"key=%s file=\"%s\" flags=0x%X", KN(h), WS(file), (unsigned)flags)

    // ----------------------------------------------------------------------- Security
    HOOK(RegGetKeySecurity, (HKEY h, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd, LPDWORD len), (h, si, sd, len),
        L"key=%s info=0x%X", KN(h), (unsigned)si)
    HOOK(RegSetKeySecurity, (HKEY h, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR sd), (h, si, sd),
        L"key=%s info=0x%X", KN(h), (unsigned)si)

    // ----------------------------------------------------------------------- Remote registry
    HOOK(RegConnectRegistryA, (LPCSTR machine, HKEY h, PHKEY out), (machine, h, out),
        L"machine=\"%s\" key=%s -> %s", AS(machine), KN(h), OutKN(r_, out))
    HOOK(RegConnectRegistryW, (LPCWSTR machine, HKEY h, PHKEY out), (machine, h, out),
        L"machine=\"%s\" key=%s -> %s", WS(machine), KN(h), OutKN(r_, out))
    HOOK(RegDisconnectRegistry, (HKEY h), (h), L"key=%s", KN(h))

    // ----------------------------------------------------------------------- Close (a mano: il nome va risolto PRIMA di chiudere)
    static LSTATUS WINAPI D_RegCloseKey(HKEY h)
{
    if (!kLogClose || t_busy)
        return Real_RegCloseKey(h);

    void* ra = _ReturnAddress();
    DWORD le = GetLastError();
    t_busy = true;
    wchar_t caller[40];
    bool doLog = ShouldLog(ra, caller);
    wchar_t name[SCR];
    name[0] = 0;
    if (doLog) wcsncpy_s(name, SCR, KN(h), _TRUNCATE);
    t_busy = false;
    SetLastError(le);

    LSTATUS r = Real_RegCloseKey(h);

    if (doLog)
    {
        le = GetLastError();
        t_busy = true;
        LogCall(caller, "RegCloseKey", r, L"key=%s", name);
        t_busy = false;
        SetLastError(le);
    }
    return r;
}

// ======================================================================= installazione
struct HookEntry { const char* name; void* detour; void** original; };
#define ENTRY(NAME) { #NAME, reinterpret_cast<void*>(&D_##NAME), reinterpret_cast<void**>(&Real_##NAME) }

static HookEntry g_hooks[] =
{
    ENTRY(RegOpenKeyExA), ENTRY(RegOpenKeyExW), ENTRY(RegOpenKeyA), ENTRY(RegOpenKeyW),
    ENTRY(RegOpenKeyTransactedA), ENTRY(RegOpenKeyTransactedW),
    ENTRY(RegOpenCurrentUser), ENTRY(RegOpenUserClassesRoot), ENTRY(RegOverridePredefKey),

    ENTRY(RegCreateKeyExA), ENTRY(RegCreateKeyExW), ENTRY(RegCreateKeyA), ENTRY(RegCreateKeyW),
    ENTRY(RegCreateKeyTransactedA), ENTRY(RegCreateKeyTransactedW),

    ENTRY(RegQueryValueExA), ENTRY(RegQueryValueExW), ENTRY(RegQueryValueA), ENTRY(RegQueryValueW),
    ENTRY(RegGetValueA), ENTRY(RegGetValueW), ENTRY(RegQueryInfoKeyA), ENTRY(RegQueryInfoKeyW),

    ENTRY(RegSetValueExA), ENTRY(RegSetValueExW), ENTRY(RegSetValueA), ENTRY(RegSetValueW),
    ENTRY(RegSetKeyValueA), ENTRY(RegSetKeyValueW),

    ENTRY(RegDeleteKeyA), ENTRY(RegDeleteKeyW), ENTRY(RegDeleteKeyExA), ENTRY(RegDeleteKeyExW),
    ENTRY(RegDeleteValueA), ENTRY(RegDeleteValueW), ENTRY(RegDeleteTreeA), ENTRY(RegDeleteTreeW),
    ENTRY(RegDeleteKeyValueA), ENTRY(RegDeleteKeyValueW),

    ENTRY(RegEnumKeyExA), ENTRY(RegEnumKeyExW), ENTRY(RegEnumKeyA), ENTRY(RegEnumKeyW),
    ENTRY(RegEnumValueA), ENTRY(RegEnumValueW),

    ENTRY(RegCloseKey), ENTRY(RegFlushKey), ENTRY(RegNotifyChangeKeyValue),

    ENTRY(RegCopyTreeA), ENTRY(RegCopyTreeW), ENTRY(RegReplaceKeyA), ENTRY(RegReplaceKeyW),
    ENTRY(RegRestoreKeyA), ENTRY(RegRestoreKeyW), ENTRY(RegSaveKeyA), ENTRY(RegSaveKeyW),
    ENTRY(RegSaveKeyExA), ENTRY(RegSaveKeyExW),

    ENTRY(RegGetKeySecurity), ENTRY(RegSetKeySecurity),

    ENTRY(RegConnectRegistryA), ENTRY(RegConnectRegistryW), ENTRY(RegDisconnectRegistry),
};

// GetProcAddress su advapi32 segue da solo i forwarder verso kernelbase e vale
// anche per le funzioni che stanno solo in advapi32. Se advapi32 non e' caricata
// si ripiega su kernelbase.
static void* ResolveTarget(const char* name)
{
    void* p = nullptr;
    HMODULE adv = GetModuleHandleW(L"advapi32.dll");
    if (adv) p = (void*)GetProcAddress(adv, name);
    if (!p)
    {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        if (kb) p = (void*)GetProcAddress(kb, name);
    }
    return p;
}

static void InitUserSid()
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return;
    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    if (len)
    {
        BYTE* buf = (BYTE*)LocalAlloc(LPTR, len);
        if (buf && GetTokenInformation(tok, TokenUser, buf, len, &len))
        {
            LPWSTR s = nullptr;
            if (ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &s))
            {
                wcsncpy_s(g_userSid, _countof(g_userSid), s, _TRUNCATE);
                g_userSidLen = wcslen(g_userSid);
                LocalFree(s);
            }
        }
        if (buf) LocalFree(buf);
    }
    CloseHandle(tok);
}

bool InstallRegHooks(HMODULE self)
{
    g_self = self;
    g_pid = GetCurrentProcessId();
    OpenLog();
    InitUserSid();

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) pNtQueryKey = (NtQueryKey_t)GetProcAddress(ntdll, "NtQueryKey");
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32) pGetMappedFileNameW = (GetMappedFileNameW_t)GetProcAddress(k32, "K32GetMappedFileNameW");

    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    LogText(L"=== avvio: %s ===", exe);

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        LogText(L"MH_Initialize fallita: %hs", MH_StatusToString(st));
        return false;
    }

    int ok = 0, skipped = 0, failed = 0;
    for (size_t i = 0; i < _countof(g_hooks); ++i)
    {
        const HookEntry& e = g_hooks[i];
        void* target = ResolveTarget(e.name);
        if (!target)
        {
            ++skipped;
            LogText(L"funzione non trovata, saltata: %hs", e.name);
            continue;
        }
        st = MH_CreateHook(target, e.detour, e.original);
        if (st != MH_OK)
        {
            ++failed;
            LogText(L"MH_CreateHook %hs: %hs", e.name, MH_StatusToString(st));
            continue;
        }
        ++ok;
    }

    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK)
    {
        LogText(L"MH_EnableHook fallita: %hs", MH_StatusToString(st));
        return false;
    }
    LogText(L"hook attivi: %d, saltati: %d, falliti: %d", ok, skipped, failed);
    return true;
}