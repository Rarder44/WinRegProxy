// REG_adapter.cpp
// Inizializzazione dell'adattatore (issue 40): crea/apre la SQLLite_REG sul path
// del DB (accanto alla DLL, ripiego %TEMP%). Singleton lazy (issues/40).
#include "pch.h"
#include "REG_adapter.h"
#include <string>
#include <cwchar>

REG_adapter* REG_adapter::instance = nullptr;   // singleton lazy

// ===== path del DB (UTF-8; il DB memorizza tutto in UTF-8) =====
static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// <dir della DLL>\reg_virtual.db ; vuoto se non calcolabile.
static std::string DBPathPrimary(HMODULE self)
{
    wchar_t path[MAX_PATH] = L"";
    if (GetModuleFileNameW(self, path, MAX_PATH) == 0) return std::string();
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return std::string();
    *slash = 0;                       // tronco dopo l'ultima \ -> solo la dir
    std::wstring dir(path);
    dir += L"\\reg_virtual.db";
    return WideToUtf8(dir);
}

// %TEMP%\reg_virtual.db ; vuoto se non calcolabile.
static std::string DBPathTemp()
{
    wchar_t tmp[MAX_PATH] = L"";
    if (GetTempPathW(MAX_PATH, tmp) == 0) return std::string();
    std::wstring t(tmp);
    t += L"reg_virtual.db";
    return WideToUtf8(t);
}

// ===== OpenKey (issue 41) =====
// UTF-8 (sottosezione da 'from' in poi) -> wide; vuoto se non convertibile.
static std::wstring Utf8ToW(const std::string& s, size_t from = 0)
{
    size_t len = s.size() - from;
    if (len == 0) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str() + from, (int)len, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str() + from, (int)len, &out[0], n);
    return out;
}

// wide -> UTF-8 (inverso di Utf8ToW).
static std::string WToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// Scompone "HKxx\subkey..." in HKEY apribile + subkey wide.
// false se il token non e' mappabile (HKPD/HKDD/altro: non apribili con RegOpenKeyEx).
static bool HiveRoot(const std::string& keyPath, HKEY* outRoot, std::wstring* outSub)
{
    size_t first = keyPath.find('\\');
    std::string tok = (first == std::string::npos) ? keyPath : keyPath.substr(0, first);
    HKEY root = NULL;
    if (tok == "HKCU")            root = HKEY_CURRENT_USER;
    else if (tok == "HKLM")       root = HKEY_LOCAL_MACHINE;
    else if (tok == "HKCR")       root = HKEY_CLASSES_ROOT;
    else if (tok == "HKU")        root = HKEY_USERS;
    else if (tok == "HKCC")       root = HKEY_CURRENT_CONFIG;
    else return false;
    *outRoot = root;
    *outSub = (first == std::string::npos) ? std::wstring() : Utf8ToW(keyPath, first + 1);
    return true;
}

bool REG_adapter::OpenKey(const std::string& keyPath, REGSAM access, LSTATUS& out, unsigned& outId)
{
    // Stato nel DB (KeyState, issue 41): -1 cancellata, 0 assente, 1 virtuale viva.
    // DB non aperto -> 0 (degrado a pass-through reale, decisione issue 40).
    int s = sqlite_reg->isOpen() ? sqlite_reg->KeyState(keyPath) : 0;
    if (s == -1)
    {
        // Cancellata virtualmente: "non esiste" anche se presente nel reale.
        out = ERROR_FILE_NOT_FOUND;
        outId = 0;
        return false;
    }
    if (s == 1)
    {
        // Virtuale viva: id fittizio, il registro reale non viene toccato.
        HKEY fake = handler_mng->registerVirtualHandler(keyPath);
        outId = (unsigned)(UINT_PTR)fake;
        out = ERROR_SUCCESS;
        return true;
    }
    // s == 0: open reale; l'HKEY reale resta nel record (chiudibile via 50/67).
    HKEY root = NULL;
    std::wstring sub;
    if (!HiveRoot(keyPath, &root, &sub))
    {
        out = ERROR_FILE_NOT_FOUND;
        outId = 0;
        return false;
    }
    HKEY real = NULL;
    LSTATUS r = Real_RegOpenKeyExW(root, sub.c_str(), 0, access, &real);
    if (r != ERROR_SUCCESS)
    {
        out = r;
        outId = 0;
        return false;
    }
    HKEY fake = handler_mng->registerRealHandler(real, keyPath);
    outId = (unsigned)(UINT_PTR)fake;
    out = ERROR_SUCCESS;
    return true;
}

// ===== CreateKey (issue 42) =====
// Scrittura SOLO nel DB (mai nel registro reale). La chiave viene creata anche
// se esiste solo nel reale: la virtualizzazione e' globale (00-phase2) e i
// valori reali restano visibili via merge (43-49). access/sam non si usa:
// chiave virtuale, nessun reale ACL.
bool REG_adapter::CreateKey(const std::string& keyPath, DWORD* disposition, LSTATUS& out, unsigned& outId)
{
    // DB non aperto -> non gestita: il detour va al reale (degrado, issue 40).
    if (!sqlite_reg->isOpen())
    {
        out = ERROR_ACCESS_DENIED;
        outId = 0;
        return false;
    }

    // existed va preso PRIMA di CreateKey: dopo, la riga c'e' per forza.
    bool existed = sqlite_reg->ExistKey(keyPath);
    if (!sqlite_reg->CreateKey(keyPath))
    {
        // errore SQL (db corrotto/pieno): non gestita, il detour va al reale.
        out = ERROR_ACCESS_DENIED;
        outId = 0;
        return false;
    }

    if (disposition)
        *disposition = existed ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
    HKEY fake = handler_mng->registerVirtualHandler(keyPath);
    outId = (unsigned)(UINT_PTR)fake;
    out = ERROR_SUCCESS;
    return true;
}

// ===== QueryValue (issue 43) =====
// Lettura di un valore: DB primo, poi reale se il valore non c'e' nel DB
// (contratto nel header: out = stato + dati; le semantica dei buffer sono
// dei detour di 62). id = id fittizio (handler_mng) o root reale nota.

// id + sub -> percorso assoluto "HKxx\..." (UTF-8). false se non risolvibile.
static bool QueryResolvePath(REG_handler_mng* mng, HKEY id, const std::wstring& sub, std::string* out)
{
    // Root nota: token + subkey (subkey vuota = la root stessa).
    const wchar_t* tok = nullptr;
    if (id == HKEY_CURRENT_USER)            tok = L"HKCU";
    else if (id == HKEY_LOCAL_MACHINE)      tok = L"HKLM";
    else if (id == HKEY_CLASSES_ROOT)       tok = L"HKCR";
    else if (id == HKEY_USERS)              tok = L"HKU";
    else if (id == HKEY_CURRENT_CONFIG)     tok = L"HKCC";
    else if (id == HKEY_DYN_DATA)           tok = L"HKDD";
    else if (id == HKEY_PERFORMANCE_DATA)   tok = L"HKPD";
    if (tok)
    {
        std::wstring w = sub.empty() ? std::wstring(tok) : std::wstring(tok) + L"\\" + sub;
        *out = WideToUtf8(w);
        return !out->empty();
    }
    // Id fittizio: keyPath dal record del handler (+ subkey se c'e').
    HandlerRecord rec;
    if (!mng->getHandler(id, &rec)) return false;
    std::wstring w = Utf8ToW(rec.keyPath);
    if (!sub.empty()) w += L"\\" + sub;
    *out = WideToUtf8(w);
    return !out->empty();
}

// ===== ResolveId (issue 68) =====
// Exposto per i detour open/create: stesso di QueryResolvePath (root o id
// fittizio), firma membro. false -> il detour passa al reale.
bool REG_adapter::ResolveId(HKEY id, const std::wstring& sub, std::string* out)
{
    return QueryResolvePath(handler_mng, id, sub, out);
}

bool REG_adapter::QueryValue(HKEY id, const std::wstring& sub, const std::wstring& name, RegQueryOut* out)
{
    out->status = ERROR_SUCCESS;
    out->type = REG_NONE;
    out->data.clear();
    out->virtualHit = false;

    if (!sqlite_reg->isOpen()) return false;                       // DB non aperto: reale
    std::string path;
    if (!QueryResolvePath(handler_mng, id, sub, &path)) return false;   // id sconosciuto: reale
    std::string n = WideToUtf8(name);

    int vs = sqlite_reg->ValueState(path, n);
    if (vs == -1 || vs == 1)
    {
        out->virtualHit = true;
        if (vs == -1)
        {
            // Cancellato virtualmente: "non esiste" anche se c'e' nel reale.
            out->status = ERROR_FILE_NOT_FOUND;
            return true;
        }
        int type = 0;
        std::string data;
        if (!sqlite_reg->QueryValue(path, n, &type, &data))
        {
            out->status = ERROR_FILE_NOT_FOUND;   // riga sparita nel frattempo
            return true;
        }
        out->type = (DWORD)type;
        // DB: stringhe UTF-8 (senza NUL); out: wide con NUL finale (come nel reale).
        DWORD t = out->type;
        if (t == REG_SZ || t == REG_EXPAND_SZ || t == REG_MULTI_SZ)
        {
            std::wstring w = Utf8ToW(data);
            if (!w.empty()) w += L'\x00';          // vuoto: data resta vuoto (0 byte)
            out->data.assign(w.empty() ? nullptr : (const char*)w.data(), w.size() * sizeof(wchar_t));
        }
        else
            out->data = data;
        return true;
    }

    // vs == 0: non c'e' nel DB -> REALE (open root+sub, lettura, close;
    // nessuna modifica al registro).
    HKEY root = NULL;
    std::wstring rsub;
    if (!HiveRoot(path, &root, &rsub)) return false;
    HKEY real = NULL;
    LSTATUS r = Real_RegOpenKeyExW(root, rsub.c_str(), 0, KEY_READ, &real);
    if (r != ERROR_SUCCESS)
    {
        out->status = r;                       // codice reale (FILE_NOT_FOUND, ecc.)
        out->virtualHit = true;
        return true;
    }
    const wchar_t* nm = name.empty() ? nullptr : name.c_str();
    DWORD type = 0, cbsize = 0;
    LSTATUS q = Real_RegQueryValueExW(real, nm, nullptr, &type, nullptr, &cbsize);
    std::string data;
    if (q == ERROR_SUCCESS && cbsize > 0)
    {
        data.assign(cbsize, '\x00');
        q = Real_RegQueryValueExW(real, nm, nullptr, &type, (LPBYTE)&data[0], &cbsize);
    }
    Real_RegCloseKey(real);
    out->virtualHit = true;
    if (q != ERROR_SUCCESS)
    {
        out->status = q;                       // NOT_FOUND ecc.: codice reale, no dati
        return true;
    }
    out->type = type;
    out->data = data;                          // stringhe: wide grezzo con NUL; altri: grezzi
    return true;
}

// byte grezzi -> wide (per i tipi stringa: il detour normalizza in UTF-16LE;
// cb dispari = ultimo wchar troncato, come fa la API reale senza NUL finale).
static std::wstring RawToW(const std::string& raw)
{
    if (raw.empty()) return std::wstring();
    return std::wstring((const wchar_t*)raw.data(), raw.size() / 2);
}

bool REG_adapter::SetValue(HKEY id, const std::wstring& sub, const std::wstring& name, DWORD type, const std::string& data, RegSetOut* out)
{
    out->status = ERROR_SUCCESS;
    out->virtualHit = false;

    if (!sqlite_reg->isOpen()) return false;                       // DB non aperto: reale
    std::string path;
    if (!QueryResolvePath(handler_mng, id, sub, &path)) return false;   // id sconosciuto: reale
    std::string n = WideToUtf8(name);

    // Convenzione DB: stringhe = UTF-8 SENZA NUL (QueryValue riacquista il NUL
    // in uscita); non-stringa: byte grezzi.
    std::string blob = data;
    if (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ)
    {
        std::wstring w = RawToW(data);
        if (!w.empty() && w.back() == L'\x00') w.pop_back();
        blob = WideToUtf8(w);
    }

    bool ok = sqlite_reg->SetValue(path, n, (int)type, blob);
    out->virtualHit = true;
    out->status = ok ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;   // pattern create (42)
    return true;
}

// ===== Delete (issue 45/46) =====
// Cancellazione virtuale: la riga nel DB vince. Se l'obiettivo NON c'e' nel
// DB, il fallback al reale e' lecito solo dove il reale puo' esistere davvero
// (record Real o root nota): in quel caso si restituisce false e il detour
// chiama Real_* con gli argomenti originali (fase 1, semantica reale). Per i
// record Virtual non c'e' reale dietro: FILE_NOT_FOUND.

// id fittizio o root nota -> "il reale dietro questo id puo' esistere?"
// (root: si, per definition; id fittizio: solo se record Real).
static bool DeleteFallbackRealAllowed(REG_handler_mng* mng, HKEY id)
{
    if (id == HKEY_CURRENT_USER || id == HKEY_LOCAL_MACHINE || id == HKEY_CLASSES_ROOT ||
        id == HKEY_USERS || id == HKEY_CURRENT_CONFIG || id == HKEY_DYN_DATA || id == HKEY_PERFORMANCE_DATA)
        return true;   // root nota: reale esistente
    HandlerRecord rec;
    if (!mng->getHandler(id, &rec)) return false;
    return rec.type != HandlerType::Virtual;   // Real -> fallback ok; Virtual -> no
}

bool REG_adapter::DeleteValue(HKEY id, const std::wstring& sub, const std::wstring& name, RegDelOut* out)
{
    out->status = ERROR_SUCCESS;
    out->virtualHit = false;

    if (!sqlite_reg->isOpen()) return false;                       // DB non aperto: reale
    std::string path;
    if (!QueryResolvePath(handler_mng, id, sub, &path)) return false;   // id sconosciuto: reale
    std::string n = WideToUtf8(name);

    if (sqlite_reg->DeleteValue(path, n))
    {
        out->virtualHit = true;
        out->status = ERROR_SUCCESS;
        return true;
    }
    // Niente da cancellare nel DB.
    if (DeleteFallbackRealAllowed(handler_mng, id)) return false;  // reale: detour -> Real_*
    out->virtualHit = true;
    out->status = ERROR_FILE_NOT_FOUND;
    return true;
}

bool REG_adapter::DeleteKey(HKEY id, const std::wstring& sub, RegDelOut* out)
{
    out->status = ERROR_SUCCESS;
    out->virtualHit = false;

    if (!sqlite_reg->isOpen()) return false;                       // DB non aperto: reale
    std::string path;
    if (!QueryResolvePath(handler_mng, id, sub, &path)) return false;   // id sconosciuto: reale

    if (sqlite_reg->DeleteKey(path))
    {
        out->virtualHit = true;
        out->status = ERROR_SUCCESS;
        return true;
    }
    // Chiave assente/cancellata nel DB.
    if (DeleteFallbackRealAllowed(handler_mng, id)) return false;  // reale: detour -> Real_*
    out->virtualHit = true;
    out->status = ERROR_FILE_NOT_FOUND;
    return true;
}

// ===== Enum (47/48): unione DB+reale =====
// Elenco REAL delle sottochiavi (ordine reale): apri la chiave reale con
// KEY_READ (HiveRoot + Real_RegOpenKeyExW). false = non esiste nel reale
// (chiave virtuale pura o root sconosciuta); *openErr = errore dell'open.
// Reale MAI modificato (close alla fine).
bool REG_adapter::EnumRealKeys(const std::string& path, std::vector<EnumEntryW>* list, LSTATUS* openErr)
{
    HKEY root = NULL;
    std::wstring rsub;
    if (!HiveRoot(path, &root, &rsub)) { if (openErr) *openErr = ERROR_FILE_NOT_FOUND; return false; }
    HKEY real = NULL;
    LSTATUS r = Real_RegOpenKeyExW(root, rsub.c_str(), 0, KEY_READ, &real);
    if (r != ERROR_SUCCESS) { if (openErr) *openErr = r; return false; }
    list->clear();
    for (DWORD i = 0; ; i++)
    {
        WCHAR buf[1024];
        DWORD cch = sizeof(buf) / sizeof(WCHAR);
        if (Real_RegEnumKeyExW(real, i, buf, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        EnumEntryW e;
        e.name = buf;
        list->push_back(e);
    }
    Real_RegCloseKey(real);
    return true;
}

// Elenco REAL dei valori (ordine reale): type/data gia' in formato API.
bool REG_adapter::EnumRealValues(const std::string& path, std::vector<EnumEntryW>* list, LSTATUS* openErr)
{
    HKEY root = NULL;
    std::wstring rsub;
    if (!HiveRoot(path, &root, &rsub)) { if (openErr) *openErr = ERROR_FILE_NOT_FOUND; return false; }
    HKEY real = NULL;
    LSTATUS r = Real_RegOpenKeyExW(root, rsub.c_str(), 0, KEY_READ, &real);
    if (r != ERROR_SUCCESS) { if (openErr) *openErr = r; return false; }
    list->clear();
    for (DWORD i = 0; ; i++)
    {
        WCHAR nbuf[1024];
        DWORD ncb = sizeof(nbuf) / sizeof(WCHAR);
        BYTE dbuf[4096];
        DWORD t = 0, dc = sizeof(dbuf);
        if (Real_RegEnumValueW(real, i, nbuf, &ncb, NULL, &t, dbuf, &dc) != ERROR_SUCCESS)
            break;
        EnumEntryW e;
        e.name = nbuf;
        e.type = t;
        e.data.assign(dbuf, dbuf + dc);
        list->push_back(e);
    }
    Real_RegCloseKey(real);
    return true;
}

// Lista DB (vivi, ordine per nome, 27/28) -> voci wide.
void REG_adapter::DbEnumKeys(SQLLite_REG* db, const std::string& path, std::vector<EnumEntryW>* list)
{
    std::vector<std::string> subs = db->EnumKey(path);
    for (size_t i = 0; i < subs.size(); i++)
    {
        EnumEntryW e;
        e.name = Utf8ToW(subs[i]);
        list->push_back(e);
    }
}

void REG_adapter::DbEnumValues(SQLLite_REG* db, const std::string& path, std::vector<EnumEntryW>* list)
{
    std::vector<RegValue> vals = db->EnumValue(path);
    for (size_t i = 0; i < vals.size(); i++)
    {
        EnumEntryW e;
        e.name = Utf8ToW(vals[i].name);
        e.type = (DWORD)vals[i].type;
        // DB: stringhe UTF-8 (senza NUL); out: wide con NUL (come QueryValue).
        if (e.type == REG_SZ || e.type == REG_EXPAND_SZ || e.type == REG_MULTI_SZ)
        {
            std::wstring w = Utf8ToW(vals[i].data);
            if (!w.empty()) w += L'\x00';
            if (!w.empty()) e.data.assign((const BYTE*)w.data(), (const BYTE*)w.data() + w.size() * sizeof(wchar_t));
        }
        else
            if (!vals[i].data.empty())
                e.data.assign((const BYTE*)vals[i].data.data(), (const BYTE*)vals[i].data.data() + vals[i].data.size());
        list->push_back(e);
    }
}

// Merge: reali (ordine reale, maschera soft-deleted nel DB) + solo-DB
// (ordine DB), dedup case-insensitive per nome (vince la voce reale).
// index >= size -> out->status = ERROR_NO_MORE_ITEMS.
bool REG_adapter::MergedEnumEntry(SQLLite_REG* db, const std::string& path, bool values,
        const std::vector<EnumEntryW>& realList, DWORD index, RegEnumOut* out)
{
    std::vector<EnumEntryW> dbList;
    if (values) REG_adapter::DbEnumValues(db, path, &dbList); else REG_adapter::DbEnumKeys(db, path, &dbList);

    std::vector<EnumEntryW> merged;
    for (size_t i = 0; i < realList.size(); i++)
    {
        std::string rn = WideToUtf8(realList[i].name);
        int st = values ? db->ValueState(path, rn)
                        : db->KeyState(path + "\\" + rn);
        if (st == -1) continue;    // oscurato virtualmente: nasconde il reale
        merged.push_back(realList[i]);
    }
    for (size_t i = 0; i < dbList.size(); i++)
    {
        bool dup = false;
        for (size_t j = 0; j < realList.size(); j++)
            if (_wcsicmp(dbList[i].name.c_str(), realList[j].name.c_str()) == 0) { dup = true; break; }
        if (!dup) merged.push_back(dbList[i]);
    }

    if (index >= merged.size())
    {
        out->status = ERROR_NO_MORE_ITEMS;
        return true;
    }
    out->status = ERROR_SUCCESS;
    out->name = merged[index].name;
    out->type = merged[index].type;
    out->data = merged[index].data;
    return true;
}

bool REG_adapter::EnumKey(HKEY id, DWORD index, RegEnumOut* out)
{
    out->status = ERROR_SUCCESS;
    out->virtualHit = false;
    out->name.clear();

    if (!sqlite_reg->isOpen()) return false;                       // DB non aperto: reale
    std::string path;
    if (!QueryResolvePath(handler_mng, id, L"", &path)) return false;   // id sconosciuto: reale

    int ks = sqlite_reg->KeyState(path);
    if (ks == -1)   // cancellata nel DB: nasconde anche il reale
    {
        out->virtualHit = true;
        out->status = ERROR_FILE_NOT_FOUND;
        return true;
    }
    std::vector<EnumEntryW> realList;
    LSTATUS openErr = ERROR_SUCCESS;
    bool realOk = REG_adapter::EnumRealKeys(path, &realList, &openErr);
    if (ks != 1 && !realOk)   // non nel DB (viva) e non nel reale: non esiste
    {
        out->virtualHit = true;
        out->status = openErr;   // tipicamente FILE_NOT_FOUND
        return true;
    }
    out->virtualHit = true;
    return REG_adapter::MergedEnumEntry(sqlite_reg, path, false, realList, index, out);
}

bool REG_adapter::EnumValue(HKEY id, DWORD index, RegEnumOut* out)
{
    out->status = ERROR_SUCCESS;
    out->virtualHit = false;
    out->name.clear();

    if (!sqlite_reg->isOpen()) return false;                       // DB non aperto: reale
    std::string path;
    if (!QueryResolvePath(handler_mng, id, L"", &path)) return false;   // id sconosciuto: reale

    int ks = sqlite_reg->KeyState(path);
    if (ks == -1)   // chiave cancellata nel DB: nasconde anche i valori reali
    {
        out->virtualHit = true;
        out->status = ERROR_FILE_NOT_FOUND;
        return true;
    }
    std::vector<EnumEntryW> realList;
    LSTATUS openErr = ERROR_SUCCESS;
    bool realOk = REG_adapter::EnumRealValues(path, &realList, &openErr);
    if (ks != 1 && !realOk)   // non nel DB (viva) e non nel reale: non esiste
    {
        out->virtualHit = true;
        out->status = openErr;
        return true;
    }
    out->virtualHit = true;
    return REG_adapter::MergedEnumEntry(sqlite_reg, path, true, realList, index, out);
}

// ===== QueryInfo (49): unione DB+reale =====
bool REG_adapter::QueryInfo(HKEY id, RegQueryInfoOut* out)
{
    std::string path;
    if (!QueryResolvePath(handler_mng, id, L"", &path))
        return false;

    out->status = ERROR_FILE_NOT_FOUND;
    out->virtualHit = true;
    out->numSubkeys = 0;
    out->numValues = 0;
    out->maxSubkeyNameLen = 0;
    out->maxValueNameLen = 0;
    out->maxValueDataLen = 0;
    out->securityDescLen = 0;
    ZeroMemory(&out->lastWrite, sizeof(FILETIME));

    if (sqlite_reg->KeyState(path) == -1)
        return true;   // soft-deleted -> NOT_FOUND (anche se esiste nel reale)

    // Parte reale (se presente): nomi reali + max*/seclen/lastWrite reali.
    std::vector<EnumEntryW> realKeys, realVals;
    LSTATUS openErr = ERROR_SUCCESS;
    bool realOk = EnumRealKeys(path, &realKeys, &openErr);
    if (realOk)
        realOk = EnumRealValues(path, &realVals, &openErr);
    FILETIME realFt = {0, 0};
    DWORD realSecLen = 0;
    if (realOk)
    {
        HKEY root = NULL;
        std::wstring rsub;
        if (HiveRoot(path, &root, &rsub))
        {
            HKEY rh = NULL;
            if (Real_RegOpenKeyExW(root, rsub.c_str(), 0, KEY_READ, &rh) == ERROR_SUCCESS)
            {
                Real_RegQueryInfoKeyW(rh, NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL, NULL, &realSecLen, &realFt);
                Real_RegCloseKey(rh);
            }
        }
    }
    if (!realOk && sqlite_reg->KeyState(path) != 1)
    {
        // Non nel DB (viva) e non nel reale: non esiste -> errore reale di open.
        out->status = openErr;
        return true;
    }

    // Unione (stessa strategia di 47/48): reali NON soft-deleted + solo-DB.
    std::vector<EnumEntryW> dbKeys, dbVals;
    DbEnumKeys(sqlite_reg, path, &dbKeys);
    DbEnumValues(sqlite_reg, path, &dbVals);

    out->numSubkeys = 0;
    for (size_t i = 0; i < realKeys.size(); i++)
    {
        if (sqlite_reg->KeyState(path + "\\" + WToUtf8(realKeys[i].name)) == -1)
            continue;   // oscurata dal DB (soft-deleted)
        out->numSubkeys++;
        DWORD l = (DWORD)realKeys[i].name.size();
        if (l > out->maxSubkeyNameLen) out->maxSubkeyNameLen = l;
    }
    for (size_t i = 0; i < dbKeys.size(); i++)
    {
        bool inReal = false;
        for (size_t j = 0; j < realKeys.size(); j++)
            if (realKeys[j].name.compare(dbKeys[i].name) == 0) { inReal = true; break; }
        if (!inReal)
        {
            out->numSubkeys++;
            DWORD l = (DWORD)dbKeys[i].name.size();
            if (l > out->maxSubkeyNameLen) out->maxSubkeyNameLen = l;
        }
    }

    out->numValues = 0;
    for (size_t i = 0; i < realVals.size(); i++)
    {
        if (sqlite_reg->ValueState(path, WToUtf8(realVals[i].name)) == -1)
            continue;   // oscurato dal DB (soft-deleted)
        out->numValues++;
        DWORD l = (DWORD)realVals[i].name.size();
        if (l > out->maxValueNameLen) out->maxValueNameLen = l;
        if ((DWORD)realVals[i].data.size() > out->maxValueDataLen)
            out->maxValueDataLen = (DWORD)realVals[i].data.size();
    }
    for (size_t i = 0; i < dbVals.size(); i++)
    {
        bool inReal = false;
        for (size_t j = 0; j < realVals.size(); j++)
            if (realVals[j].name.compare(dbVals[i].name) == 0) { inReal = true; break; }
        if (!inReal)
        {
            out->numValues++;
            DWORD l = (DWORD)dbVals[i].name.size();
            if (l > out->maxValueNameLen) out->maxValueNameLen = l;
            if ((DWORD)dbVals[i].data.size() > out->maxValueDataLen)
                out->maxValueDataLen = (DWORD)dbVals[i].data.size();
        }
    }

    out->securityDescLen = realSecLen;
    out->lastWrite = realFt;
    out->status = ERROR_SUCCESS;
    return true;
}

// ===== CloseKey (50): rilascia l'id fittizio =====
bool REG_adapter::CloseKey(HKEY id, LSTATUS& out)
{
    HandlerRecord rec;
    if (!handler_mng->getHandler(id, &rec))
        return false;   // id non gestito: il detour chiude l'HKEY originale
    if (rec.type == HandlerType::Real && rec.real)
        out = Real_RegCloseKey(rec.real);
    else
        out = ERROR_SUCCESS;
    handler_mng->removeHandler(id);
    return true;
}

// ===== costruttore =====
REG_adapter::REG_adapter(HMODULE self)
{
    handler_mng = REG_handler_mng::getInstance();

    std::string p = DBPathPrimary(self);
    if (p.empty()) p = DBPathTemp();
    sqlite_reg = new SQLLite_REG(p);

    // dir non scrivibile (es. DLL in cartella read-only): ripiego su %TEMP%.
    // Stesso try-dir-then-temp del log (OpenLog). Se non si apre NESSUN DB,
    // sqlite_reg resta con db==nullptr -> tutti i metodi restituiscono errore
    // -> l'adattatore degrada a pass-through (decisione issue 40).
    if (!sqlite_reg->isOpen())
    {
        std::string t = DBPathTemp();
        if (!t.empty() && t != p)
        {
            SQLLite_REG* alt = new SQLLite_REG(t);
            if (alt->isOpen())      { delete sqlite_reg; sqlite_reg = alt; }
            else                    { delete alt; }
        }
    }
}
