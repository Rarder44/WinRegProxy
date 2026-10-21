// reg_hooks.cpp
// Fase 1: pass-through. Ogni funzione Reg* viene inoltrata al registro vero e
// la chiamata viene scritta in reg_hook.log (accanto alla DLL).
// Il comportamento del programma NON cambia.
#include "pch.h"
#include "REG_adapter.h"
#include <intrin.h>
#include <sddl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <wchar.h>

// Definizione dei Real_* (dichiarati extern in reg_types.h, incluso da pch.h).
// MinHook li riempie in InstallRegHooks (MH_CreateHook, e.original = &Real_##NAME).
#define DEF_REAL(NAME) NAME##_t Real_##NAME = nullptr
DEF_REAL(RegOpenKeyExA);     DEF_REAL(RegOpenKeyExW);
DEF_REAL(RegOpenKeyA);       DEF_REAL(RegOpenKeyW);
DEF_REAL(RegCreateKeyExA);   DEF_REAL(RegCreateKeyExW);
DEF_REAL(RegCreateKeyA);     DEF_REAL(RegCreateKeyW);
DEF_REAL(RegCreateKeyTransactedA); DEF_REAL(RegCreateKeyTransactedW);
DEF_REAL(RegOpenKeyTransactedA);   DEF_REAL(RegOpenKeyTransactedW);
DEF_REAL(RegOpenCurrentUser);
DEF_REAL(RegOpenUserClassesRoot);
DEF_REAL(RegOverridePredefKey);
DEF_REAL(RegQueryValueExA);  DEF_REAL(RegQueryValueExW);
DEF_REAL(RegQueryValueA);    DEF_REAL(RegQueryValueW);
DEF_REAL(RegGetValueA);      DEF_REAL(RegGetValueW);
DEF_REAL(RegQueryInfoKeyA);  DEF_REAL(RegQueryInfoKeyW);
DEF_REAL(RegSetValueExA);    DEF_REAL(RegSetValueExW);
DEF_REAL(RegSetValueA);      DEF_REAL(RegSetValueW);
DEF_REAL(RegSetKeyValueA);   DEF_REAL(RegSetKeyValueW);
DEF_REAL(RegDeleteKeyA);     DEF_REAL(RegDeleteKeyW);
DEF_REAL(RegDeleteKeyExA);   DEF_REAL(RegDeleteKeyExW);
DEF_REAL(RegDeleteValueA);   DEF_REAL(RegDeleteValueW);
DEF_REAL(RegDeleteTreeA);    DEF_REAL(RegDeleteTreeW);
DEF_REAL(RegDeleteKeyValueA); DEF_REAL(RegDeleteKeyValueW);
DEF_REAL(RegEnumKeyExA);     DEF_REAL(RegEnumKeyExW);
DEF_REAL(RegEnumKeyA);       DEF_REAL(RegEnumKeyW);
DEF_REAL(RegEnumValueA);     DEF_REAL(RegEnumValueW);
DEF_REAL(RegCloseKey);
DEF_REAL(RegFlushKey);
DEF_REAL(RegNotifyChangeKeyValue);
DEF_REAL(RegCopyTreeA);      DEF_REAL(RegCopyTreeW);
DEF_REAL(RegReplaceKeyA);    DEF_REAL(RegReplaceKeyW);
DEF_REAL(RegRestoreKeyA);    DEF_REAL(RegRestoreKeyW);
DEF_REAL(RegSaveKeyA);       DEF_REAL(RegSaveKeyW);
DEF_REAL(RegSaveKeyExA);     DEF_REAL(RegSaveKeyExW);
DEF_REAL(RegGetKeySecurity); DEF_REAL(RegSetKeySecurity);
DEF_REAL(RegConnectRegistryA); DEF_REAL(RegConnectRegistryW);
DEF_REAL(RegDisconnectRegistry);
#undef DEF_REAL


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

// issue 60: l'open passa dall'adattatore (REG_adapter::OpenKey). Null finche'
// InstallRegHooks non inizializza; il detour lo usa se g_useAdapter.
static REG_adapter* g_adapter = nullptr;
static bool         g_useAdapter = false;

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

// ======================================================================= open virtuale (issue 60)
// Percorso assoluto "HKxx\\sub" (UTF-8, formato DB) della chiave (h, sub).
// false se il registro va comunque lasciato al reale: h non e' una root nota
// (es. id fittizio virtuale: sub e' gia' il percorso completo, non ricompilarlo),
// sub e' null (chiusura della "chiave corrente") o il nome supera MAX_PATH.
static bool ResolveKeyPath(HKEY h, const wchar_t* sub, std::string* out)
{
    const wchar_t* tok = nullptr;
    if (h == HKEY_CURRENT_USER)        tok = L"HKCU";
    else if (h == HKEY_LOCAL_MACHINE)  tok = L"HKLM";
    else if (h == HKEY_CLASSES_ROOT)   tok = L"HKCR";
    else if (h == HKEY_USERS)          tok = L"HKU";
    else if (h == HKEY_PERFORMANCE_DATA) tok = L"HKPD";
    else if (h == HKEY_CURRENT_CONFIG) tok = L"HKCC";
    else if (h == HKEY_DYN_DATA)       tok = L"HKDD";
    else return false;               // root sconosciuta/fittizia: reale
    if (!sub) return false;
    size_t subLen = wcslen(sub);
    if (subLen >= MAX_PATH) return false;
    wchar_t full[MAX_PATH];
    _snwprintf_s(full, _countof(full), _TRUNCATE, L"%s\\%s", tok, sub);
    int n = WideCharToMultiByte(CP_UTF8, 0, full, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return false;
    out->resize((size_t)n - 1);
    WideCharToMultiByte(CP_UTF8, 0, full, -1, &(*out)[0], n, nullptr, nullptr);
    return true;
}

// Detour "manuale" per le 4 funzioni open (in luogo della macro HOOK): stessa
// forma del pass-through, ma l'instradamento viene da REG_adapter::OpenKey
// (issue 41). Gli id fittizi vanno verso l'app, i Real_* restano dentro la DLL.

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
// issue 60: le 4 funzioni open passano dall'adattatore (REG_adapter::OpenKey,
// issue 41). Stessa forma del pass-through della macro HOOK; se l'open non va
// dall'adattatore (g_useAdapter false, out==null, h fittizio, sub null) e'
// Real_Reg* con gli argomenti originali: comportamento identico al reale.
static LSTATUS WINAPI D_RegOpenKeyExW(HKEY h, LPCWSTR sub, DWORD opt, REGSAM sam, PHKEY out)
{
    void* ra = _ReturnAddress();
    LSTATUS r = ERROR_SUCCESS;
    if (g_useAdapter && out && g_adapter)
    {
        std::string p;
        bool res = ResolveKeyPath(h, sub, &p);
        // issue 68: parent fittizio (non root) -> risolvi l'id via adattatore
        if (!res) res = g_adapter->ResolveId(h, sub ? sub : L"", &p);
        if (res)
        {
            unsigned id = 0;
            if (g_adapter->OpenKey(p, sam, r, id)) *out = (HKEY)(UINT_PTR)id;
            else *out = NULL;
        }
        else r = Real_RegOpenKeyExW(h, sub, opt, sam, out);
    }
    else r = Real_RegOpenKeyExW(h, sub, opt, sam, out);

    if (!t_busy)
    {
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller_[40];
        if (ShouldLog(ra, caller_))
            LogCall(caller_, "RegOpenKeyExW", r, L"key=%s sub=\"%s\" opt=0x%X sam=0x%X -> %s",
                KN(h), WS(sub), (unsigned)opt, (unsigned)sam, OutKN(r, out));
        t_busy = false;
        SetLastError(le);
    }
    return r;
}

static LSTATUS WINAPI D_RegOpenKeyExA(HKEY h, LPCSTR sub, DWORD opt, REGSAM sam, PHKEY out)
{
    wchar_t wsub[512];
    int n = sub ? MultiByteToWideChar(CP_ACP, 0, sub, -1, wsub, _countof(wsub)) : 0;
    if (sub && n <= 0) wsub[0] = 0;
    return D_RegOpenKeyExW(h, sub ? wsub : nullptr, opt, sam, out);
}

static LSTATUS WINAPI D_RegOpenKeyW(HKEY h, LPCWSTR sub, PHKEY out)
{
    void* ra = _ReturnAddress();
    LSTATUS r = ERROR_SUCCESS;
    if (g_useAdapter && out && g_adapter)
    {
        std::string p;
        bool res = ResolveKeyPath(h, sub, &p);
        // issue 68: parent fittizio (non root) -> risolvi l'id via adattatore
        if (!res) res = g_adapter->ResolveId(h, sub ? sub : L"", &p);
        if (res)
        {
            unsigned id = 0;
            if (g_adapter->OpenKey(p, KEY_READ, r, id)) *out = (HKEY)(UINT_PTR)id;
            else *out = NULL;
        }
        else r = Real_RegOpenKeyW(h, sub, out);
    }
    else r = Real_RegOpenKeyW(h, sub, out);

    if (!t_busy)
    {
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller_[40];
        if (ShouldLog(ra, caller_))
            LogCall(caller_, "RegOpenKeyW", r, L"key=%s sub=\"%s\" -> %s", KN(h), WS(sub), OutKN(r, out));
        t_busy = false;
        SetLastError(le);
    }
    return r;
}

static LSTATUS WINAPI D_RegOpenKeyA(HKEY h, LPCSTR sub, PHKEY out)
{
    wchar_t wsub[512];
    int n = sub ? MultiByteToWideChar(CP_ACP, 0, sub, -1, wsub, _countof(wsub)) : 0;
    if (sub && n <= 0) wsub[0] = 0;
    return D_RegOpenKeyW(h, sub ? wsub : nullptr, out);
}

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
    // issue 61: i create passano dall'adattatore (REG_adapter::CreateKey,
    // issue 42): la chiave va SOLO nel DB (handle virtuale), mai nel registro
    // reale. Stessa forma dell'open (60): se non gestito (g_useAdapter false,
    // out==null, sub==null, h fittizio/non risolvibile) e' Real_Reg* con gli
    // argomenti originali: comportamento identico al reale.
    // sub==NULL -> NON si virtualizza (create(NULL) = apertura della chiave
    // corrente: va al reale; cosi' la radice di una hive non diventa mai
    // "virtuale" e il comportamento resta quello reale).
    static LSTATUS WINAPI D_RegCreateKeyExW(HKEY h, LPCWSTR sub, DWORD res, LPWSTR cls, DWORD opt, REGSAM sam,
        LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp)
    {
        void* ra = _ReturnAddress();
        LSTATUS r = ERROR_SUCCESS;
        if (g_useAdapter && out && sub && g_adapter)
        {
            std::string p;
            bool res = ResolveKeyPath(h, sub, &p);
            // issue 68: parent fittizio (non root) -> risolvi l'id via adattatore
            if (!res) res = g_adapter->ResolveId(h, sub ? sub : L"", &p);
            if (res)
            {
                unsigned id = 0;
                if (g_adapter->CreateKey(p, disp, r, id)) *out = (HKEY)(UINT_PTR)id;
                else *out = NULL;
            }
            else r = Real_RegCreateKeyExW(h, sub, res, cls, opt, sam, sa, out, disp);
        }
        else r = Real_RegCreateKeyExW(h, sub, res, cls, opt, sam, sa, out, disp);

        if (!t_busy)
        {
            DWORD le = GetLastError();
            t_busy = true;
            wchar_t caller_[40];
            if (ShouldLog(ra, caller_))
                LogCall(caller_, "RegCreateKeyExW", r, L"key=%s sub=\"%s\" opt=0x%X sam=0x%X disp=%u -> %s",
                    KN(h), WS(sub), (unsigned)opt, (unsigned)sam,
                    (unsigned)(r == ERROR_SUCCESS ? D(disp) : 0), OutKN(r, out));
            t_busy = false;
            SetLastError(le);
        }
        return r;
    }

    static LSTATUS WINAPI D_RegCreateKeyExA(HKEY h, LPCSTR sub, DWORD res, LPSTR cls, DWORD opt, REGSAM sam,
        LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp)
    {
        wchar_t wsub[512], wcls[128];
        int n = sub ? MultiByteToWideChar(CP_ACP, 0, sub, -1, wsub, _countof(wsub)) : 0;
        if (sub && n <= 0) wsub[0] = 0;
        wchar_t* w = nullptr;   // class e' ignorata dal registro: conversione best-effort
        if (cls)
        {
            n = MultiByteToWideChar(CP_ACP, 0, cls, -1, wcls, _countof(wcls));
            if (n > 0) w = wcls;
        }
        return D_RegCreateKeyExW(h, sub ? wsub : nullptr, res, w, opt, sam, sa, out, disp);
    }

    static LSTATUS WINAPI D_RegCreateKeyW(HKEY h, LPCWSTR sub, PHKEY out)
    {
        void* ra = _ReturnAddress();
        LSTATUS r = ERROR_SUCCESS;
        if (g_useAdapter && out && sub && g_adapter)
        {
            std::string p;
            bool res = ResolveKeyPath(h, sub, &p);
            // issue 68: parent fittizio (non root) -> risolvi l'id via adattatore
            if (!res) res = g_adapter->ResolveId(h, sub ? sub : L"", &p);
            if (res)
            {
                unsigned id = 0;
                if (g_adapter->CreateKey(p, NULL, r, id)) *out = (HKEY)(UINT_PTR)id;
                else *out = NULL;
            }
            else r = Real_RegCreateKeyW(h, sub, out);
        }
        else r = Real_RegCreateKeyW(h, sub, out);

        if (!t_busy)
        {
            DWORD le = GetLastError();
            t_busy = true;
            wchar_t caller_[40];
            if (ShouldLog(ra, caller_))
                LogCall(caller_, "RegCreateKeyW", r, L"key=%s sub=\"%s\" -> %s", KN(h), WS(sub), OutKN(r, out));
            t_busy = false;
            SetLastError(le);
        }
        return r;
    }

    static LSTATUS WINAPI D_RegCreateKeyA(HKEY h, LPCSTR sub, PHKEY out)
    {
        wchar_t wsub[512];
        int n = sub ? MultiByteToWideChar(CP_ACP, 0, sub, -1, wsub, _countof(wsub)) : 0;
        if (sub && n <= 0) wsub[0] = 0;
        return D_RegCreateKeyW(h, sub ? wsub : nullptr, out);
    }

    static LSTATUS WINAPI D_RegCreateKeyTransactedW(HKEY h, LPCWSTR sub, DWORD res, LPWSTR cls, DWORD opt, REGSAM sam,
        LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp, HANDLE trans, PVOID ext)
    {
        // il ramo virtuale ignora trans/ext (la transazione non e' modellata);
        // il ramo reale li passa invariati.
        void* ra = _ReturnAddress();
        LSTATUS r = ERROR_SUCCESS;
        if (g_useAdapter && out && sub && g_adapter)
        {
            std::string p;
            bool res = ResolveKeyPath(h, sub, &p);
            // issue 68: parent fittizio (non root) -> risolvi l'id via adattatore
            if (!res) res = g_adapter->ResolveId(h, sub ? sub : L"", &p);
            if (res)
            {
                unsigned id = 0;
                if (g_adapter->CreateKey(p, disp, r, id)) *out = (HKEY)(UINT_PTR)id;
                else *out = NULL;
            }
            else r = Real_RegCreateKeyTransactedW(h, sub, res, cls, opt, sam, sa, out, disp, trans, ext);
        }
        else r = Real_RegCreateKeyTransactedW(h, sub, res, cls, opt, sam, sa, out, disp, trans, ext);

        if (!t_busy)
        {
            DWORD le = GetLastError();
            t_busy = true;
            wchar_t caller_[40];
            if (ShouldLog(ra, caller_))
                LogCall(caller_, "RegCreateKeyTransactedW", r,
                    L"key=%s sub=\"%s\" opt=0x%X sam=0x%X disp=%u trans=%p -> %s",
                    KN(h), WS(sub), (unsigned)opt, (unsigned)sam,
                    (unsigned)(r == ERROR_SUCCESS ? D(disp) : 0), trans, OutKN(r, out));
            t_busy = false;
            SetLastError(le);
        }
        return r;
    }

    static LSTATUS WINAPI D_RegCreateKeyTransactedA(HKEY h, LPCSTR sub, DWORD res, LPSTR cls, DWORD opt, REGSAM sam,
        LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp, HANDLE trans, PVOID ext)
    {
        wchar_t wsub[512], wcls[128];
        int n = sub ? MultiByteToWideChar(CP_ACP, 0, sub, -1, wsub, _countof(wsub)) : 0;
        if (sub && n <= 0) wsub[0] = 0;
        wchar_t* w = nullptr;   // class ignorata: conversione best-effort
        if (cls)
        {
            n = MultiByteToWideChar(CP_ACP, 0, cls, -1, wcls, _countof(wcls));
            if (n > 0) w = wcls;
        }
        return D_RegCreateKeyTransactedW(h, sub ? wsub : nullptr, res, w, opt, sam, sa, out, disp, trans, ext);
    }

    // ----------------------------------------------------------------------- Query
    // issue 62: la famiglia read passa dall'adattatore (REG_adapter::QueryValue,
    // issue 43): l'adattatore risolve "id+sub -> stato + dati" (DB primo, poi
    // reale se il valore non c'e' nel DB); ogni detour applica le semantica
    // buffer della propria famiglia (QVEx 2-pass, QV exact, GV +2/NUL quirk) e
    // le conversioni A<->W (i dati dell'adattatore sono wide; i detour A
    // convertono in CP_ACP). Non gestita (DB non aperto, id sconosciuto):
    // Real_Reg* con gli argomenti originali (comportamento identico al reale).
    static bool IsStrType(DWORD t)
    {
        return t == REG_SZ || t == REG_EXPAND_SZ || t == REG_MULTI_SZ;
    }

    // dati adattatore (stringhe: wide con NUL; altri: grezzi) -> byte per la API
    // A (stringhe in CP_ACP; non stringhe invariati).
    static std::string QVToA(DWORD type, const std::string& wide)
    {
        if (!IsStrType(type)) return wide;
        if (wide.empty()) return std::string();
        int n = WideCharToMultiByte(CP_ACP, 0, (const wchar_t*)wide.data(), (int)(wide.size() / sizeof(wchar_t)),
            nullptr, 0, nullptr, nullptr);
        if (n <= 0) return std::string();
        std::string out((size_t)n, '\0');
        WideCharToMultiByte(CP_ACP, 0, (const wchar_t*)wide.data(), (int)(wide.size() / sizeof(wchar_t)), &out[0], n, nullptr, nullptr);
        return out;
    }

    // Semantica QVEx (probe p1-2): cb==NULL -> 87; data==NULL -> sizing exact
    // (niente +2); *cb < required -> MORE_DATA (cb = required); success: copia
    // + cb = required. Fallita: type/cb non toccate (come il reale).
    static LSTATUS ApplyQVEx(const REG_adapter::RegQueryOut& out, LPDWORD type, LPBYTE data, LPDWORD cb)
    {
        if (!cb) return ERROR_INVALID_PARAMETER;
        if (out.status != ERROR_SUCCESS) return out.status;
        DWORD req = (DWORD)out.data.size();
        if (type) *type = out.type;
        if (!data) { *cb = req; return ERROR_SUCCESS; }
        if (*cb < req) { *cb = req; return ERROR_MORE_DATA; }
        if (req) memcpy(data, out.data.data(), req);
        *cb = req;
        return ERROR_SUCCESS;
    }

    static LSTATUS ApplyQVExA(const REG_adapter::RegQueryOut& out, LPDWORD type, LPBYTE data, LPDWORD cb)
    {
        if (!cb) return ERROR_INVALID_PARAMETER;
        if (out.status != ERROR_SUCCESS) return out.status;
        std::string a = QVToA(out.type, out.data);
        DWORD req = (DWORD)a.size();
        if (type) *type = out.type;
        if (!data) { *cb = req; return ERROR_SUCCESS; }
        if (*cb < req) { *cb = req; return ERROR_MORE_DATA; }
        if (req) memcpy(data, a.data(), req);
        *cb = req;
        return ERROR_SUCCESS;
    }

    // Semantica QV deprecated (probe p2-3): solo stringhe (altri -> 13, come
    // il reale); data==NULL -> sizing exact (valore vuoto: 2, come il reale);
    // buffer piccolo -> MORE_DATA (lcb = required); success: copia + lcb =
    // required (exact, niente +2).
    static LSTATUS ApplyQV(const REG_adapter::RegQueryOut& out, LPWSTR data, PLONG lcb)
    {
        if (!lcb) return ERROR_INVALID_PARAMETER;
        if (out.status != ERROR_SUCCESS) return out.status;
        if (!IsStrType(out.type)) return ERROR_ACCESS_DENIED;
        DWORD req = out.data.empty() ? 2 : (DWORD)out.data.size();
        if (!data) { *lcb = (LONG)req; return ERROR_SUCCESS; }
        if (*lcb < (LONG)req) { *lcb = (LONG)req; return ERROR_MORE_DATA; }
        if (out.data.empty()) ZeroMemory(data, 2);
        else memcpy(data, out.data.data(), req);
        *lcb = (LONG)req;
        return ERROR_SUCCESS;
    }

    static LSTATUS ApplyQVA(const REG_adapter::RegQueryOut& out, LPSTR data, PLONG lcb)
    {
        if (!lcb) return ERROR_INVALID_PARAMETER;
        if (out.status != ERROR_SUCCESS) return out.status;
        if (!IsStrType(out.type)) return ERROR_ACCESS_DENIED;
        std::string a = QVToA(out.type, out.data);
        DWORD req = a.empty() ? 1 : (DWORD)a.size();   // ANSI: vuoto = 1 byte (NUL)
        if (!data) { *lcb = (LONG)req; return ERROR_SUCCESS; }
        if (*lcb < (LONG)req) { *lcb = (LONG)req; return ERROR_MORE_DATA; }
        if (a.empty()) ZeroMemory(data, 1);
        else memcpy(data, a.data(), req);
        *lcb = (LONG)req;
        return ERROR_SUCCESS;
    }

    // Semantica GV (probe p1-5): sizing (pvData NULL, stringhe): cb =
    // required+2 (quirk, riservato all'extra NUL); buffer troppo piccolo:
    // MORE_DATA cb = required+2; success: cb = required exact (niente +2).
    // RRF_RT_* mismatch -> 1630.
    static DWORD RtfBit(DWORD type)
    {
        switch (type)
        {
        case REG_NONE:        return 0x1;
        case REG_SZ:          return 0x2;
        case REG_EXPAND_SZ:   return 0x4;
        case REG_BINARY:      return 0x8;
        case REG_DWORD:       return 0x10;
        case REG_MULTI_SZ:    return 0x20;
        case REG_QWORD:       return 0x40;
        }
        return 0;
    }

    static bool RtfMatches(DWORD rt, DWORD type, DWORD flags)
    {
        if (rt == 0x0000FFFF) return true;    // RRF_RT_ANY
        bool m = (rt & RtfBit(type)) != 0;
        // EXPAND_SZ soddisfa RRF_RT_REG_SZ se senza RRF_NOEXPAND (il valore
        // viene riportato come SZ espanso).
        if (!m && type == REG_EXPAND_SZ && (rt & 0x2) && !(flags & 0x10000000)) m = true;
        return m;
    }

    static LSTATUS ApplyGV(const REG_adapter::RegQueryOut& out, DWORD flags, LPDWORD type,
        PVOID data, LPDWORD cb, bool ansi)
    {
        if (out.status != ERROR_SUCCESS)
        {
            if ((flags & 0x20000000) && data && cb && *cb) ZeroMemory(data, *cb);   // RRF_ZEROONFAILURE
            return out.status;
        }
        DWORD rt = flags & 0x0000FFFF;
        if (!RtfMatches(rt, out.type, flags))
        {
            if ((flags & 0x20000000) && data && cb && *cb) ZeroMemory(data, *cb);
            return ERROR_INVALID_DATA;
        }
        DWORD repType = out.type;
        if (out.type == REG_EXPAND_SZ && (rt & 0x2) && !(flags & 0x10000000))
            repType = REG_SZ;   // espanso: riportato come SZ
        std::string a = ansi ? QVToA(out.type, out.data) : out.data;
        bool str = IsStrType(out.type);
        DWORD req = (DWORD)a.size();
        if (type) *type = repType;
        if (!data)
        {
            if (cb) *cb = req + (str ? 2 : 0);   // quirk +2 (stringhe)
            return ERROR_SUCCESS;
        }
        if (cb && *cb < req + (str ? 2 : 0))
        {
            if (cb) *cb = req + (str ? 2 : 0);   // quirk +2 (stringhe)
            return ERROR_MORE_DATA;
        }
        if (req) memcpy(data, a.data(), req);
        if (cb) *cb = req;
        return ERROR_SUCCESS;
    }

    // ============================ detour ============================
    static LSTATUS WINAPI D_RegQueryValueExW(HKEY h, LPCWSTR name, LPDWORD rsv, LPDWORD type, LPBYTE data, LPDWORD cb)
    {
        void* ra = _ReturnAddress();
        LSTATUS r = ERROR_SUCCESS;
        REG_adapter::RegQueryOut out;
        if (g_useAdapter && g_adapter && g_adapter->QueryValue(h, L"", name ? name : L"", &out))
            r = ApplyQVEx(out, type, data, cb);
        else r = Real_RegQueryValueExW(h, name, rsv, type, data, cb);

        if (!t_busy)
        {
            DWORD le = GetLastError();
            t_busy = true;
            wchar_t caller_[40];
            if (ShouldLog(ra, caller_))
                LogCall(caller_, "RegQueryValueExW", r, L"key=%s value=\"%s\" type=%s size=%u data=%s",
                    KN(h), VN(name), TypeR(r, type), (unsigned)D(cb), OutData(r, type, data, cb, false));
            t_busy = false;
            SetLastError(le);
        }
        return r;
    }

    static LSTATUS WINAPI D_RegQueryValueExA(HKEY h, LPCSTR name, LPDWORD rsv, LPDWORD type, LPBYTE data, LPDWORD cb)
    {
        void* ra = _ReturnAddress();
        LSTATUS r = ERROR_SUCCESS;
        std::wstring wn;
        if (name)
        {
            int n = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
            if (n > 0)
            {
                wn.resize((size_t)n - 1);
                MultiByteToWideChar(CP_ACP, 0, name, -1, &wn[0], n);
            }
        }
        REG_adapter::RegQueryOut out;
        if (g_useAdapter && g_adapter && g_adapter->QueryValue(h, L"", name ? wn.c_str() : L"", &out))
            r = ApplyQVExA(out, type, data, cb);
        else r = Real_RegQueryValueExA(h, name, rsv, type, data, cb);

        if (!t_busy)
        {
            DWORD le = GetLastError();
            t_busy = true;
            wchar_t caller_[40];
            if (ShouldLog(ra, caller_))
                LogCall(caller_, "RegQueryValueExA", r, L"key=%s value=\"%s\" type=%s size=%u data=%s",
                    KN(h), VNA(name), TypeR(r, type), (unsigned)D(cb), OutData(r, type, data, cb, true));
            t_busy = false;
            SetLastError(le);
        }
        return r;
    }

    static LSTATUS WINAPI D_RegQueryValueW(HKEY h, LPCWSTR sub, LPWSTR data, PLONG cb)
    {
        void* ra = _ReturnAddress();
        LSTATUS r = ERROR_SUCCESS;
        REG_adapter::RegQueryOut out;
        if (g_useAdapter && g_adapter && g_adapter->QueryValue(h, sub ? sub : L"", L"", &out))
            r = ApplyQV(out, data, (LONG*)cb);
        else r = Real_RegQueryValueW(h, sub, data, cb);

        if (!t_busy)
        {
            DWORD le = GetLastError();
            t_busy = true;
            wchar_t caller_[40];
            if (ShouldLog(ra, caller_))
                LogCall(caller_, "RegQueryValueW", r, L"key=%s sub=\"%s\" size=%ld data=%s",
                    KN(h), WS(sub), (long)((r == ERROR_SUCCESS && cb) ? *cb : 0), OutStr(r, data, cb, false));
            t_busy = false;
            SetLastError(le);
        }
        return r;
    }

    static LSTATUS WINAPI D_RegQueryValueA(HKEY h, LPCSTR sub, LPSTR data, PLONG cb)
    {
        void* ra = _ReturnAddress();
        LSTATUS r = ERROR_SUCCESS;
        std::wstring ws;
        if (sub)
        {
            int n = MultiByteToWideChar(CP_ACP, 0, sub, -1, nullptr, 0);
            if (n > 0)
            {
                ws.resize((size_t)n - 1);
                MultiByteToWideChar(CP_ACP, 0, sub, -1, &ws[0], n);
            }
        }
        REG_adapter::RegQueryOut out;
        if (g_useAdapter && g_adapter && g_adapter->QueryValue(h, sub ? ws.c_str() : L"", L"", &out))
            r = ApplyQVA(out, data, (LONG*)cb);
        else r = Real_RegQueryValueA(h, sub, data, cb);

        if (!t_busy)
        {
            DWORD le = GetLastError();
            t_busy = true;
            wchar_t caller_[40];
            if (ShouldLog(ra, caller_))
                LogCall(caller_, "RegQueryValueA", r, L"key=%s sub=\"%s\" size=%ld data=%s",
                    KN(h), AS(sub), (long)((r == ERROR_SUCCESS && cb) ? *cb : 0), OutStr(r, data, cb, true));
            t_busy = false;
            SetLastError(le);
        }
        return r;
    }

    static LSTATUS WINAPI D_RegGetValueW(HKEY h, LPCWSTR sub, LPCWSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD cb)
    {
        void* ra = _ReturnAddress();
        LSTATUS r = ERROR_SUCCESS;
        REG_adapter::RegQueryOut out;
        if (g_useAdapter && g_adapter &&
            g_adapter->QueryValue(h, sub ? sub : L"", name ? name : L"", &out))
            r = ApplyGV(out, flags, type, data, cb, false);
        else r = Real_RegGetValueW(h, sub, name, flags, type, data, cb);

        if (!t_busy)
        {
            DWORD le = GetLastError();
            t_busy = true;
            wchar_t caller_[40];
            if (ShouldLog(ra, caller_))
                LogCall(caller_, "RegGetValueW", r,
                    L"key=%s sub=\"%s\" value=\"%s\" flags=0x%X type=%s size=%u data=%s",
                    KN(h), WS(sub), VN(name), (unsigned)flags, TypeR(r, type), (unsigned)D(cb), OutData(r, type, data, cb, false));
            t_busy = false;
            SetLastError(le);
        }
        return r;
    }

    static LSTATUS WINAPI D_RegGetValueA(HKEY h, LPCSTR sub, LPCSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD cb)
    {
        void* ra = _ReturnAddress();
        LSTATUS r = ERROR_SUCCESS;
        std::wstring ws, wn;
        if (sub)
        {
            int n = MultiByteToWideChar(CP_ACP, 0, sub, -1, nullptr, 0);
            if (n > 0)
            {
                ws.resize((size_t)n - 1);
                MultiByteToWideChar(CP_ACP, 0, sub, -1, &ws[0], n);
            }
        }
        if (name)
        {
            int n = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
            if (n > 0)
            {
                wn.resize((size_t)n - 1);
                MultiByteToWideChar(CP_ACP, 0, name, -1, &wn[0], n);
            }
        }
        REG_adapter::RegQueryOut out;
        if (g_useAdapter && g_adapter &&
            g_adapter->QueryValue(h, sub ? ws.c_str() : L"", name ? wn.c_str() : L"", &out))
            r = ApplyGV(out, flags, type, data, cb, true);
        else r = Real_RegGetValueA(h, sub, name, flags, type, data, cb);

        if (!t_busy)
        {
            DWORD le = GetLastError();
            t_busy = true;
            wchar_t caller_[40];
            if (ShouldLog(ra, caller_))
                LogCall(caller_, "RegGetValueA", r,
                    L"key=%s sub=\"%s\" value=\"%s\" flags=0x%X type=%s size=%u data=%s",
                    KN(h), AS(sub), VNA(name), (unsigned)flags, TypeR(r, type), (unsigned)D(cb), OutData(r, type, data, cb, true));
            t_busy = false;
            SetLastError(le);
        }
        return r;
    }

    // ----------------------------------------------------------------------- QueryInfoKey
    // Vista unita DB+reale (49/66): chiave gestita (id fittizio) -> info
    // dall'adattatore; id non gestito -> reale (fase 1).
    // ATTENZIONE: i NOMI dei parametri nel typedef sono spostati (reg_types.h);
    // l'ordine REALE (SDK) e': hKey, lpClass, lpcchClass, lpReserved,
    // lpcSubKeys, lpcbMaxSubKeyLen, lpcbMaxClassLen, lpcValues,
    // lpcbMaxValueNameLen, lpcbMaxValueLen, lpcbSecurityDescriptor,
    // lpftLastWriteTime. Quindi: cval=lpcSubKeys, maxval=lpcValues,
    // maxlen=lpcbMaxValueName, secname=lpcbMaxValueLen, seclen=lpcbSecurity.
    static LSTATUS WINAPI D_RegQueryInfoKeyW(HKEY h, LPWSTR name, LPDWORD rsv, LPDWORD csub, LPDWORD cval, LPDWORD maxsub, LPDWORD maxname, LPDWORD maxval, LPDWORD maxlen, LPDWORD secname, LPDWORD seclen, PFILETIME ft)
    {
        if (t_busy) return Real_RegQueryInfoKeyW(h, name, rsv, csub, cval, maxsub, maxname, maxval, maxlen, secname, seclen, ft);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        t_busy = false;
        SetLastError(le);

        LSTATUS r = ERROR_ACCESS_DENIED;
        if (g_useAdapter && g_adapter)
        {
            REG_adapter::RegQueryInfoOut eo;
            if (g_adapter->QueryInfo(h, &eo))
            {
                r = eo.status;
                if (r == ERROR_SUCCESS)
                {
                    if (rsv) *rsv = 0;                              // lpcchClass (classe vuota)
                    if (cval) *cval = eo.numSubkeys;                // lpcSubKeys
                    if (maxsub) *maxsub = eo.maxSubkeyNameLen;      // lpcbMaxSubKeyLen
                    if (maxname) *maxname = 0;                      // lpcbMaxClassLen
                    if (maxval) *maxval = eo.numValues;             // lpcValues
                    if (maxlen) *maxlen = eo.maxValueNameLen;       // lpcbMaxValueNameLen
                    if (secname) *secname = eo.maxValueDataLen;     // lpcbMaxValueLen
                    if (seclen) *seclen = eo.securityDescLen;       // lpcbSecurityDescriptor
                    if (ft) { ft->dwLowDateTime = eo.lastWrite.dwLowDateTime; ft->dwHighDateTime = eo.lastWrite.dwHighDateTime; }
                }
            }
            else r = Real_RegQueryInfoKeyW(h, name, rsv, csub, cval, maxsub, maxname, maxval, maxlen, secname, seclen, ft);
        }
        else r = Real_RegQueryInfoKeyW(h, name, rsv, csub, cval, maxsub, maxname, maxval, maxlen, secname, seclen, ft);

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegQueryInfoKeyW", r, L"key=%s", KN(h));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegQueryInfoKeyA(HKEY h, LPSTR name, LPDWORD rsv, LPDWORD csub, LPDWORD cval, LPDWORD maxsub, LPDWORD maxname, LPDWORD maxval, LPDWORD maxlen, LPDWORD secname, LPDWORD seclen, PFILETIME ft)
    {
        if (t_busy) return Real_RegQueryInfoKeyA(h, name, rsv, csub, cval, maxsub, maxname, maxval, maxlen, secname, seclen, ft);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        t_busy = false;
        SetLastError(le);

        LSTATUS r = ERROR_ACCESS_DENIED;
        if (g_useAdapter && g_adapter)
        {
            REG_adapter::RegQueryInfoOut eo;
            if (g_adapter->QueryInfo(h, &eo))
            {
                r = eo.status;
                if (r == ERROR_SUCCESS)
                {
                    if (rsv) *rsv = 0;
                    if (cval) *cval = eo.numSubkeys;
                    if (maxsub) *maxsub = eo.maxSubkeyNameLen;
                    if (maxname) *maxname = 0;
                    if (maxval) *maxval = eo.numValues;
                    if (maxlen) *maxlen = eo.maxValueNameLen;
                    if (secname) *secname = eo.maxValueDataLen;
                    if (seclen) *seclen = eo.securityDescLen;
                    if (ft) { ft->dwLowDateTime = eo.lastWrite.dwLowDateTime; ft->dwHighDateTime = eo.lastWrite.dwHighDateTime; }
                }
            }
            else r = Real_RegQueryInfoKeyA(h, name, rsv, csub, cval, maxsub, maxname, maxval, maxlen, secname, seclen, ft);
        }
        else r = Real_RegQueryInfoKeyA(h, name, rsv, csub, cval, maxsub, maxname, maxval, maxlen, secname, seclen, ft);

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegQueryInfoKeyA", r, L"key=%s", KN(h));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    // ----------------------------------------------------------------------- Set
    // Scrittura via adattatore (44/63): chiave gestita (id fittizio) o root
    // nota -> SOLO nel DB; id sconosciuto -> reale (fase 1).
    // NormSetData: normalizza i dati al formato atteso dall'adattatore
    // (stringhe -> UTF-16LE, ANSI convertita; non-stringa -> byte grezzi).
    static std::string NormSetData(DWORD type, const void* data, DWORD cb, bool ansi)
    {
        if (!data) return std::string();
        bool isStr = (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ);
        if (cb == 0 && isStr)
            cb = ansi ? (DWORD)(strlen((const char*)data) + 1)
                      : (DWORD)((wcslen((const wchar_t*)data) + 1) * sizeof(wchar_t));
        if (cb == 0) return std::string();
        if (isStr && ansi)
        {
            int n = MultiByteToWideChar(CP_ACP, 0, (const char*)data, (int)cb, nullptr, 0);
            if (n <= 0) return std::string();
            std::wstring w((size_t)n, L'\x00');
            MultiByteToWideChar(CP_ACP, 0, (const char*)data, (int)cb, &w[0], n);
            return std::string((const char*)w.data(), w.size() * sizeof(wchar_t));
        }
        return std::string((const char*)data, cb);
    }

    static LSTATUS WINAPI D_RegSetValueExW(HKEY h, LPCWSTR name, DWORD rsv, DWORD type, const BYTE* data, DWORD cb)
    {
        if (t_busy) return Real_RegSetValueExW(h, name, rsv, type, data, cb);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        std::string nd = NormSetData(type, data, cb, false);
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegSetOut so{ERROR_SUCCESS, false};
        LSTATUS r = (g_useAdapter && g_adapter && g_adapter->SetValue(h, L"", name ? name : L"", type, nd, &so))
                     ? so.status
                     : Real_RegSetValueExW(h, name, rsv, type, data, cb);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegSetValueExW", r, L"key=%s value=\"%s\" type=%s size=%u data=%s",
                    KN(h), VN(name), TypeName(type), (unsigned)cb, SetData(r, type, data, cb, false));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegSetValueExA(HKEY h, LPCSTR name, DWORD rsv, DWORD type, const BYTE* data, DWORD cb)
    {
        if (t_busy) return Real_RegSetValueExA(h, name, rsv, type, data, cb);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        std::wstring wname;
        if (name) { int n = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0); if (n > 0) { wname.assign(n - 1, L'\x00'); MultiByteToWideChar(CP_ACP, 0, name, -1, &wname[0], n); } }
        std::string nd = NormSetData(type, data, cb, true);
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegSetOut so{ERROR_SUCCESS, false};
        LSTATUS r = (g_useAdapter && g_adapter && g_adapter->SetValue(h, L"", wname, type, nd, &so))
                     ? so.status
                     : Real_RegSetValueExA(h, name, rsv, type, data, cb);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegSetValueExA", r, L"key=%s value=\"%s\" type=%s size=%u data=%s",
                    KN(h), VNA(name), TypeName(type), (unsigned)cb, SetData(r, type, data, cb, true));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegSetValueW(HKEY h, LPCWSTR name, DWORD type, LPCWSTR data, DWORD cb)
    {
        if (t_busy) return Real_RegSetValueW(h, name, type, data, cb);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        std::string nd = NormSetData(type, data ? (const void*)data : nullptr, cb, false);
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegSetOut so{ERROR_SUCCESS, false};
        LSTATUS r = (g_useAdapter && g_adapter && g_adapter->SetValue(h, L"", name ? name : L"", type, nd, &so))
                     ? so.status
                     : Real_RegSetValueW(h, name, type, data, cb);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegSetValueW", r, L"key=%s value=\"%s\" type=%s data=\"%s\"",
                    KN(h), VN(name), TypeName(type), (r == ERROR_SUCCESS ? WS(data) : L"-"));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegSetValueA(HKEY h, LPCSTR name, DWORD type, LPCSTR data, DWORD cb)
    {
        if (t_busy) return Real_RegSetValueA(h, name, type, data, cb);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        std::wstring wname;
        if (name) { int n = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0); if (n > 0) { wname.assign(n - 1, L'\x00'); MultiByteToWideChar(CP_ACP, 0, name, -1, &wname[0], n); } }
        std::string nd = NormSetData(type, data ? (const void*)data : nullptr, cb, true);
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegSetOut so{ERROR_SUCCESS, false};
        LSTATUS r = (g_useAdapter && g_adapter && g_adapter->SetValue(h, L"", wname, type, nd, &so))
                     ? so.status
                     : Real_RegSetValueA(h, name, type, data, cb);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegSetValueA", r, L"key=%s value=\"%s\" type=%s data=\"%s\"",
                    KN(h), VNA(name), TypeName(type), (r == ERROR_SUCCESS ? AS(data) : L"-"));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegSetKeyValueW(HKEY root, LPCWSTR sub, LPCWSTR name, DWORD type, LPCVOID data, DWORD cb)
    {
        if (t_busy) return Real_RegSetKeyValueW(root, sub, name, type, data, cb);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        std::string nd = NormSetData(type, data, cb, false);
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegSetOut so{ERROR_SUCCESS, false};
        LSTATUS r = (g_useAdapter && g_adapter && g_adapter->SetValue(root, sub ? sub : L"", name ? name : L"", type, nd, &so))
                     ? so.status
                     : Real_RegSetKeyValueW(root, sub, name, type, data, cb);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegSetKeyValueW", r, L"key=%s sub=\"%s\" value=\"%s\" type=%s size=%u data=%s",
                    KN(root), WS(sub), VN(name), TypeName(type), (unsigned)cb, SetData(r, type, data, cb, false));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegSetKeyValueA(HKEY root, LPCSTR sub, LPCSTR name, DWORD type, LPCVOID data, DWORD cb)
    {
        if (t_busy) return Real_RegSetKeyValueA(root, sub, name, type, data, cb);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        std::wstring wsub, wname;
        if (sub) { int n = MultiByteToWideChar(CP_ACP, 0, sub, -1, nullptr, 0); if (n > 0) { wsub.assign(n - 1, L'\x00'); MultiByteToWideChar(CP_ACP, 0, sub, -1, &wsub[0], n); } }
        if (name) { int n = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0); if (n > 0) { wname.assign(n - 1, L'\x00'); MultiByteToWideChar(CP_ACP, 0, name, -1, &wname[0], n); } }
        std::string nd = NormSetData(type, data, cb, true);
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegSetOut so{ERROR_SUCCESS, false};
        LSTATUS r = (g_useAdapter && g_adapter && g_adapter->SetValue(root, wsub, wname, type, nd, &so))
                     ? so.status
                     : Real_RegSetKeyValueA(root, sub, name, type, data, cb);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegSetKeyValueA", r, L"key=%s sub=\"%s\" value=\"%s\" type=%s size=%u data=%s",
                    KN(root), AS(sub), VNA(name), TypeName(type), (unsigned)cb, SetData(r, type, data, cb, true));
        t_busy = false;
        SetLastError(le);
        return r;
    }
    // ----------------------------------------------------------------------- Delete
    // Cancellazione via adattatore (45/46/64): riga nel DB -> SOLO nel DB
    // (soft-delete); assente dal DB: record Real/root -> reale (stessa
    // famiglia), record Virtual -> FILE_NOT_FOUND (gestito dall'adattatore).
    static LSTATUS WINAPI D_RegDeleteValueW(HKEY h, LPCWSTR name)
    {
        if (t_busy) return Real_RegDeleteValueW(h, name);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegDelOut so{ERROR_SUCCESS, false};
        LSTATUS r = (g_useAdapter && g_adapter && g_adapter->DeleteValue(h, L"", name ? name : L"", &so))
                     ? so.status
                     : Real_RegDeleteValueW(h, name);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegDeleteValueW", r, L"key=%s value=\"%s\"", KN(h), VN(name));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegDeleteValueA(HKEY h, LPCSTR name)
    {
        if (t_busy) return Real_RegDeleteValueA(h, name);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        std::wstring wname;
        if (name) { int n = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0); if (n > 0) { wname.assign(n - 1, L'\x00'); MultiByteToWideChar(CP_ACP, 0, name, -1, &wname[0], n); } }
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegDelOut so{ERROR_SUCCESS, false};
        LSTATUS r = (g_useAdapter && g_adapter && g_adapter->DeleteValue(h, L"", wname, &so))
                     ? so.status
                     : Real_RegDeleteValueA(h, name);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegDeleteValueA", r, L"key=%s value=\"%s\"", KN(h), VNA(name));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegDeleteKeyValueW(HKEY h, LPCWSTR sub, LPCWSTR name)
    {
        if (t_busy) return Real_RegDeleteKeyValueW(h, sub, name);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegDelOut so{ERROR_SUCCESS, false};
        LSTATUS r = (g_useAdapter && g_adapter && g_adapter->DeleteValue(h, sub ? sub : L"", name ? name : L"", &so))
                     ? so.status
                     : Real_RegDeleteKeyValueW(h, sub, name);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegDeleteKeyValueW", r, L"key=%s sub=\"%s\" value=\"%s\"", KN(h), WS(sub), VN(name));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegDeleteKeyValueA(HKEY h, LPCSTR sub, LPCSTR name)
    {
        if (t_busy) return Real_RegDeleteKeyValueA(h, sub, name);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        std::wstring wsub, wname;
        if (sub) { int n = MultiByteToWideChar(CP_ACP, 0, sub, -1, nullptr, 0); if (n > 0) { wsub.assign(n - 1, L'\x00'); MultiByteToWideChar(CP_ACP, 0, sub, -1, &wsub[0], n); } }
        if (name) { int n = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0); if (n > 0) { wname.assign(n - 1, L'\x00'); MultiByteToWideChar(CP_ACP, 0, name, -1, &wname[0], n); } }
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegDelOut so{ERROR_SUCCESS, false};
        LSTATUS r = (g_useAdapter && g_adapter && g_adapter->DeleteValue(h, wsub, wname, &so))
                     ? so.status
                     : Real_RegDeleteKeyValueA(h, sub, name);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegDeleteKeyValueA", r, L"key=%s sub=\"%s\" value=\"%s\"", KN(h), AS(sub), VNA(name));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegDeleteKeyW(HKEY h, LPCWSTR sub)
    {
        if (t_busy) return Real_RegDeleteKeyW(h, sub);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegDelOut so{ERROR_SUCCESS, false};
        LSTATUS r = (g_useAdapter && g_adapter && g_adapter->DeleteKey(h, sub ? sub : L"", &so))
                     ? so.status
                     : Real_RegDeleteKeyW(h, sub);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegDeleteKeyW", r, L"key=%s sub=\"%s\"", KN(h), WS(sub));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegDeleteKeyA(HKEY h, LPCSTR sub)
    {
        if (t_busy) return Real_RegDeleteKeyA(h, sub);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        std::wstring wsub;
        if (sub) { int n = MultiByteToWideChar(CP_ACP, 0, sub, -1, nullptr, 0); if (n > 0) { wsub.assign(n - 1, L'\x00'); MultiByteToWideChar(CP_ACP, 0, sub, -1, &wsub[0], n); } }
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegDelOut so{ERROR_SUCCESS, false};
        LSTATUS r = (g_useAdapter && g_adapter && g_adapter->DeleteKey(h, wsub, &so))
                     ? so.status
                     : Real_RegDeleteKeyA(h, sub);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegDeleteKeyA", r, L"key=%s sub=\"%s\"", KN(h), AS(sub));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegDeleteKeyExW(HKEY h, LPCWSTR sub, REGSAM sam, DWORD rsv)
    {
        if (t_busy) return Real_RegDeleteKeyExW(h, sub, sam, rsv);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegDelOut so{ERROR_SUCCESS, false};
        LSTATUS r = (g_useAdapter && g_adapter && g_adapter->DeleteKey(h, sub ? sub : L"", &so))
                     ? so.status
                     : Real_RegDeleteKeyExW(h, sub, sam, rsv);   // non gestita: reale (sam/rsv nel reale)

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegDeleteKeyExW", r, L"key=%s sub=\"%s\" sam=0x%X", KN(h), WS(sub), (unsigned)sam);
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegDeleteKeyExA(HKEY h, LPCSTR sub, REGSAM sam, DWORD rsv)
    {
        if (t_busy) return Real_RegDeleteKeyExA(h, sub, sam, rsv);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        std::wstring wsub;
        if (sub) { int n = MultiByteToWideChar(CP_ACP, 0, sub, -1, nullptr, 0); if (n > 0) { wsub.assign(n - 1, L'\x00'); MultiByteToWideChar(CP_ACP, 0, sub, -1, &wsub[0], n); } }
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegDelOut so{ERROR_SUCCESS, false};
        LSTATUS r = (g_useAdapter && g_adapter && g_adapter->DeleteKey(h, wsub, &so))
                     ? so.status
                     : Real_RegDeleteKeyExA(h, sub, sam, rsv);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegDeleteKeyExA", r, L"key=%s sub=\"%s\" sam=0x%X", KN(h), AS(sub), (unsigned)sam);
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegDeleteTreeW(HKEY h, LPCWSTR sub)
    {
        if (t_busy) return Real_RegDeleteTreeW(h, sub);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegDelOut so{ERROR_SUCCESS, false};
        LSTATUS r = (g_useAdapter && g_adapter && g_adapter->DeleteKey(h, sub ? sub : L"", &so))
                     ? so.status
                     : Real_RegDeleteTreeW(h, sub);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegDeleteTreeW", r, L"key=%s sub=\"%s\"", KN(h), WS(sub));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegDeleteTreeA(HKEY h, LPCSTR sub)
    {
        if (t_busy) return Real_RegDeleteTreeA(h, sub);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        std::wstring wsub;
        if (sub) { int n = MultiByteToWideChar(CP_ACP, 0, sub, -1, nullptr, 0); if (n > 0) { wsub.assign(n - 1, L'\x00'); MultiByteToWideChar(CP_ACP, 0, sub, -1, &wsub[0], n); } }
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegDelOut so{ERROR_SUCCESS, false};
        LSTATUS r = (g_useAdapter && g_adapter && g_adapter->DeleteKey(h, wsub, &so))
                     ? so.status
                     : Real_RegDeleteTreeA(h, sub);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegDeleteTreeA", r, L"key=%s sub=\"%s\"", KN(h), AS(sub));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    // ----------------------------------------------------------------------- Enumerate
    // ===== Enum (65: wiring; unione DB+reale nell'adattatore, 47/48) =====
    // Adattatore: true + status (SUCCESS con name[/type/data] | NO_MORE_ITEMS
    // | FILE_NOT_FOUND). false -> reale con gli argomenti originali.

    static LSTATUS WINAPI D_RegEnumKeyExW(HKEY h, DWORD idx, LPWSTR name, LPDWORD cch, LPDWORD rsv, LPWSTR cls, LPDWORD ccls, PFILETIME ft)
    {
        if (t_busy) return Real_RegEnumKeyExW(h, idx, name, cch, rsv, cls, ccls, ft);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegEnumOut eo{ERROR_SUCCESS, false, L"", 0, {}};
        LSTATUS r = ERROR_NO_MORE_ITEMS;
        if (g_useAdapter && g_adapter && g_adapter->EnumKey(h, idx, &eo))
        {
            if (eo.status == ERROR_SUCCESS)
            {
                DWORD need = (DWORD)eo.name.size() + 1;
                if (cch && *cch < need)      { r = ERROR_MORE_DATA; *cch = need; }
                else                        { if (name) memcpy(name, eo.name.c_str(), need * sizeof(WCHAR)); if (cch) *cch = need; r = ERROR_SUCCESS; }
                if (ft) { ft->dwLowDateTime = 0; ft->dwHighDateTime = 0; }   // virtuali: data non disponibile
            }
            else r = eo.status;
        }
        else r = Real_RegEnumKeyExW(h, idx, name, cch, rsv, cls, ccls, ft);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegEnumKeyExW", r, L"key=%s idx=%u name=\"%s\"",
                KN(h), (unsigned)idx, (r == ERROR_SUCCESS ? WS(name) : L"-"));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegEnumKeyExA(HKEY h, DWORD idx, LPSTR name, LPDWORD cch, LPDWORD rsv, LPSTR cls, LPDWORD ccls, PFILETIME ft)
    {
        if (t_busy) return Real_RegEnumKeyExA(h, idx, name, cch, rsv, cls, ccls, ft);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegEnumOut eo{ERROR_SUCCESS, false, L"", 0, {}};
        LSTATUS r = ERROR_NO_MORE_ITEMS;
        if (g_useAdapter && g_adapter && g_adapter->EnumKey(h, idx, &eo))
        {
            if (eo.status == ERROR_SUCCESS)
            {
                int need = WideCharToMultiByte(CP_ACP, 0, eo.name.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (cch && *cch < (DWORD)need)   { r = ERROR_MORE_DATA; *cch = (DWORD)need; }
                else
                {
                    if (name) WideCharToMultiByte(CP_ACP, 0, eo.name.c_str(), -1, name, (int)(cch ? *cch : (DWORD)lstrlenA(name)), nullptr, nullptr);
                    if (cch) *cch = (DWORD)need;
                    r = ERROR_SUCCESS;
                }
                if (ft) { ft->dwLowDateTime = 0; ft->dwHighDateTime = 0; }
            }
            else r = eo.status;
        }
        else r = Real_RegEnumKeyExA(h, idx, name, cch, rsv, cls, ccls, ft);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegEnumKeyExA", r, L"key=%s idx=%u name=\"%s\"",
                KN(h), (unsigned)idx, (r == ERROR_SUCCESS ? AS(name) : L"-"));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegEnumKeyW(HKEY h, DWORD idx, LPWSTR name, DWORD cch)
    {
        if (t_busy) return Real_RegEnumKeyW(h, idx, name, cch);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegEnumOut eo{ERROR_SUCCESS, false, L"", 0, {}};
        LSTATUS r = ERROR_NO_MORE_ITEMS;
        if (g_useAdapter && g_adapter && g_adapter->EnumKey(h, idx, &eo))
        {
            if (eo.status == ERROR_SUCCESS)
            {
                DWORD need = (DWORD)eo.name.size() + 1;
                if (cch < need)              { r = ERROR_MORE_DATA; }
                else                        { if (name) memcpy(name, eo.name.c_str(), need * sizeof(WCHAR)); r = ERROR_SUCCESS; }
            }
            else r = eo.status;
        }
        else r = Real_RegEnumKeyW(h, idx, name, cch);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegEnumKeyW", r, L"key=%s idx=%u name=\"%s\"",
                KN(h), (unsigned)idx, (r == ERROR_SUCCESS ? WS(name) : L"-"));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegEnumKeyA(HKEY h, DWORD idx, LPSTR name, DWORD cch)
    {
        if (t_busy) return Real_RegEnumKeyA(h, idx, name, cch);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegEnumOut eo{ERROR_SUCCESS, false, L"", 0, {}};
        LSTATUS r = ERROR_NO_MORE_ITEMS;
        if (g_useAdapter && g_adapter && g_adapter->EnumKey(h, idx, &eo))
        {
            if (eo.status == ERROR_SUCCESS)
            {
                int need = WideCharToMultiByte(CP_ACP, 0, eo.name.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (cch < (DWORD)need)       { r = ERROR_MORE_DATA; }
                else
                {
                    if (name) WideCharToMultiByte(CP_ACP, 0, eo.name.c_str(), -1, name, cch, nullptr, nullptr);
                    r = ERROR_SUCCESS;
                }
            }
            else r = eo.status;
        }
        else r = Real_RegEnumKeyA(h, idx, name, cch);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegEnumKeyA", r, L"key=%s idx=%u name=\"%s\"",
                KN(h), (unsigned)idx, (r == ERROR_SUCCESS ? AS(name) : L"-"));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegEnumValueW(HKEY h, DWORD idx, LPWSTR name, LPDWORD cch, LPDWORD rsv, LPDWORD type, LPBYTE data, LPDWORD cb)
    {
        if (t_busy) return Real_RegEnumValueW(h, idx, name, cch, rsv, type, data, cb);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegEnumOut eo{ERROR_SUCCESS, false, L"", 0, {}};
        LSTATUS r = ERROR_NO_MORE_ITEMS;
        if (g_useAdapter && g_adapter && g_adapter->EnumValue(h, idx, &eo))
        {
            if (eo.status == ERROR_SUCCESS)
            {
                DWORD needN = (DWORD)eo.name.size() + 1;
                DWORD needD = (DWORD)eo.data.size();
                if (cch && *cch < needN)          { r = ERROR_MORE_DATA; *cch = needN; }
                else if (cb && *cb < needD)       { r = ERROR_MORE_DATA; *cb = needD; }
                else
                {
                    if (name) memcpy(name, eo.name.c_str(), needN * sizeof(WCHAR));
                    if (cch) *cch = needN;
                    if (type) *type = eo.type;
                    if (data && needD) memcpy(data, eo.data.data(), needD);
                    if (cb) *cb = needD;
                    r = ERROR_SUCCESS;
                }
            }
            else r = eo.status;
        }
        else r = Real_RegEnumValueW(h, idx, name, cch, rsv, type, data, cb);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegEnumValueW", r, L"key=%s idx=%u name=\"%s\" type=%s data=%s",
                KN(h), (unsigned)idx, (r == ERROR_SUCCESS ? WS(name) : L"-"),
                TypeR(r, type), OutData(r, type, data, cb, false));
        t_busy = false;
        SetLastError(le);
        return r;
    }

    static LSTATUS WINAPI D_RegEnumValueA(HKEY h, DWORD idx, LPSTR name, LPDWORD cch, LPDWORD rsv, LPDWORD type, LPBYTE data, LPDWORD cb)
    {
        if (t_busy) return Real_RegEnumValueA(h, idx, name, cch, rsv, type, data, cb);
        DWORD le = GetLastError();
        t_busy = true;
        wchar_t caller[40];
        bool doLog = ShouldLog(_ReturnAddress(), caller);
        t_busy = false;
        SetLastError(le);

        REG_adapter::RegEnumOut eo{ERROR_SUCCESS, false, L"", 0, {}};
        LSTATUS r = ERROR_NO_MORE_ITEMS;
        if (g_useAdapter && g_adapter && g_adapter->EnumValue(h, idx, &eo))
        {
            if (eo.status == ERROR_SUCCESS)
            {
                DWORD needN = (DWORD)eo.name.size() + 1;
                DWORD needD = (DWORD)eo.data.size();
                if (cch && *cch < needN)          { r = ERROR_MORE_DATA; *cch = needN; }
                else if (cb && *cb < needD)       { r = ERROR_MORE_DATA; *cb = needD; }
                else
                {
                    if (name) WideCharToMultiByte(CP_ACP, 0, eo.name.c_str(), -1, name, (int)needN, nullptr, nullptr);
                    if (cch) *cch = needN;
                    if (type) *type = eo.type;
                    if (data && needD) memcpy(data, eo.data.data(), needD);
                    if (cb) *cb = needD;
                    r = ERROR_SUCCESS;
                }
            }
            else r = eo.status;
        }
        else r = Real_RegEnumValueA(h, idx, name, cch, rsv, type, data, cb);   // non gestita: reale

        t_busy = true;
        if (doLog)
            LogCall(caller, "RegEnumValueA", r, L"key=%s idx=%u name=\"%s\" type=%s data=%s",
                KN(h), (unsigned)idx, (r == ERROR_SUCCESS ? AS(name) : L"-"),
                TypeR(r, type), OutData(r, type, data, cb, true));
        t_busy = false;
        SetLastError(le);
        return r;
    }

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
    void* ra = _ReturnAddress();
    DWORD le = GetLastError();
    t_busy = true;
    wchar_t caller[40];
    bool doLog = (kLogClose && !t_busy) && ShouldLog(ra, caller);
    wchar_t name[SCR];
    name[0] = 0;
    if (doLog) wcsncpy_s(name, SCR, KN(h), _TRUNCATE);   // nome PRIMA della chiusura
    t_busy = false;
    SetLastError(le);

    // 50/67: l'h e' un id fittizio (tutti gli open passano per i detour e
    // registrano un record). Record Real -> Real_RegCloseKey del handle
    // memorizzato; Virtual -> solo rimozione. Id non gestito (handle esterni,
    // root) -> chiusura reale diretta.
    LSTATUS r;
    if (g_useAdapter && g_adapter && g_adapter->CloseKey(h, r))
        ;   // gestita
    else
        r = Real_RegCloseKey(h);

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

    // issue 60: l'open passa dall'adattatore (creato lazy qui; il DB si apre
    // accanto alla DLL o in %TEMP%). Se il DB non si apre, OpenKey degrada a
    // reale (pass-through) senza altri effetti.
    g_adapter = REG_adapter::getInstance(self);
    g_useAdapter = (g_adapter != nullptr);

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