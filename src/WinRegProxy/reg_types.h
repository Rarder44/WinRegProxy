#pragma once
// Tipi delle funzioni Reg* e puntatori "Real_*" (i trampolini creati da MinHook).
// I Real_* sono dichiarati EXTERN qui e DEFINITI in reg_hooks.cpp: il file e'
// quindi includibile liberamente da chi deve chiamare gli originali (reg_hooks.cpp,
// REG_adapter.cpp, ...).

#include "pch.h"

#ifndef REG_TYPES
#define REG_TYPES



// ---------------------------------------------------------------- Open / create
typedef LSTATUS(WINAPI* RegOpenKeyExA_t)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LSTATUS(WINAPI* RegOpenKeyExW_t)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
typedef LSTATUS(WINAPI* RegOpenKeyA_t)(HKEY, LPCSTR, PHKEY);
typedef LSTATUS(WINAPI* RegOpenKeyW_t)(HKEY, LPCWSTR, PHKEY);

typedef LSTATUS(WINAPI* RegCreateKeyExA_t)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM,
    LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
typedef LSTATUS(WINAPI* RegCreateKeyExW_t)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM,
    LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
typedef LSTATUS(WINAPI* RegCreateKeyA_t)(HKEY, LPCSTR, PHKEY);
typedef LSTATUS(WINAPI* RegCreateKeyW_t)(HKEY, LPCWSTR, PHKEY);

typedef LSTATUS(WINAPI* RegCreateKeyTransactedA_t)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM,
    LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD, HANDLE, PVOID);
typedef LSTATUS(WINAPI* RegCreateKeyTransactedW_t)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM,
    LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD, HANDLE, PVOID);
typedef LSTATUS(WINAPI* RegOpenKeyTransactedA_t)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY, HANDLE, PVOID);
typedef LSTATUS(WINAPI* RegOpenKeyTransactedW_t)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY, HANDLE, PVOID);

typedef LSTATUS(WINAPI* RegOpenCurrentUser_t)(REGSAM, PHKEY);
typedef LSTATUS(WINAPI* RegOpenUserClassesRoot_t)(HANDLE, DWORD, REGSAM, PHKEY);
typedef LSTATUS(WINAPI* RegOverridePredefKey_t)(HKEY, HKEY);

// ---------------------------------------------------------------- Query
typedef LSTATUS(WINAPI* RegQueryValueExA_t)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LSTATUS(WINAPI* RegQueryValueExW_t)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LSTATUS(WINAPI* RegQueryValueA_t)(HKEY, LPCSTR, LPSTR, PLONG);
typedef LSTATUS(WINAPI* RegQueryValueW_t)(HKEY, LPCWSTR, LPWSTR, PLONG);
typedef LSTATUS(WINAPI* RegGetValueA_t)(HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD);
typedef LSTATUS(WINAPI* RegGetValueW_t)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);

// RegQueryInfoKey: l'undicesimo parametro e' LPDWORD (security descriptor), non una stringa
typedef LSTATUS(WINAPI* RegQueryInfoKeyA_t)(HKEY, LPSTR, LPDWORD, LPDWORD, LPDWORD, LPDWORD,
    LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, PFILETIME);
typedef LSTATUS(WINAPI* RegQueryInfoKeyW_t)(HKEY, LPWSTR, LPDWORD, LPDWORD, LPDWORD, LPDWORD,
    LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, PFILETIME);

// ---------------------------------------------------------------- Set
typedef LSTATUS(WINAPI* RegSetValueExA_t)(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD);
typedef LSTATUS(WINAPI* RegSetValueExW_t)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD);
typedef LSTATUS(WINAPI* RegSetValueA_t)(HKEY, LPCSTR, DWORD, LPCSTR, DWORD);
typedef LSTATUS(WINAPI* RegSetValueW_t)(HKEY, LPCWSTR, DWORD, LPCWSTR, DWORD);
typedef LSTATUS(WINAPI* RegSetKeyValueA_t)(HKEY, LPCSTR, LPCSTR, DWORD, LPCVOID, DWORD);
typedef LSTATUS(WINAPI* RegSetKeyValueW_t)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPCVOID, DWORD);

// ---------------------------------------------------------------- Delete
typedef LSTATUS(WINAPI* RegDeleteKeyA_t)(HKEY, LPCSTR);
typedef LSTATUS(WINAPI* RegDeleteKeyW_t)(HKEY, LPCWSTR);
typedef LSTATUS(WINAPI* RegDeleteKeyExA_t)(HKEY, LPCSTR, REGSAM, DWORD);
typedef LSTATUS(WINAPI* RegDeleteKeyExW_t)(HKEY, LPCWSTR, REGSAM, DWORD);
typedef LSTATUS(WINAPI* RegDeleteValueA_t)(HKEY, LPCSTR);
typedef LSTATUS(WINAPI* RegDeleteValueW_t)(HKEY, LPCWSTR);
typedef LSTATUS(WINAPI* RegDeleteTreeA_t)(HKEY, LPCSTR);
typedef LSTATUS(WINAPI* RegDeleteTreeW_t)(HKEY, LPCWSTR);
typedef LSTATUS(WINAPI* RegDeleteKeyValueA_t)(HKEY, LPCSTR, LPCSTR);
typedef LSTATUS(WINAPI* RegDeleteKeyValueW_t)(HKEY, LPCWSTR, LPCWSTR);

// ---------------------------------------------------------------- Enumerate
typedef LSTATUS(WINAPI* RegEnumKeyExA_t)(HKEY, DWORD, LPSTR, LPDWORD, LPDWORD, LPSTR, LPDWORD, PFILETIME);
typedef LSTATUS(WINAPI* RegEnumKeyExW_t)(HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD, LPWSTR, LPDWORD, PFILETIME);
typedef LSTATUS(WINAPI* RegEnumKeyA_t)(HKEY, DWORD, LPSTR, DWORD);
typedef LSTATUS(WINAPI* RegEnumKeyW_t)(HKEY, DWORD, LPWSTR, DWORD);
typedef LSTATUS(WINAPI* RegEnumValueA_t)(HKEY, DWORD, LPSTR, LPDWORD, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LSTATUS(WINAPI* RegEnumValueW_t)(HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD, LPDWORD, LPBYTE, LPDWORD);

// ---------------------------------------------------------------- Close / flush / notify
typedef LSTATUS(WINAPI* RegCloseKey_t)(HKEY);
typedef LSTATUS(WINAPI* RegFlushKey_t)(HKEY);
typedef LSTATUS(WINAPI* RegNotifyChangeKeyValue_t)(HKEY, BOOL, DWORD, HANDLE, BOOL);

// ---------------------------------------------------------------- Copy / replace / save / restore
typedef LSTATUS(WINAPI* RegCopyTreeA_t)(HKEY, LPCSTR, HKEY);
typedef LSTATUS(WINAPI* RegCopyTreeW_t)(HKEY, LPCWSTR, HKEY);
typedef LSTATUS(WINAPI* RegReplaceKeyA_t)(HKEY, LPCSTR, LPCSTR, LPCSTR);
typedef LSTATUS(WINAPI* RegReplaceKeyW_t)(HKEY, LPCWSTR, LPCWSTR, LPCWSTR);
typedef LSTATUS(WINAPI* RegRestoreKeyA_t)(HKEY, LPCSTR, DWORD);
typedef LSTATUS(WINAPI* RegRestoreKeyW_t)(HKEY, LPCWSTR, DWORD);
typedef LSTATUS(WINAPI* RegSaveKeyA_t)(HKEY, LPCSTR, LPSECURITY_ATTRIBUTES);
typedef LSTATUS(WINAPI* RegSaveKeyW_t)(HKEY, LPCWSTR, LPSECURITY_ATTRIBUTES);
typedef LSTATUS(WINAPI* RegSaveKeyExA_t)(HKEY, LPCSTR, LPSECURITY_ATTRIBUTES, DWORD);
typedef LSTATUS(WINAPI* RegSaveKeyExW_t)(HKEY, LPCWSTR, LPSECURITY_ATTRIBUTES, DWORD);

// ---------------------------------------------------------------- Security
typedef LSTATUS(WINAPI* RegGetKeySecurity_t)(HKEY, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR, LPDWORD);
typedef LSTATUS(WINAPI* RegSetKeySecurity_t)(HKEY, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR);

// ---------------------------------------------------------------- Remote registry
typedef LSTATUS(WINAPI* RegConnectRegistryA_t)(LPCSTR, HKEY, PHKEY);
typedef LSTATUS(WINAPI* RegConnectRegistryW_t)(LPCWSTR, HKEY, PHKEY);
typedef LSTATUS(WINAPI* RegDisconnectRegistry_t)(HKEY);

// ================================================================ Real_* (trampolini)
#define DECL_REAL(NAME) extern NAME##_t Real_##NAME

DECL_REAL(RegOpenKeyExA);     DECL_REAL(RegOpenKeyExW);
DECL_REAL(RegOpenKeyA);       DECL_REAL(RegOpenKeyW);
DECL_REAL(RegCreateKeyExA);   DECL_REAL(RegCreateKeyExW);
DECL_REAL(RegCreateKeyA);     DECL_REAL(RegCreateKeyW);
DECL_REAL(RegCreateKeyTransactedA); DECL_REAL(RegCreateKeyTransactedW);
DECL_REAL(RegOpenKeyTransactedA);   DECL_REAL(RegOpenKeyTransactedW);
DECL_REAL(RegOpenCurrentUser);
DECL_REAL(RegOpenUserClassesRoot);
DECL_REAL(RegOverridePredefKey);

DECL_REAL(RegQueryValueExA);  DECL_REAL(RegQueryValueExW);
DECL_REAL(RegQueryValueA);    DECL_REAL(RegQueryValueW);
DECL_REAL(RegGetValueA);      DECL_REAL(RegGetValueW);
DECL_REAL(RegQueryInfoKeyA);  DECL_REAL(RegQueryInfoKeyW);

DECL_REAL(RegSetValueExA);    DECL_REAL(RegSetValueExW);
DECL_REAL(RegSetValueA);      DECL_REAL(RegSetValueW);
DECL_REAL(RegSetKeyValueA);   DECL_REAL(RegSetKeyValueW);

DECL_REAL(RegDeleteKeyA);     DECL_REAL(RegDeleteKeyW);
DECL_REAL(RegDeleteKeyExA);   DECL_REAL(RegDeleteKeyExW);
DECL_REAL(RegDeleteValueA);   DECL_REAL(RegDeleteValueW);
DECL_REAL(RegDeleteTreeA);    DECL_REAL(RegDeleteTreeW);
DECL_REAL(RegDeleteKeyValueA); DECL_REAL(RegDeleteKeyValueW);

DECL_REAL(RegEnumKeyExA);     DECL_REAL(RegEnumKeyExW);
DECL_REAL(RegEnumKeyA);       DECL_REAL(RegEnumKeyW);
DECL_REAL(RegEnumValueA);     DECL_REAL(RegEnumValueW);

DECL_REAL(RegCloseKey);
DECL_REAL(RegFlushKey);
DECL_REAL(RegNotifyChangeKeyValue);

DECL_REAL(RegCopyTreeA);      DECL_REAL(RegCopyTreeW);
DECL_REAL(RegReplaceKeyA);    DECL_REAL(RegReplaceKeyW);
DECL_REAL(RegRestoreKeyA);    DECL_REAL(RegRestoreKeyW);
DECL_REAL(RegSaveKeyA);       DECL_REAL(RegSaveKeyW);
DECL_REAL(RegSaveKeyExA);     DECL_REAL(RegSaveKeyExW);

DECL_REAL(RegGetKeySecurity); DECL_REAL(RegSetKeySecurity);

DECL_REAL(RegConnectRegistryA); DECL_REAL(RegConnectRegistryW);
DECL_REAL(RegDisconnectRegistry);


#endif // REG_TYPES