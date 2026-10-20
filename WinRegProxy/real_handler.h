#pragma once

// Open / create
typedef LSTATUS(WINAPI* RegOpenKeyExA_t)(
    HKEY, LPCSTR, DWORD, REGSAM, PHKEY);

typedef LSTATUS(WINAPI* RegOpenKeyExW_t)(
    HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);

typedef LSTATUS(WINAPI* RegCreateKeyExA_t)(
    HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM,
    const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);

typedef LSTATUS(WINAPI* RegCreateKeyExW_t)(
    HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM,
    const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);


// Query
typedef LSTATUS(WINAPI* RegQueryValueExA_t)(
    HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);

typedef LSTATUS(WINAPI* RegQueryValueExW_t)(
    HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);

typedef LSTATUS(WINAPI* RegQueryInfoKeyA_t)(
    HKEY, LPSTR, LPDWORD, LPDWORD, LPDWORD, LPDWORD,
    LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPSTR, PFILETIME);

typedef LSTATUS(WINAPI* RegQueryInfoKeyW_t)(
    HKEY, LPWSTR, LPDWORD, LPDWORD, LPDWORD, LPDWORD,
    LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPWSTR, PFILETIME);


// Set
typedef LSTATUS(WINAPI* RegSetValueExA_t)(
    HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD);

typedef LSTATUS(WINAPI* RegSetValueExW_t)(
    HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD);

typedef LSTATUS(WINAPI* RegSetKeyValueA_t)(
    HKEY, LPCSTR, LPCSTR, DWORD, const void*, DWORD);

typedef LSTATUS(WINAPI* RegSetKeyValueW_t)(
    HKEY, LPCWSTR, LPCWSTR, DWORD, const void*, DWORD);


// Delete
typedef LSTATUS(WINAPI* RegDeleteKeyA_t)(
    HKEY, LPCSTR);

typedef LSTATUS(WINAPI* RegDeleteKeyW_t)(
    HKEY, LPCWSTR);

typedef LSTATUS(WINAPI* RegDeleteKeyExA_t)(
    HKEY, LPCSTR, REGSAM, DWORD);

typedef LSTATUS(WINAPI* RegDeleteKeyExW_t)(
    HKEY, LPCWSTR, REGSAM, DWORD);

typedef LSTATUS(WINAPI* RegDeleteValueA_t)(
    HKEY, LPCSTR);

typedef LSTATUS(WINAPI* RegDeleteValueW_t)(
    HKEY, LPCWSTR);


// Enumerate
typedef LSTATUS(WINAPI* RegEnumKeyExA_t)(
    HKEY, DWORD, LPSTR, LPDWORD, LPDWORD,
    LPSTR, LPDWORD, PFILETIME);

typedef LSTATUS(WINAPI* RegEnumKeyExW_t)(
    HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD,
    LPWSTR, LPDWORD, PFILETIME);

typedef LSTATUS(WINAPI* RegEnumValueA_t)(
    HKEY, DWORD, LPSTR, LPDWORD, LPDWORD,
    LPDWORD, LPBYTE, LPDWORD);

typedef LSTATUS(WINAPI* RegEnumValueW_t)(
    HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD,
    LPDWORD, LPBYTE, LPDWORD);


// Close / flush
typedef LSTATUS(WINAPI* RegCloseKey_t)(
    HKEY);

typedef LSTATUS(WINAPI* RegFlushKey_t)(
    HKEY);


// Copy / replace / restore
typedef LSTATUS(WINAPI* RegCopyTreeA_t)(
    HKEY, LPCSTR, HKEY);

typedef LSTATUS(WINAPI* RegCopyTreeW_t)(
    HKEY, LPCWSTR, HKEY);

typedef LSTATUS(WINAPI* RegReplaceKeyA_t)(
    HKEY, LPCSTR, LPCSTR, LPCSTR);

typedef LSTATUS(WINAPI* RegReplaceKeyW_t)(
    HKEY, LPCWSTR, LPCWSTR, LPCWSTR);

typedef LSTATUS(WINAPI* RegRestoreKeyA_t)(
    HKEY, LPCSTR, DWORD);

typedef LSTATUS(WINAPI* RegRestoreKeyW_t)(
    HKEY, LPCWSTR, DWORD);

typedef LSTATUS(WINAPI* RegSaveKeyA_t)(
    HKEY, LPCSTR, const LPSECURITY_ATTRIBUTES);

typedef LSTATUS(WINAPI* RegSaveKeyW_t)(
    HKEY, LPCWSTR, const LPSECURITY_ATTRIBUTES);

typedef LSTATUS(WINAPI* RegSaveKeyExA_t)(
    HKEY, LPCSTR, const LPSECURITY_ATTRIBUTES, DWORD);

typedef LSTATUS(WINAPI* RegSaveKeyExW_t)(
    HKEY, LPCWSTR, const LPSECURITY_ATTRIBUTES, DWORD);


// Security
typedef LSTATUS(WINAPI* RegGetKeySecurity_t)(
    HKEY, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR, LPDWORD);

typedef LSTATUS(WINAPI* RegSetKeySecurity_t)(
    HKEY, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR);


// Link / symbolic
typedef LSTATUS(WINAPI* RegCreateKeyTransactedA_t)(
    HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM,
    const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD,
    HANDLE, PVOID);

typedef LSTATUS(WINAPI* RegCreateKeyTransactedW_t)(
    HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM,
    const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD,
    HANDLE, PVOID);

typedef LSTATUS(WINAPI* RegOpenKeyTransactedA_t)(
    HKEY, LPCSTR, DWORD, REGSAM, PHKEY, HANDLE);

typedef LSTATUS(WINAPI* RegOpenKeyTransactedW_t)(
    HKEY, LPCWSTR, DWORD, REGSAM, PHKEY, HANDLE);


// Remote registry
typedef LSTATUS(WINAPI* RegConnectRegistryA_t)(
    LPCSTR, HKEY, PHKEY);

typedef LSTATUS(WINAPI* RegConnectRegistryW_t)(
    LPCWSTR, HKEY, PHKEY);

typedef LSTATUS(WINAPI* RegDisconnectRegistry_t)(
    HKEY);

static RegOpenKeyExA_t Real_RegOpenKeyExA = nullptr;
static RegOpenKeyExW_t Real_RegOpenKeyExW = nullptr;

static RegCreateKeyExA_t Real_RegCreateKeyExA = nullptr;
static RegCreateKeyExW_t Real_RegCreateKeyExW = nullptr;

static RegQueryValueExA_t Real_RegQueryValueExA = nullptr;
static RegQueryValueExW_t Real_RegQueryValueExW = nullptr;

static RegQueryInfoKeyA_t Real_RegQueryInfoKeyA = nullptr;
static RegQueryInfoKeyW_t Real_RegQueryInfoKeyW = nullptr;

static RegSetValueExA_t Real_RegSetValueExA = nullptr;
static RegSetValueExW_t Real_RegSetValueExW = nullptr;

static RegSetKeyValueA_t Real_RegSetKeyValueA = nullptr;
static RegSetKeyValueW_t Real_RegSetKeyValueW = nullptr;

static RegDeleteKeyA_t Real_RegDeleteKeyA = nullptr;
static RegDeleteKeyW_t Real_RegDeleteKeyW = nullptr;

static RegDeleteKeyExA_t Real_RegDeleteKeyExA = nullptr;
static RegDeleteKeyExW_t Real_RegDeleteKeyExW = nullptr;

static RegDeleteValueA_t Real_RegDeleteValueA = nullptr;
static RegDeleteValueW_t Real_RegDeleteValueW = nullptr;

static RegEnumKeyExA_t Real_RegEnumKeyExA = nullptr;
static RegEnumKeyExW_t Real_RegEnumKeyExW = nullptr;

static RegEnumValueA_t Real_RegEnumValueA = nullptr;
static RegEnumValueW_t Real_RegEnumValueW = nullptr;

static RegCloseKey_t Real_RegCloseKey = nullptr;
static RegFlushKey_t Real_RegFlushKey = nullptr;

static RegCopyTreeA_t Real_RegCopyTreeA = nullptr;
static RegCopyTreeW_t Real_RegCopyTreeW = nullptr;

static RegReplaceKeyA_t Real_RegReplaceKeyA = nullptr;
static RegReplaceKeyW_t Real_RegReplaceKeyW = nullptr;

static RegRestoreKeyA_t Real_RegRestoreKeyA = nullptr;
static RegRestoreKeyW_t Real_RegRestoreKeyW = nullptr;

static RegSaveKeyA_t Real_RegSaveKeyA = nullptr;
static RegSaveKeyW_t Real_RegSaveKeyW = nullptr;

static RegSaveKeyExA_t Real_RegSaveKeyExA = nullptr;
static RegSaveKeyExW_t Real_RegSaveKeyExW = nullptr;

static RegGetKeySecurity_t Real_RegGetKeySecurity = nullptr;
static RegSetKeySecurity_t Real_RegSetKeySecurity = nullptr;

static RegCreateKeyTransactedA_t Real_RegCreateKeyTransactedA = nullptr;
static RegCreateKeyTransactedW_t Real_RegCreateKeyTransactedW = nullptr;

static RegOpenKeyTransactedA_t Real_RegOpenKeyTransactedA = nullptr;
static RegOpenKeyTransactedW_t Real_RegOpenKeyTransactedW = nullptr;

static RegConnectRegistryA_t Real_RegConnectRegistryA = nullptr;
static RegConnectRegistryW_t Real_RegConnectRegistryW = nullptr;

static RegDisconnectRegistry_t Real_RegDisconnectRegistry = nullptr;