// main.cpp  -  VirtTest: ambiente di test virtuale
//
// Carica la version.dll LOCALE (stessa cartella dell'exe). La DllMain del proxy
// esegue InstallRegHooks: da quel momento le chiamate Reg* di questo exe
// passano PER I DETOUR. Qui si verifica l'ambiente (load + detour attivi +
// helper di ispezione .db) e, una sezione per funzione, i comportamenti
// virtualizzati via via che si implementa REG_adapter (40-50) e il wiring dei
// detour (60-67). Fail fast (regola AGENTS).
//
// Nota: open/create (60/61) e query (62) sono wired e tornano/leggono
// id fittizi; set/enum/info (63/64/65) non sono ancora wired. Le chiavi
// di test hanno nomi univoci per-run (non serve cleanup reale: il reale
// non e' toccato).
//
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#pragma comment(lib, "shell32.lib")
#include <string>
#include <cstring>
#include <fstream>
#include <sqlite3.h>

// ---------------------------------------------------------------- helper
static int s_pass = 0;
static void Ok(const char* w)     { printf("  [OK]   %s\n", w); ++s_pass; }
static void Fail(const char* w)   { printf("  [FAIL] %s\n", w); fflush(stdout); exit(1); }
static void FailGle(const char* w){ Fail((std::string(w) + " (gle=" + std::to_string(GetLastError()) + ")").c_str()); }

// directory dell'exe (la DLL e' nella stessa cartella).
static std::wstring exeDir()
{
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(NULL, p, MAX_PATH);
    std::wstring s(p);
    size_t sl = s.find_last_of(L'\\');
    return (sl != std::wstring::npos) ? s.substr(0, sl) : L".";
}

// wide -> utf-8 (per passare path/substring al sqlite e alla ricerca nel log).
static std::string w2u(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ---- helper di ispezione del file .db (SOLA LETTURA) ----
// Il proxy (nello stesso processo) tiene reg_virtual.db aperto in lettura/
// scrittura; un'apertura in sola lettura con busy timeout coesiste senza
// problemi (lettore conaged shared lock, lo scrittore impiega exclusive solo
// per il commit).
static bool dbOpenRO(const std::string& pathUtf8, sqlite3** out)
{
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(pathUtf8.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_busy_timeout(db, 2000);
    *out = db;
    return true;
}

static bool dbScalar(const sqlite3* db, const char* sql, long long* out)
{
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(const_cast<sqlite3*>(db), sql, -1, &st, nullptr) != SQLITE_OK) return false;
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) { *out = sqlite3_column_int64(st, 0); ok = true; }
    sqlite3_finalize(st);
    return ok;
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);   // debug: output immediato (anche in crash)

    // ---------------------------------------------------- 0) figlio pulito
    // Modalita' "--realcheck <subpath sotto HKCU>": questo exe spawna se
    // stesso cosi' per il check "il create non ha toccato il REALE" (00-phase2).
    // Il figlio NON carica la DLL: nessun detour, RegOpenKeyExW e' genuina.
    // Exit code: 0 = chiave assente nel reale, 1 = presente (FAIL per il padre),
    // 2 = errore inatteso.
    // (Si legge GetCommandLineW + CommandLineToArgvW: l'argv di main() in
    //  MSVC non e' affidabile se dichiarato wide, e un figlio che NON prendesse
    //  questa branch ri-execcherebbe l'intero test in un loop infinito.)
    {
        int argc2 = 0;
        LPWSTR* argv2 = CommandLineToArgvW(GetCommandLineW(), &argc2);
        if (argv2 && argc2 >= 3 && _wcsicmp(argv2[1], L"--realcheck") == 0)
        {
            HKEY rh = NULL;
            LSTATUS st = RegOpenKeyExW(HKEY_CURRENT_USER, argv2[2], 0, KEY_READ, &rh);
            if (st == ERROR_SUCCESS) { RegCloseKey(rh); return 1; }
            if (st == ERROR_FILE_NOT_FOUND) return 0;
            return 2;
        }


        if (argv2 && argc2 >= 3 && _wcsicmp(argv2[1], L"--realmake") == 0)
        {
            // Crea una chiave REALE (figlio senza DLL): per il check del
            // fallback reale del delete (issue 64). Net-zero: il test la
            // cancella subito dopo.
            HKEY kh = NULL;
            LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER, argv2[2], 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &kh, NULL);
            if (st != ERROR_SUCCESS || !kh) return 2;
            wchar_t m[] = L"m";
            RegSetValueExW(kh, L"mark", 0, REG_SZ, (const BYTE*)m, sizeof(m));
            RegCloseKey(kh);
            return 0;
        }
        if (argv2 && argc2 >= 3 && _wcsicmp(argv2[1], L"--realdel") == 0)
        {
            // Cancellazione REALE (figlio senza DLL): per le pulizie e2e (70)
            // quando la chiave ha anche una shadow nel DB (delete virtuale).
            LSTATUS st = RegDeleteKeyW(HKEY_CURRENT_USER, argv2[2]);
            if (st == ERROR_SUCCESS) return 0;
            if (st == ERROR_FILE_NOT_FOUND) return 0;   // gia' assente: ok
            return 3;
        }
        if (argv2 && argc2 >= 5 && _wcsicmp(argv2[1], L"--realval") == 0)
        {
            // Valore REALE (figlio senza DLL): 0 = presente e uguale a argv[4],
            // 1 = assente, 2 = presente ma diverso, 3 = errore. (e2e, issue 70)
            HKEY kv = NULL;
            LSTATUS st = RegOpenKeyExW(HKEY_CURRENT_USER, argv2[2], 0, KEY_READ, &kv);
            if (st != ERROR_SUCCESS) return (st == ERROR_FILE_NOT_FOUND) ? 1 : 3;
            DWORD t = 0, sz = 4096;
            wchar_t buf[4096] = {0};
            st = RegQueryValueExW(kv, argv2[3], NULL, &t, (LPBYTE)buf, &sz);
            RegCloseKey(kv);
            if (st == ERROR_FILE_NOT_FOUND) return 1;
            if (st != ERROR_SUCCESS) return 3;
            return (wcsncmp(buf, argv2[4], 4095) == 0) ? 0 : 2;
        }
        if (argv2) LocalFree(argv2);
    }

    std::wprintf(L"=== VirtTest: ambiente di test virtuale ===\n\n");

    // ------------------------------------------------------------ 1) load
    std::wprintf(L"==== 1) load (version.dll locale) ====\n");
    HMODULE h = LoadLibraryW(L"version.dll");
    if (!h) FailGle("LoadLibraryW(version.dll)");
    wchar_t dllPath[MAX_PATH];
    GetModuleFileNameW(h, dllPath, MAX_PATH);
    std::wstring dllDir; { std::wstring d(dllPath); size_t s = d.find_last_of(L'\\'); dllDir = d.substr(0, s); }
    std::wprintf(L"  caricata: %s\n", dllPath);
    if (dllDir != exeDir())
        Fail("version.dll NON e' quella locale (caricata da un'altra cartella, es. System32)");
    Ok("version.dll locale caricata (DllMain eseguita, hook installati)");

    // ---------------------------------------------------- 2) hooks (detour)
    std::wprintf(L"\n==== 2) hooks (detour attivi) ====\n");
    // marker univoco per run (PID+tick): la ricerca nel log non fa falsi
    // positivi da run precedenti, e fallisce se il detour NON scatta.
    // open di una chiave NON esistente: nessun effetto collaterale, e il log
    // registra la chiamata (anche se fallisce) col marker.
    std::wstring probe = L"WinRegProxyVirtTest" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    std::wstring sub = L"Software\\" + probe;
    HKEY hk = NULL;
    LSTATUS st = RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_READ, &hk);
    if (st != ERROR_FILE_NOT_FOUND)
        FailGle("RegOpenKeyExW sonda: atteso ERROR_FILE_NOT_FOUND (chiave mai creata)");
    // open riuscito di una chiave reale esistente (l'open reale avviene dentro
    // l'adapter; qui si riceve un id fittizio, da chiudere subito).
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software", 0, KEY_READ, &hk) != ERROR_SUCCESS || !hk)
        FailGle("RegOpenKeyExW(HKCU\\Software)");
    RegCloseKey(hk);
    // verifica che le chiamate siano passate PER I DETOUR: il log (accanto alla
    // DLL, append-only, utf-8) deve contenere il marker di questo run.
    std::wstring logPath = exeDir() + L"\\reg_hook.log";
    std::ifstream l2(logPath.c_str(), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(l2)), std::istreambuf_iterator<char>());
    l2.close();
    if (content.find(w2u(probe)) == std::string::npos)
        Fail("marker NON presente in reg_hook.log -> i detour NON sono attivi?");
    Ok("detour attivi (marker nel log) + open reale/virtuale ok");

    // ------------------------------------------------------------ 3) db
    std::wprintf(L"\n==== 3) db (ispezione reg_virtual.db in sola lettura) ====\n");
    std::wstring dbPath = exeDir() + L"\\reg_virtual.db";
    std::wprintf(L"  path atteso del DB del proxy: %s\n", dbPath.c_str());
    if (GetFileAttributesW(dbPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        std::wprintf(L"  stato: assente (atteso: il DB nasce con l'adapter, issue 40)\n");
    else
        std::wprintf(L"  stato: presente\n");
    // self-test del helper su un db temporaneo proprio (rw create + sola lettura)
    wchar_t tmpDir[MAX_PATH]; DWORD tn = GetEnvironmentVariableW(L"TEMP", tmpDir, MAX_PATH);
    std::wstring tmp = (tn > 0 ? std::wstring(tmpDir, tn) : L".") + L"\\virttest_selftest.db";
    DeleteFileW(tmp.c_str());
    {
        sqlite3* wdb = nullptr;
        if (sqlite3_open_v2(w2u(tmp).c_str(), &wdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
            Fail("open (rw) db temporaneo per self-test");
        char* err = nullptr;
        if (sqlite3_exec(wdb, "CREATE TABLE t(k TEXT); INSERT INTO t VALUES('a'),('b'),('c');", nullptr, nullptr, &err) != SQLITE_OK) {
            std::string e = err ? err : "?"; sqlite3_free(err); sqlite3_close(wdb); Fail((std::string("popola db temporaneo: ") + e).c_str());
        }
        sqlite3_close(wdb);
    }
    long long n = -1;
    {
        sqlite3* ro = nullptr;
        if (!dbOpenRO(w2u(tmp), &ro)) Fail("dbOpenRO (self-test)");
        if (!dbScalar(ro, "SELECT COUNT(*) FROM t", &n)) Fail("dbScalar COUNT (self-test)");
        sqlite3_close(ro);
    }
    DeleteFileW(tmp.c_str());
    if (n != 3) Fail(("self-test COUNT atteso 3, trovato " + std::to_string(n)).c_str());
    Ok("helper .db: rw create + sola lettura (COUNT=3)");

    // ---------------------------------------------------- 4) create (61)
    std::wprintf(L"\n==== 4) create (issue 61: SOLO nel DB, mai nel reale) ====\n");
    std::wstring csub = L"Software\\" + probe;   // univoco per-run: prima create = NEW
    std::string cpathUtf8 = w2u(L"HKCU\\" + csub);
    HKEY ck = NULL;
    DWORD disp = 99;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, csub.c_str(), 0, NULL, 0,
        KEY_READ | KEY_WRITE, NULL, &ck, &disp) != ERROR_SUCCESS || !ck)
        FailGle("RegCreateKeyExW chiave di test");
    if (disp != REG_CREATED_NEW_KEY) Fail("disposition atteso REG_CREATED_NEW_KEY");
    RegCloseKey(ck);

    // ri-creazione: EXISTING (coerenza col DB, issue 42)
    disp = 99;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, csub.c_str(), 0, NULL, 0,
        KEY_READ | KEY_WRITE, NULL, &ck, &disp) != ERROR_SUCCESS || !ck)
        FailGle("RegCreateKeyExW ri-creazione");
    if (disp != REG_OPENED_EXISTING_KEY) Fail("disposition atteso REG_OPENED_EXISTING_KEY");
    RegCloseKey(ck);

    // riapertura con open (60): la chiave e' ora virtuale e viva nel DB
    if (RegOpenKeyExW(HKEY_CURRENT_USER, csub.c_str(), 0, KEY_READ, &ck) != ERROR_SUCCESS || !ck)
        FailGle("RegOpenKeyExW chiave virtuale");
    RegCloseKey(ck);

    // la riga e' nel DB (lettura diretta del file del proxy, in sola lettura)
    {
        sqlite3* ro = nullptr;
        if (!dbOpenRO(w2u(dbPath), &ro)) Fail("open db (ro) per check create");
        long long c = -1;
        std::string q = "SELECT COUNT(*) FROM keys WHERE key_path='" + cpathUtf8 + "' AND is_deleted=0";
        if (!dbScalar(ro, q.c_str(), &c)) Fail("query keys (check create)");
        sqlite3_close(ro);
        if (c != 1) Fail(("riga chiave nel DB: atteso 1, trovato " + std::to_string(c)).c_str());
    }
    Ok("create -> DB: riga presente, disposition NEW/EXISTING, riapertura ok");

    // il REALE non e' toccato: check da "processo pulito" (00-phase2) SENZA
    // helper exe dedicato: si spawna questo stesso exe con --realcheck, che
    // NON carica la DLL (detour assenti -> RegOpenKeyExW genuina) ed esce con:
    //   0 = chiave assente nel reale (ci aspettiamo questo)
    //   1 = chiave presente nel reale (il create ha toccato il reale: FAIL)
    //   2 = errore inatteto
    // (Alternativa scartata: NtOpenKey diretto via ntdll — testato, ma un
    //  processo user normale riceve ACCESS_DENIED gia' su \\Registry.)
    {
        wchar_t exePath[MAX_PATH] = {0};
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --realcheck " + csub;
        wchar_t cmdBuf[2048];
        wcsncpy_s(cmdBuf, _countof(cmdBuf), cmdLine.c_str(), _TRUNCATE);
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pinfo = {};
        if (!CreateProcessW(NULL, cmdBuf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pinfo))
            FailGle("CreateProcess (figlio pulito --realcheck)");
        WaitForSingleObject(pinfo.hProcess, 10000);
        DWORD code = 0xFFFF;
        GetExitCodeProcess(pinfo.hProcess, &code);
        CloseHandle(pinfo.hProcess);
        CloseHandle(pinfo.hThread);
        if (code == 1) Fail("chiave TROVATA nel registro REALE -> create non virtualizzato");
        if (code != 0) Fail("figlio pulito: exit code inattesa (2=errore di apertura)");
    }
    Ok("reale non toccato (figlio pulito: chiave assente nel reale)");

    // ---------------------------------------------------- 5) query (62)
    // Read di valori SU CHIAVE VIRTUALE via detour. I valori si seminano nel DB
    // del proxy (apertura rw dello stesso file, come fa l'adapter: un'apertura
    // aggiuntiva nel processo coesiste) e si leggono PER I DETOUR, che devono
    // rispondere dal DB con le semantica dei buffer verificate nei probe p1-5
    // (QVEx 2-pass exact, QV deprecated exact, GV +2 sulle stringhe).
    std::wprintf(L"\n==== 5) query (issue 62: read virtuale via detour) ====\n");
    {
        // CONVENZIONE DB (adattatore QueryValue): le stringhe (SZ/EXPAND/MULTI)
        // sono salvate come BLOB UTF-8 SENZA NUL; l'adapter le converte in wide
        // con NUL finale per la API (come nel reale). I non-stringa restano grezzi.
        // 'ciao' UTF-8 = 63 69 61 6F -> X'6369616F'   (4 B; -> wide 10 B in uscita)
        // 'defv' UTF-8 = 64 65 66 76 -> X'64656676'   (4 B; -> wide 10 B in uscita)
        // 'bye'  UTF-8 = 62 79 65    -> X'627965'     (3 B; soft-deleted qui sotto)
        // dword 42 = X'2A000000' (non-stringa: grezzo)
        std::string k = w2u(L"HKCU\\") + w2u(csub);
        {
            sqlite3* rw = nullptr;
            if (sqlite3_open_v2(w2u(dbPath).c_str(), &rw, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
                Fail("open db (rw) per semina valori");
            sqlite3_busy_timeout(rw, 2000);
            std::string sql =
                "INSERT INTO \"values\"(key_path,name,type,data,is_deleted) VALUES "
                "(?1,'vsz',1,X'6369616F',0),"
                "(?1,'dword',4,X'2A000000',0),"
                "(?1,'',1,X'64656676',0),"
                "(?1,'delme',1,X'627965',0) "
                "ON CONFLICT(key_path,name) DO UPDATE SET type=excluded.type, data=excluded.data, is_deleted=0;";
            sqlite3_stmt* st2 = nullptr;
            if (sqlite3_prepare_v2(rw, sql.c_str(), -1, &st2, nullptr) != SQLITE_OK)
                Fail("prepare semina valori");
            sqlite3_bind_text(st2, 1, k.c_str(), k.size(), SQLITE_TRANSIENT);
            if (sqlite3_step(st2) != SQLITE_DONE) { sqlite3_finalize(st2); sqlite3_close(rw); Fail("semina valori"); }
            sqlite3_finalize(st2);
            // soft-delete di 'delme' (la riga resta, is_deleted=1)
            if (sqlite3_exec(rw, ("UPDATE \"values\" SET is_deleted=1 WHERE key_path='" + k + "' AND name='delme'").c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
                Fail("semina: soft-delete delme");
            sqlite3_close(rw);
        }
        if (RegOpenKeyExW(HKEY_CURRENT_USER, csub.c_str(), 0, KEY_READ, &hk) != ERROR_SUCCESS || !hk)
            FailGle("RegOpenKeyExW chiave con valori");

        // QVExW: sizing (data=NULL -> SUCCESS + dimensione exact)
        DWORD t = 99, cb = 99;
        if (RegQueryValueExW(hk, L"vsz", NULL, &t, NULL, &cb) != ERROR_SUCCESS)
            FailGle("QVExW sizing vsz");
        if (t != REG_SZ || cb != 10)
            Fail("QVExW sizing: atteso type=REG_SZ cb=10");

        // QVExW: lettura (2-pass: il buffer e' sufficientemente grande)
        wchar_t buf[32]; cb = 32; t = 99;
        if (RegQueryValueExW(hk, L"vsz", NULL, &t, (LPBYTE)buf, &cb) != ERROR_SUCCESS)
            FailGle("QVExW read vsz");
        if (t != REG_SZ || cb != 10 || wcsncmp(buf, L"ciao", 4) != 0 || buf[4] != 0)
            Fail("QVExW read vsz: atteso 'ciao' wide 10 B con NUL");

        // QVExW: buffer piccolo -> MORE_DATA + dimensione richiesta
        cb = 8;
        if (RegQueryValueExW(hk, L"vsz", NULL, &t, (LPBYTE)buf, &cb) != ERROR_MORE_DATA || cb != 10)
            Fail("QVExW buffer piccolo: atteso MORE_DATA cb=10");

        // QVExW: valore assente -> NOT_FOUND
        cb = 32; t = 99;
        if (RegQueryValueExW(hk, L"nope", NULL, &t, (LPBYTE)buf, &cb) != ERROR_FILE_NOT_FOUND)
            Fail("QVExW valore assente: atteso FILE_NOT_FOUND");

        // QVExW: DWORD
        cb = 32; t = 99;
        if (RegQueryValueExW(hk, L"dword", NULL, &t, (LPBYTE)buf, &cb) != ERROR_SUCCESS)
            FailGle("QVExW read dword");
        if (t != REG_DWORD || cb != 4 || *(DWORD*)buf != 42)
            Fail("QVExW read dword: atteso 42 (4 B)");
        Ok("QVExW su chiave virtuale: sizing / read / MORE_DATA / NOT_FOUND / DWORD ok");

        // GVW: sizing (data=NULL): quirk +2 sulle stringhe (probe p4/p5)
        DWORD gcb = 99, gt = 99;
        if (RegGetValueW(HKEY_CURRENT_USER, csub.c_str(), L"vsz", RRF_RT_ANY, &gt, NULL, &gcb) != ERROR_SUCCESS)
            FailGle("GVW sizing vsz");
        if (gt != REG_SZ || gcb != 12)
            Fail("GVW sizing: atteso type=REG_SZ cb=12 (10+2 quirk)");

        // GVW: lettura success -> cb exact (niente +2)
        wchar_t gbuf[32]; gcb = 32; gt = 99;
        if (RegGetValueW(HKEY_CURRENT_USER, csub.c_str(), L"vsz", RRF_RT_ANY, &gt, gbuf, &gcb) != ERROR_SUCCESS)
            FailGle("GVW read vsz");
        if (gt != REG_SZ || gcb != 10 || wcsncmp(gbuf, L"ciao", 4) != 0 || gbuf[4] != 0)
            Fail("GVW read vsz: atteso 'ciao' (10 B exact)");

        // GVW: buffer piccolo (11 < 10+2) -> MORE_DATA cb=12
        gcb = 11;
        if (RegGetValueW(HKEY_CURRENT_USER, csub.c_str(), L"vsz", RRF_RT_ANY, &gt, gbuf, &gcb) != ERROR_MORE_DATA || gcb != 12)
            Fail("GVW buffer piccolo: atteso MORE_DATA cb=12");

        // GVW: RRF_RT mismatch (DWORD chiesto, SZ presente) -> 1630
        gcb = 32;
        if (RegGetValueW(HKEY_CURRENT_USER, csub.c_str(), L"vsz", RRF_RT_REG_DWORD, &gt, gbuf, &gcb) != ERROR_INVALID_DATA)
            Fail("GVW RRF_RT mismatch: atteso ERROR_INVALID_DATA (1630)");
        Ok("GVW su chiave virtuale: sizing +2 / read exact / MORE_DATA / mismatch 1630 ok");

        // QVW (deprecated): default della chiave, read + sizing
        LONG lcb = 32;
        if (RegQueryValueW(HKEY_CURRENT_USER, csub.c_str(), buf, &lcb) != ERROR_SUCCESS)
            FailGle("QVW (depr) read default");
        if (lcb != 10 || wcsncmp(buf, L"defv", 4) != 0 || buf[4] != 0)
            Fail("QVW (depr) read default: atteso 'defv' (10 B)");
        lcb = 0;
        if (RegQueryValueW(HKEY_CURRENT_USER, csub.c_str(), NULL, &lcb) != ERROR_SUCCESS || lcb != 10)
            Fail("QVW (depr) sizing default: atteso 10");
        Ok("QVW (depr) su chiave virtuale: read + sizing default ok");

        // valore soft-deleted (riga presente, is_deleted=1): il DB vince sul
        // reale -> NOT_FOUND (non c'e' reale qui, ma la riga deve oscurare)
        cb = 32; t = 99;
        if (RegQueryValueExW(hk, L"delme", NULL, &t, (LPBYTE)buf, &cb) != ERROR_FILE_NOT_FOUND)
            Fail("QVExW valore soft-deleted: atteso FILE_NOT_FOUND");
        Ok("valore soft-deleted oscurato (NOT_FOUND)");

        // close di un id fittizio: il detour e' ancora solo log (wiring 67/50),
        // Real_RegCloseKey su un id fittizio risponde FILE_NOT_FOUND (2): atteso
        // finche' 67 non passa dall'adattatore, quindi non si asserisce.
        RegCloseKey(hk);
    }

    // ---------------------------------------------------- 6) set (44/63)
    // Scrittura via detour su chiave virtuale: il valore va SOLO nel DB, mai
    // nel reale; la readback avviene via detour di query (62). I dati si
    // passano nel formato API (stringhe wide/ANSI con NUL, DWORD grezzo):
    // l'adattatore li normalizza alla convenzione DB (stringhe UTF-8 senza
    // NUL). Il DB e' lo stesso file del proxy: l'apertura rw parallela
    // coesiste (lo fa gia' la sezione 5).
    std::wprintf(L"\n==== 6) set (issue 63: scrittura virtuale via detour) ====\n");
    {
        std::wstring ssub = L"Software\\" + probe + L"_set";   // chiave nuova per-run
        HKEY hk = NULL;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, ssub.c_str(), 0, NULL, 0,
            KEY_READ | KEY_WRITE, NULL, &hk, NULL) != ERROR_SUCCESS || !hk)
            FailGle("RegCreateKeyExW chiave set");
        std::string skey = w2u(L"HKCU\\") + w2u(ssub);
        sqlite3* rw = nullptr;
        if (sqlite3_open_v2(w2u(dbPath).c_str(), &rw, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
            Fail("open db (rw) per check valori");
        sqlite3_busy_timeout(rw, 2000);
        // helper: (type, hex(data)) del valore nel DB
        auto dbVal = [&](const wchar_t* nm) -> std::pair<long long, std::string>
        {
            std::pair<long long, std::string> out{-1, ""};
            std::string q = "SELECT type, hex(data) FROM \"values\" WHERE key_path='" + skey + "' AND name='" + w2u(nm) + "'";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(rw, q.c_str(), -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
            {
                out.first = sqlite3_column_int64(st, 0);
                const char* hx = (const char*)sqlite3_column_text(st, 1);
                if (hx) out.second = hx;
            }
            if (st) sqlite3_finalize(st);
            return out;
        };

        // --- 1) SetValueExW SZ: creazione + overwrite
        wchar_t ws[] = L"valore";   // 6 char + NUL = 14 B
        if (RegSetValueExW(hk, L"wstr", 0, REG_SZ, (const BYTE*)ws, sizeof(ws)) != ERROR_SUCCESS)
            FailGle("SetValueExW wstr");
        std::pair<long long, std::string> v = dbVal(L"wstr");
        if (v.first != REG_SZ || v.second != "76616C6F7265")   // 'valore' UTF-8, senza NUL
            Fail("DB wstr: atteso SZ utf8 'valore'");
        // overwrite: una sola riga, dati nuovi (niente duplicati)
        wchar_t wa[] = L"altro";
        if (RegSetValueExW(hk, L"wstr", 0, REG_SZ, (const BYTE*)wa, sizeof(wa)) != ERROR_SUCCESS)
            FailGle("SetValueExW overwrite");
        v = dbVal(L"wstr");
        if (v.first != REG_SZ || v.second != "616C74726F")
            Fail("DB overwrite: atteso 'altro'");
        long long c = -1;
        {
            sqlite3_stmt* st = nullptr;
            std::string q = "SELECT COUNT(*) FROM \"values\" WHERE key_path='" + skey + "' AND name='wstr'";
            if (sqlite3_prepare_v2(rw, q.c_str(), -1, &st, nullptr) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW)
                Fail("query conteggio righe wstr");
            c = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
        if (c != 1) Fail("DB wstr: attesa 1 riga (overwrite, non duplicato)");
        // readback via detour
        {
            DWORD rt = 99, rc = 99;
            if (RegQueryValueExW(hk, L"wstr", NULL, &rt, NULL, &rc) != ERROR_SUCCESS || rt != REG_SZ || rc != 12)
                Fail("readback wstr: atteso SZ 12 B ('altro' wide)");
        }
        Ok("SetValueExW: creazione + overwrite -> DB (1 riga), readback ok, reale intatto");

        // --- 2) SetValueExW DWORD (non-stringa: byte grezzi)
        DWORD dw = 77;
        if (RegSetValueExW(hk, L"d2", 0, REG_DWORD, (const BYTE*)&dw, sizeof(dw)) != ERROR_SUCCESS)
            FailGle("SetValueExW dword");
        v = dbVal(L"d2");
        if (v.first != REG_DWORD || v.second != "4D000000")
            Fail("DB d2: atteso DWORD 77");
        {
            BYTE rbuf[16]; DWORD rt = 99, rc = 16;
            if (RegQueryValueExW(hk, L"d2", NULL, &rt, rbuf, &rc) != ERROR_SUCCESS || rt != REG_DWORD || rc != 4 || *(DWORD*)rbuf != 77)
                Fail("readback d2: atteso 77");
        }
        Ok("SetValueExW DWORD -> DB grezzo + readback ok");

        // --- 3) SetValueA: nome e dati ANSI (cb=0 -> API calcola strlen+1)
        if (RegSetValueA(hk, "astr", REG_SZ, "ascii", 0) != ERROR_SUCCESS)
            FailGle("SetValueA astr");
        v = dbVal(L"astr");
        if (v.first != REG_SZ || v.second != "6173636969")   // 'ascii'
            Fail("DB astr (ANSI): atteso 'ascii'");
        {
            wchar_t rwb[16]; DWORD rt = 99, rc = 16;
            if (RegQueryValueExW(hk, L"astr", NULL, &rt, (LPBYTE)rwb, &rc) != ERROR_SUCCESS || rc != 12 || wcscmp(rwb, L"ascii") != 0)
                Fail("readback astr: atteso 'ascii'");
        }
        Ok("SetValueA (ANSI) -> DB + readback ok");

        // --- 4) RegSetKeyValueW / A (root + subkey + value)
        wchar_t wskv[] = L"skvval";
        if (RegSetKeyValueW(HKEY_CURRENT_USER, ssub.c_str(), L"skv", REG_SZ, wskv, sizeof(wskv)) != ERROR_SUCCESS)
            FailGle("SetKeyValueW skv");
        v = dbVal(L"skv");
        if (v.first != REG_SZ || v.second != "736B7676616C")
            Fail("DB skv: atteso 'skvval'");
        if (RegSetKeyValueA(HKEY_CURRENT_USER, w2u(ssub).c_str(), "skva", REG_SZ, "ansiskv", 0) != ERROR_SUCCESS)
            FailGle("SetKeyValueA skva");
        v = dbVal(L"skva");
        if (v.first != REG_SZ || v.second != "616E7369736B76")
            Fail("DB skva: atteso 'ansiskv'");
        // reale non toccato: check da "processo pulito" (--realcheck, senza
        // detour): la chiave stessa non deve esistere nel registro reale
        // (se non esiste la chiave, non possono esserci i valori).
        {
            wchar_t exePath[MAX_PATH] = {0};
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --realcheck " + ssub;
            wchar_t cmdBuf[2048];
            wcsncpy_s(cmdBuf, _countof(cmdBuf), cmdLine.c_str(), _TRUNCATE);
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pinfo = {};
            if (!CreateProcessW(NULL, cmdBuf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pinfo))
                FailGle("CreateProcess (figlio pulito --realcheck)");
            WaitForSingleObject(pinfo.hProcess, 10000);
            DWORD code = 0xFFFF;
            GetExitCodeProcess(pinfo.hProcess, &code);
            CloseHandle(pinfo.hProcess);
            CloseHandle(pinfo.hThread);
            if (code == 1) Fail("chiave TROVATA nel registro REALE -> set non virtualizzato");
            if (code != 0) Fail("figlio pulito: exit code inattesa");
        }
        Ok("SetKeyValueW/A -> DB, reale intatto");

        // --- 5) resurrezione (25): valore soft-deleted -> set = risorge
        if (RegSetValueExW(hk, L"gone", 0, REG_SZ, (const BYTE*)ws, sizeof(ws)) != ERROR_SUCCESS)
            FailGle("SetValueExW gone (prima)");
        if (sqlite3_exec(rw, ("UPDATE \"values\" SET is_deleted=1 WHERE key_path='" + skey + "' AND name='gone'").c_str(),
            nullptr, nullptr, nullptr) != SQLITE_OK)
            Fail("resurrezione: soft-delete grezzo");
        wchar_t wy[] = L"y";
        if (RegSetValueExW(hk, L"gone", 0, REG_SZ, (const BYTE*)wy, sizeof(wy)) != ERROR_SUCCESS)
            FailGle("SetValueExW gone (resurrezione)");
        v = dbVal(L"gone");
        if (v.first != REG_SZ || v.second != "79")
            Fail("resurrezione: atteso 'y' nel DB");
        {
            sqlite3_stmt* st = nullptr;
            std::string q = "SELECT COUNT(*) FROM \"values\" WHERE key_path='" + skey + "' AND name='gone'";
            if (sqlite3_prepare_v2(rw, q.c_str(), -1, &st, nullptr) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW)
                Fail("query conteggio righe gone");
            c = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
        if (c != 1) Fail("resurrezione: attesa 1 riga per 'gone'");
        {
            wchar_t rwb[16]; DWORD rc = 16;
            if (RegQueryValueExW(hk, L"gone", NULL, nullptr, (LPBYTE)rwb, &rc) != ERROR_SUCCESS || rc != 4 || wcscmp(rwb, L"y") != 0)
                Fail("readback gone: atteso 'y'");
        }
        Ok("resurrezione: soft-deleted risorge al set (1 riga), readback ok");

        sqlite3_close(rw);
        RegCloseKey(hk);
    }
    std::wprintf(L"\n==== 7) delete (issues 64/45/46: cancellazione SOLO nel DB) ====\n");
    {
        std::wstring dsub = L"Software\\" + probe + L"_del";
        std::string dkey = w2u(L"HKCU\\") + w2u(dsub);
        sqlite3* rw = nullptr;
        if (sqlite3_open_v2(w2u(dbPath).c_str(), &rw, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
            Fail("open db (rw) per check delete");
        sqlite3_busy_timeout(rw, 2000);
        auto valState = [&](const std::string& kpath, const wchar_t* nm) -> long long
        {
            long long out = 2;   // 2 = assente
            std::string q = "SELECT is_deleted FROM \"values\" WHERE key_path='" + kpath + "' AND name='" + w2u(nm) + "'";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(rw, q.c_str(), -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
                out = sqlite3_column_int(st, 0);
            if (st) sqlite3_finalize(st);
            return out;
        };
        auto keyState = [&](const std::string& kpath) -> long long
        {
            long long out = 2;
            std::string q = "SELECT is_deleted FROM keys WHERE key_path='" + kpath + "'";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(rw, q.c_str(), -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
                out = sqlite3_column_int(st, 0);
            if (st) sqlite3_finalize(st);
            return out;
        };
        auto realcheck = [&](const std::wstring& sub) -> DWORD
        {
            wchar_t exePath[MAX_PATH] = {0};
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --realcheck " + sub;
            wchar_t cmdBuf[2048];
            wcsncpy_s(cmdBuf, _countof(cmdBuf), cmdLine.c_str(), _TRUNCATE);
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pinfo = {};
            if (!CreateProcessW(NULL, cmdBuf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pinfo))
                FailGle("CreateProcess (figlio pulito)");
            WaitForSingleObject(pinfo.hProcess, 10000);
            DWORD code = 0xFFFF;
            GetExitCodeProcess(pinfo.hProcess, &code);
            CloseHandle(pinfo.hProcess);
            CloseHandle(pinfo.hThread);
            return code;
        };

        // setup: chiave virtuale + 2 valori + sottochiave con valore
        HKEY hk = NULL;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, dsub.c_str(), 0, NULL, 0,
            KEY_READ | KEY_WRITE, NULL, &hk, NULL) != ERROR_SUCCESS || !hk)
            FailGle("RegCreateKeyExW chiave delete");
        wchar_t w1[] = L"1", w2[] = L"2", w3[] = L"3";
        if (RegSetValueExW(hk, L"dv1", 0, REG_SZ, (const BYTE*)w1, sizeof(w1)) != ERROR_SUCCESS)
            FailGle("SetValueExW dv1");
        if (RegSetValueExW(hk, L"dv2", 0, REG_SZ, (const BYTE*)w2, sizeof(w2)) != ERROR_SUCCESS)
            FailGle("SetValueExW dv2");
        // child via PATH DI ROOT (issue 68: il parent fittizio non e' ancora
        // risolto nel detour create; qui serve solo una sottochiave nel DB)
        std::wstring dsubChild = dsub + L"\\child";
        HKEY ch = NULL;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, dsubChild.c_str(), 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &ch, NULL) != ERROR_SUCCESS || !ch)
            FailGle("RegCreateKeyExW child");
        if (RegSetValueExW(ch, L"cv", 0, REG_SZ, (const BYTE*)w3, sizeof(w3)) != ERROR_SUCCESS)
            FailGle("SetValueExW cv");
        RegCloseKey(ch);

        // --- 1) RegDeleteValueW: valore -> SOLO nel DB (soft-delete)
        if (RegDeleteValueW(hk, L"dv1") != ERROR_SUCCESS)
            FailGle("DeleteValueW dv1");
        if (valState(dkey, L"dv1") != 1)
            Fail("dv1: atteso is_deleted=1 nel DB");
        {
            BYTE d1[32]; DWORD d1t = 99, d1c = 32;
            if (RegQueryValueExW(hk, L"dv1", NULL, &d1t, (LPBYTE)d1, &d1c) != ERROR_FILE_NOT_FOUND)
                FailGle("dv1: query dopo delete attesa NOT_FOUND");
        }
        // gia' cancellato: di nuovo NOT_FOUND (record virtuale: mai il reale)
        if (RegDeleteValueA(hk, "dv1") != ERROR_FILE_NOT_FOUND)
            FailGle("DeleteValueA dv1 (gia' cancellato): atteso NOT_FOUND");
        Ok("DeleteValueW: soft-deleted nel DB, query -> NOT_FOUND; DeleteValueA su gia' cancellato -> NOT_FOUND");

        // --- 2) RegDeleteKeyValueW (root + subkey + value)
        if (RegDeleteKeyValueW(HKEY_CURRENT_USER, dsub.c_str(), L"dv2") != ERROR_SUCCESS)
            FailGle("DeleteKeyValueW dv2");
        if (valState(dkey, L"dv2") != 1)
            Fail("dv2: atteso is_deleted=1 nel DB");
        {
            BYTE d2[32]; DWORD d2t = 99, d2c = 32;
            if (RegQueryValueExW(hk, L"dv2", NULL, &d2t, (LPBYTE)d2, &d2c) != ERROR_FILE_NOT_FOUND)
                FailGle("dv2: query dopo delete attesa NOT_FOUND");
        }
        Ok("DeleteKeyValueW (root+sub): valore soft-deleted nel DB, query -> NOT_FOUND");

        // --- 3) RegDeleteKeyW su sottochiave virtuale: chiave + sottoalbero nascosti
        if (RegDeleteKeyW(hk, L"child") != ERROR_SUCCESS)
            FailGle("DeleteKeyW child");
        if (keyState(dkey + "\\child") != 1)
            Fail("child: atteso is_deleted=1 nel DB");
        {
            HKEY rh = NULL;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, (dsub + L"\\child").c_str(), 0, KEY_READ, &rh) != ERROR_FILE_NOT_FOUND)
                FailGle("child: riopen attesa NOT_FOUND");
        }
        // il padre e' ancora vivo
        {
            HKEY pk = NULL;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, dsub.c_str(), 0, KEY_READ, &pk) != ERROR_SUCCESS || !pk)
                FailGle("riopen padre (atteso vivo)");
            RegCloseKey(pk);
        }
        Ok("DeleteKeyW su sottochiave: sottoalbero nascosto (riopen -> NOT_FOUND), padre intatto");

        // --- 4) RegDeleteTreeW: albero intero
        if (RegDeleteTreeW(HKEY_CURRENT_USER, dsub.c_str()) != ERROR_SUCCESS)
            FailGle("DeleteTreeW");
        if (keyState(dkey) != 1)
            Fail("dsub: atteso is_deleted=1 (albero soft-deleted)");
        {
            HKEY th = NULL;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, dsub.c_str(), 0, KEY_READ, &th) != ERROR_FILE_NOT_FOUND)
                FailGle("dsub: riopen attesa NOT_FOUND");
        }
        Ok("DeleteTreeW: albero soft-deleted, riopen -> NOT_FOUND");
        RegCloseKey(hk);

        // --- 5) fallback reale: root + assente dal DB -> il REALE si tocca
        // (figlio pulito crea una chiave REALE senza DLL; il detour in-process
        // la cancella davvero; poi il figlio pulito verifica l'assenza)
        std::wstring rsub = L"Software\\" + probe + L"_delreal";
        {
            wchar_t exePath[MAX_PATH] = {0};
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --realmake " + rsub;
            wchar_t cmdBuf[2048];
            wcsncpy_s(cmdBuf, _countof(cmdBuf), cmdLine.c_str(), _TRUNCATE);
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pinfo = {};
            if (!CreateProcessW(NULL, cmdBuf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pinfo))
                FailGle("CreateProcess (figlio --realmake)");
            WaitForSingleObject(pinfo.hProcess, 10000);
            DWORD code = 0xFFFF;
            GetExitCodeProcess(pinfo.hProcess, &code);
            CloseHandle(pinfo.hProcess);
            CloseHandle(pinfo.hThread);
            if (code != 0) Fail("figlio --realmake: exit code inattesa");
        }
        if (RegDeleteKeyW(HKEY_CURRENT_USER, rsub.c_str()) != ERROR_SUCCESS)
            FailGle("DeleteKeyW chiave reale (fallback)");
        if (realcheck(rsub) != 0)
            Fail("chiave reale ancora presente dopo il delete (figlio pulito)");
        Ok("fallback reale: assente dal DB + root -> chiave reale cancellata davvero");

        // --- 6) reale non toccato dalle cancellazioni virtuali
        if (realcheck(dsub) != 0)
            Fail("chiave virtuale TROVATA nel reale (figlio pulito)");
        Ok("reale non toccato (figlio pulito: chiavi virtuali assenti nel reale)");

        sqlite3_close(rw);
    }
    // ==========================================================
    std::wprintf(L"\n==== 8) enum (issues 65/47/48: unione DB+reale via detour) ====\n");
    {
        std::wstring esub = L"Software\\" + probe + L"_enum";
        HKEY hk = NULL;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, esub.c_str(), 0, NULL, 0,
            KEY_READ | KEY_WRITE, NULL, &hk, NULL) != ERROR_SUCCESS || !hk)
            FailGle("RegCreateKeyExW chiave enum");
        // child via PATH DI ROOT (issue 68: parent fittizio non risolto nel detour create)
        std::wstring csub = esub + L"\\child1";
        HKEY hc = NULL;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, csub.c_str(), 0, NULL, 0,
            KEY_READ | KEY_WRITE, NULL, &hc, NULL) != ERROR_SUCCESS || !hc)
            FailGle("RegCreateKeyExW child1");
        RegCloseKey(hc);
        wchar_t w1[] = L"1", w2[] = L"2";
        if (RegSetValueExW(hk, L"ev1", 0, REG_SZ, (const BYTE*)w1, sizeof(w1)) != ERROR_SUCCESS)
            FailGle("SetValueExW ev1");
        if (RegSetValueExW(hk, L"ev2", 0, REG_SZ, (const BYTE*)w2, sizeof(w2)) != ERROR_SUCCESS)
            FailGle("SetValueExW ev2");
        RegCloseKey(hk);

        // riapri per l'handle fittizio (stesso pattern della sezione 5)
        HKEY hq = NULL;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, esub.c_str(), 0, KEY_READ, &hq) != ERROR_SUCCESS || !hq)
            FailGle("RegOpenKeyExW chiave enum");

        WCHAR nm[MAX_PATH];
        BYTE dbuf[64];
        DWORD cch = MAX_PATH, t = 0, cb = sizeof(dbuf);

        // --- 1) EnumKeyExW: chiave virtuale pura -> solo il child DB
        if (RegEnumKeyExW(hq, 0, nm, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            FailGle("EnumKeyExW idx0 (attesa child1)");
        if (wcscmp(nm, L"child1") != 0) Fail("EnumKeyExW idx0: nome inatteso");
        cch = MAX_PATH;
        if (RegEnumKeyExW(hq, 1, nm, &cch, NULL, NULL, NULL, NULL) != ERROR_NO_MORE_ITEMS)
            Fail("EnumKeyExW idx1: attesa NO_MORE_ITEMS");
        Ok("EnumKeyExW: child1 + NO_MORE_ITEMS (chiave virtuale pura)");

        // --- 2) EnumValueW: 2 valori DB in formato API + NO_MORE
        if (RegEnumValueW(hq, 0, nm, &cch, NULL, &t, dbuf, &cb) != ERROR_SUCCESS)
            FailGle("EnumValueW idx0 (attesa ev1)");
        if (wcscmp(nm, L"ev1") != 0) Fail("EnumValueW idx0: nome inatteso");
        if (t != REG_SZ || cb != sizeof(w1) || memcmp(dbuf, w1, sizeof(w1)) != 0)
            Fail("EnumValueW idx0: type/dato inattesi (REG_SZ wide con NUL)");
        cch = MAX_PATH; cb = sizeof(dbuf);
        if (RegEnumValueW(hq, 1, nm, &cch, NULL, &t, dbuf, &cb) != ERROR_SUCCESS)
            FailGle("EnumValueW idx1 (attesa ev2)");
        if (wcscmp(nm, L"ev2") != 0) Fail("EnumValueW idx1: nome inatteso");
        if (t != REG_SZ || cb != sizeof(w2) || memcmp(dbuf, w2, sizeof(w2)) != 0)
            Fail("EnumValueW idx1: type/dato inattesi");
        cch = MAX_PATH; cb = sizeof(dbuf);
        if (RegEnumValueW(hq, 2, nm, &cch, NULL, &t, dbuf, &cb) != ERROR_NO_MORE_ITEMS)
            Fail("EnumValueW idx2: attesa NO_MORE_ITEMS");
        Ok("EnumValueW: ev1/ev2 (REG_SZ, dato wide con NUL) + NO_MORE_ITEMS");

        // --- 3) buffer piccoli -> MORE_DATA + dimensioni aggiornate
        cch = 2;
        if (RegEnumKeyExW(hq, 0, nm, &cch, NULL, NULL, NULL, NULL) != ERROR_MORE_DATA)
            Fail("EnumKeyExW: attesa MORE_DATA (buffer 2)");
        if (cch < 7) Fail("EnumKeyExW MORE_DATA: cch non aggiornata (need 7 per 'child1')");
        cch = MAX_PATH; cb = 2;
        if (RegEnumValueW(hq, 0, nm, &cch, NULL, &t, dbuf, &cb) != ERROR_MORE_DATA)
            Fail("EnumValueW: attesa MORE_DATA (cb 2)");
        if (cb != sizeof(w1)) Fail("EnumValueW MORE_DATA: cb non aggiornata");
        Ok("MORE_DATA: buffer piccoli -> ERROR_MORE_DATA + dimensioni aggiornate");

        // --- 4) chiave virtuale VUOTA -> subito NO_MORE
        HKEY hch = NULL;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, csub.c_str(), 0, KEY_READ, &hch) != ERROR_SUCCESS || !hch)
            FailGle("RegOpenKeyExW child1");
        cch = MAX_PATH; cb = sizeof(dbuf);
        if (RegEnumValueW(hch, 0, nm, &cch, NULL, &t, dbuf, &cb) != ERROR_NO_MORE_ITEMS)
            Fail("EnumValueW child1 vuota: attesa NO_MORE_ITEMS");
        cch = MAX_PATH;
        if (RegEnumKeyExW(hch, 0, nm, &cch, NULL, NULL, NULL, NULL) != ERROR_NO_MORE_ITEMS)
            Fail("EnumKeyExW child1 vuota: attesa NO_MORE_ITEMS");
        RegCloseKey(hch);
        Ok("Enum su chiave vuota: NO_MORE_ITEMS immediato");

        // --- 5) merge DB+reale: child VIRTUALE diretta di HKCU compare
        // nell'enum della HKCU (handle REALE di root -> unione DB+reale)
        std::wstring dirsub = L"WinRegProbe" + probe + L"_enumdir";
        HKEY hd = NULL;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, dirsub.c_str(), 0, NULL, 0,
            KEY_READ | KEY_WRITE, NULL, &hd, NULL) != ERROR_SUCCESS || !hd)
            FailGle("RegCreateKeyExW dirsub");
        RegCloseKey(hd);
        bool found = false;
        for (DWORD i = 0; i < 200; i++)
        {
            cch = MAX_PATH;
            LSTATUS st = RegEnumKeyExW(HKEY_CURRENT_USER, i, nm, &cch, NULL, NULL, NULL, NULL);
            if (st == ERROR_NO_MORE_ITEMS) break;
            if (st != ERROR_SUCCESS) FailGle("EnumKeyExW HKCU (scan merge)");
            if (_wcsicmp(nm, dirsub.c_str()) == 0) { found = true; break; }
        }
        if (!found)
            Fail("merge: child virtuale di HKCU non trovata nell'enum reale di HKCU");
        if (RegDeleteKeyW(HKEY_CURRENT_USER, dirsub.c_str()) != ERROR_SUCCESS)
            FailGle("DeleteKeyW dirsub (cleanup)");
        Ok("merge: child virtuale di HKCU presente nell'enum (unione DB+reale)");

        RegCloseKey(hq);

        // --- 6) reale non toccato: esub assente nel reale (figlio pulito)
        {
            wchar_t exePath[MAX_PATH] = {0};
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --realcheck " + esub;
            wchar_t cmdBuf[2048];
            wcsncpy_s(cmdBuf, _countof(cmdBuf), cmdLine.c_str(), _TRUNCATE);
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pinfo = {};
            if (!CreateProcessW(NULL, cmdBuf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pinfo))
                FailGle("CreateProcess (figlio pulito --realcheck)");
            WaitForSingleObject(pinfo.hProcess, 10000);
            DWORD code = 0xFFFF;
            GetExitCodeProcess(pinfo.hProcess, &code);
            CloseHandle(pinfo.hProcess);
            CloseHandle(pinfo.hThread);
            if (code != 0)
                Fail("chiave enum TROVATA nel reale (figlio pulito)");
        }
        Ok("reale non toccato (figlio pulito: chiave enum assente nel reale)");
    }

    std::wprintf(L"\n==== 9) queryinfo+close (issues 66/49/67/50: unione DB+reale, close) ====\n");
    {
        // helper figlio pulito (stesso pattern delle sezioni 4/8)
        auto realcheck = [](const std::wstring& csub) -> DWORD {
            wchar_t exePath[MAX_PATH] = {0};
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --realcheck " + csub;
            wchar_t cmdBuf[2048];
            wcsncpy_s(cmdBuf, _countof(cmdBuf), cmdLine.c_str(), _TRUNCATE);
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pinfo = {};
            if (!CreateProcessW(NULL, cmdBuf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pinfo))
                FailGle("CreateProcess (figlio pulito --realcheck)");
            WaitForSingleObject(pinfo.hProcess, 10000);
            DWORD code = 0xFFFF;
            GetExitCodeProcess(pinfo.hProcess, &code);
            CloseHandle(pinfo.hProcess);
            CloseHandle(pinfo.hThread);
            return code;
        };
        auto realmake = [](const std::wstring& csub) {
            wchar_t exePath[MAX_PATH] = {0};
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --realmake " + csub;
            wchar_t cmdBuf[2048];
            wcsncpy_s(cmdBuf, _countof(cmdBuf), cmdLine.c_str(), _TRUNCATE);
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pinfo = {};
            if (!CreateProcessW(NULL, cmdBuf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pinfo))
                FailGle("CreateProcess (figlio pulito --realmake)");
            WaitForSingleObject(pinfo.hProcess, 10000);
            DWORD code = 0xFFFF;
            GetExitCodeProcess(pinfo.hProcess, &code);
            CloseHandle(pinfo.hProcess);
            CloseHandle(pinfo.hThread);
            if (code != 0) Fail("realmake: exit code inattesa");
        };

        // setup: chiave virtuale pura (2 child + 2 valori) via path di root
        std::wstring qsub = L"Software\\" + probe + L"_qinfo";
        HKEY hq = NULL;
        HKEY hd = NULL;   // handle scartato per i child
        if (RegCreateKeyExW(HKEY_CURRENT_USER, qsub.c_str(), 0, NULL, 0,
            KEY_READ | KEY_WRITE, NULL, &hq, NULL) != ERROR_SUCCESS || !hq)
            FailGle("RegCreateKeyExW qinfo");
        if (RegCreateKeyExW(HKEY_CURRENT_USER, (qsub + L"\\child1").c_str(), 0, NULL, 0,
            KEY_READ | KEY_WRITE, NULL, &hd, NULL) != ERROR_SUCCESS)
            FailGle("RegCreateKeyExW child1");
        RegCloseKey(hd);
        if (RegCreateKeyExW(HKEY_CURRENT_USER, (qsub + L"\\child2").c_str(), 0, NULL, 0,
            KEY_READ | KEY_WRITE, NULL, &hd, NULL) != ERROR_SUCCESS)
            FailGle("RegCreateKeyExW child2");
        RegCloseKey(hd);
        wchar_t w1[] = L"1", w2[] = L"22";
        if (RegSetValueExW(hq, L"ev1", 0, REG_SZ, (const BYTE*)w1, sizeof(w1)) != ERROR_SUCCESS)
            FailGle("SetValueExW ev1");
        if (RegSetValueExW(hq, L"ev2", 0, REG_SZ, (const BYTE*)w2, sizeof(w2)) != ERROR_SUCCESS)
            FailGle("SetValueExW ev2");
        RegCloseKey(hq);
        HKEY hq2 = NULL;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, qsub.c_str(), 0, KEY_READ, &hq2) != ERROR_SUCCESS || !hq2)
            FailGle("RegOpenKeyExW qinfo");
        HKEY hc2 = NULL;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, (qsub + L"\\child2").c_str(), 0, KEY_READ, &hc2) != ERROR_SUCCESS || !hc2)
            FailGle("RegOpenKeyExW child2");

        // 1) chiave virtuale pura: conteggi dal DB (2 child / 2 valori).
        // Parametri SDK: hKey, lpClass, lpcchClass, lpReserved, lpcSubKeys,
        // lpcbMaxSubKeyLen, lpcbMaxClassLen, lpcValues, lpcbMaxValueNameLen,
        // lpcbMaxValueLen, lpcbSecurityDescriptor, lpftLastWriteTime.
        {
            DWORD cls = 0, resv = 0, nsub = 0, mxsl = 0, mxcl = 0, nval = 0, mxvn = 0, mxd = 0, sdl = 0;
            FILETIME ft = {};
            if (RegQueryInfoKeyW(hq2, NULL, &cls, &resv, &nsub, &mxsl, &mxcl, &nval, &mxvn, &mxd, &sdl, &ft) != ERROR_SUCCESS)
                FailGle("RegQueryInfoKeyW virtuale");
            // 2 child (max nome 7 = child1), 2 valori (max nome 3, max dato 6 = "22"+NUL)
            if (nsub != 2 || nval != 2 || mxsl != 6 || mxcl != 0 || mxvn != 3 || mxd != sizeof(w2))
                Fail("QueryInfoKeyW virtuale: conteggi inattesi (attesa 2/2, max 7/0/3/6)");
        }
        Ok("QueryInfoKeyW chiave virtuale: 2 child / 2 valori (solo DB)");

        // 2) chiave REALE (realmake): open -> id fittizio Real record -> info reali
        std::wstring rsub = L"Software\\" + probe + L"_qinfo_real";
        realmake(rsub);
        HKEY hr = NULL;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, rsub.c_str(), 0, KEY_READ, &hr) != ERROR_SUCCESS || !hr)
            FailGle("RegOpenKeyExW chiave reale");
        {
            DWORD cls = 0, resv = 0, nsub = 0, mxsl = 0, mxcl = 0, nval = 0, mxvn = 0, mxd = 0, sdl = 0;
            FILETIME ft = {};
            if (RegQueryInfoKeyW(hr, NULL, &cls, &resv, &nsub, &mxsl, &mxcl, &nval, &mxvn, &mxd, &sdl, &ft) != ERROR_SUCCESS)
                FailGle("RegQueryInfoKeyW reale");
            // realmake: 0 child, 1 valore 'mark' (REG_SZ wide "m" = 4 byte)
            if (nsub != 0 || nval != 1 || mxvn != 4 || mxd != 4)
                Fail("QueryInfoKeyW reale: conteggi inattesi (attesa 0/1, mark=4 byte)");
        }
        Ok("QueryInfoKeyW chiave reale (record Real): 0 child / 1 valore reale");

        // 3) merge DB+reale: semina 1 child + 1 valore SOLO-DB sotto la chiave reale
        {
            sqlite3* rw = NULL;
            if (sqlite3_open_v2(w2u(dbPath).c_str(), &rw, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
                Fail("open db (rw) per semina merge");
            sqlite3_busy_timeout(rw, 2000);
            std::string k = w2u(L"HKCU\\") + w2u(rsub);
            std::string kc = w2u(L"HKCU\\") + w2u(rsub + L"\\dbchild");
            std::string sql =
                "INSERT INTO \"keys\"(key_path,is_deleted) VALUES ('" + kc + "',0) "
                "ON CONFLICT(key_path) DO UPDATE SET is_deleted=0;"
                "INSERT INTO \"values\"(key_path,name,type,data,is_deleted) VALUES ('" + k + "','dbv',1,X'646276',0) "
                "ON CONFLICT(key_path,name) DO UPDATE SET is_deleted=0;";
            if (sqlite3_exec(rw, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) { sqlite3_close(rw); Fail("semina merge"); }
            sqlite3_close(rw);
        }
        {
            DWORD cls = 0, resv = 0, nsub = 0, mxsl = 0, mxcl = 0, nval = 0, mxvn = 0, mxd = 0, sdl = 0;
            FILETIME ft = {};
            if (RegQueryInfoKeyW(hr, NULL, &cls, &resv, &nsub, &mxsl, &mxcl, &nval, &mxvn, &mxd, &sdl, &ft) != ERROR_SUCCESS)
                FailGle("RegQueryInfoKeyW merge");
            // 0 reali + 1 solo-DB child; 1 reale + 1 solo-DB valore
            if (nsub != 1 || nval != 2 || mxvn != 4)
                Fail("QueryInfoKeyW merge: conteggi inattesi (attesa 1 child / 2 valori)");
        }
        Ok("QueryInfoKeyW merge: child/valore solo-DB uniti al reale (1/2)");

        // 4) soft-delete: child2 cancellato -> conteggio scende + handle cancellato -> NOT_FOUND
        if (RegDeleteKeyW(HKEY_CURRENT_USER, (qsub + L"\\child2").c_str()) != ERROR_SUCCESS)
            FailGle("DeleteKeyW child2");
        {
            DWORD cls = 0, resv = 0, nsub = 0, mxsl = 0, mxcl = 0, nval = 0, mxvn = 0, mxd = 0, sdl = 0;
            FILETIME ft = {};
            if (RegQueryInfoKeyW(hq2, NULL, &cls, &resv, &nsub, &mxsl, &mxcl, &nval, &mxvn, &mxd, &sdl, &ft) != ERROR_SUCCESS)
                FailGle("RegQueryInfoKeyW post-delete");
            if (nsub != 1) Fail("QueryInfoKeyW post-delete: child2 non oscurato (attesa 1)");
        }
        {
            DWORD cls = 0, resv = 0, nsub = 0, mxsl = 0, mxcl = 0, nval = 0, mxvn = 0, mxd = 0, sdl = 0;
            FILETIME ft = {};
            if (RegQueryInfoKeyW(hc2, NULL, &cls, &resv, &nsub, &mxsl, &mxcl, &nval, &mxvn, &mxd, &sdl, &ft) != ERROR_FILE_NOT_FOUND)
                Fail("QueryInfoKeyW su chiave cancellata: attesa NOT_FOUND");
        }
        Ok("QueryInfoKeyW: soft-delete oscura (child 2->1); chiave cancellata -> NOT_FOUND");

        // 5) close (67/50): reale -> Real_RegCloseKey del record + rimozione
        RegCloseKey(hr);
        Ok("RegCloseKey chiave reale (record Real): nessun crash, reale chiuso via record");

        // 6) doppia close -> no-op (id gia' rimosso -> reale su handle invalido, no crash)
        LSTATUS dc = RegCloseKey(hr);
        if (dc == ERROR_ACCESS_DENIED || dc == ERROR_SUCCESS || dc == ERROR_INVALID_HANDLE)
            ;   // tutte accettabili: importante e' non crashare
        Ok("RegCloseKey doppia close: no-op (no crash)");

        // 7) close virtuale: solo rimozione associazione, mai il reale
        RegCloseKey(hc2);
        RegCloseKey(hq2);
        Ok("RegCloseKey chiavi virtuali: solo rimozione associazione (no crash)");

        // 8) close di handle NON gestito (root): chiusura reale diretta, no crash
        LSTATUS rc = RegCloseKey(HKEY_CURRENT_USER);
        if (rc != ERROR_SUCCESS && rc != ERROR_ACCESS_DENIED)
            Fail("RegCloseKey HKCU: errore inatteso (no crash e' l'importante)");
        Ok("RegCloseKey handle non gestito (root): percorso reale, no crash");

        // 9) reale intatto per le chiavi virtuali (figlio pulito)
        DWORD cc = realcheck(qsub);
        if (cc != 0)
            Fail("chiave virtuale TROVATA nel reale (figlio pulito)");
        Ok("reale non toccato (figlio pulito: chiave virtuale assente nel reale)");
    }

    // ---------------------------------------------------- 10) parent fittizio (68)
    // Create/open con parent = id fittizio del proxy (non root). Prima di 68
    // ResolveKeyPath non risolveva l'id fittizio e il detour passava al REALE
    // con l'id fittizio (gle=2). Ora: resolve via adattatore, scrittura nel DB.
    std::wprintf(L"\n==== 10) create/open parent fittizio (issue 68) ====\n");
    {
        std::wstring fp = L"WinRegProxy" + probe + L"_fakepar";
        HKEY hf = NULL, hc = NULL, h2 = NULL, h3 = NULL;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, fp.c_str(), 0, NULL, 0,
            KEY_READ | KEY_WRITE, NULL, &hf, NULL) != ERROR_SUCCESS || !hf)
            FailGle("RegCreateKeyExW parent (root)");
        // create con parent fittizio: la child va nel DB, mai nel reale
        if (RegCreateKeyExW(hf, L"child", 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &hc, NULL)
            != ERROR_SUCCESS || !hc)
            FailGle("RegCreateKeyExW child (parent fittizio)");
        DWORD dv = 7;
        if (RegSetValueExW(hc, L"fv", 0, REG_DWORD, (const BYTE*)&dv, sizeof(dv)) != ERROR_SUCCESS)
            FailGle("RegSetValueExW su child (parent fittizio)");
        RegCloseKey(hc);
        RegCloseKey(hf);
        // open con parent fittizio + readback del valore
        if (RegOpenKeyExW(HKEY_CURRENT_USER, fp.c_str(), 0, KEY_READ, &h2) != ERROR_SUCCESS || !h2)
            FailGle("RegOpenKeyExW parent (root, riapertura)");
        if (RegOpenKeyExW(h2, L"child", 0, KEY_READ, &h3) != ERROR_SUCCESS || !h3)
            FailGle("RegOpenKeyExW child (parent fittizio)");
        DWORD rd = 0, t = 0, sz = sizeof(rd);
        if (RegQueryValueExW(h3, L"fv", NULL, &t, (BYTE*)&rd, &sz) != ERROR_SUCCESS || rd != 7)
            Fail("RegQueryValueExW readback su child (parent fittizio)");
        RegCloseKey(h3);
        RegCloseKey(h2);
        Ok("create/open/setvalue/queryvalue con parent fittizio: chiavi nel DB");
        // reale non toccato: child assente nel registro reale (figlio pulito)
        {
            wchar_t exePath[MAX_PATH] = {0};
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --realcheck " + fp + L"\\child";
            wchar_t cmdBuf[2048];
            wcsncpy_s(cmdBuf, _countof(cmdBuf), cmdLine.c_str(), _TRUNCATE);
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pinfo = {};
            if (!CreateProcessW(NULL, cmdBuf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pinfo))
                FailGle("CreateProcess (figlio pulito)");
            WaitForSingleObject(pinfo.hProcess, 10000);
            DWORD code = 0xFFFF;
            GetExitCodeProcess(pinfo.hProcess, &code);
            CloseHandle(pinfo.hProcess);
            CloseHandle(pinfo.hThread);
            if (code == 1) Fail("child TROVATA nel REALE (create parent fittizio non virtualizzato)");
            if (code != 0) Fail("figlio pulito: exit code inattesa");
        }
        Ok("reale non toccato (child assente nel reale: parent fittizio)");
    }

    // ---------------------------------------------------- 11) e2e (70)
    // Scenario completo su una chiave REALE: base reale creata da un figlio
    // pulito (senza DLL), scritture virtuali via detour (DB), letture unite
    // (reale + DB), reale verificato intatto, cancellazione + rianimazione,
    // pulizie finali (reale + DB) per ri-eseguibilita'.
    std::wprintf(L"\n==== 11) e2e: reale + virtuale insieme (issue 70) ====\n");
    {
        std::wstring esub = L"Software\\" + probe + L"_e2e";
        std::string eu = w2u(esub);
        // 11.1 setup: chiave reale con valore reale (figlio pulito, senza DLL)
        {
            wchar_t exePath[MAX_PATH] = {0};
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --realmake " + esub;
            wchar_t cmdBuf[2048];
            wcsncpy_s(cmdBuf, _countof(cmdBuf), cmdLine.c_str(), _TRUNCATE);
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pinfo = {};
            if (!CreateProcessW(NULL, cmdBuf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pinfo))
                FailGle("CreateProcess (figlio pulito realmake)");
            WaitForSingleObject(pinfo.hProcess, 10000);
            DWORD code = 0xFFFF;
            GetExitCodeProcess(pinfo.hProcess, &code);
            CloseHandle(pinfo.hProcess);
            CloseHandle(pinfo.hThread);
            if (code != 0) Fail("--realmake chiave di base (exit != 0)");
        }
        HKEY hk = NULL, hchild = NULL;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, esub.c_str(), 0, KEY_READ | KEY_WRITE, &hk) != ERROR_SUCCESS || !hk)
            FailGle("open chiave reale (record reale)");
        // 11.2 scritture virtuali: valore sulla chiave reale + sottochiave (DB)
        wchar_t v[] = L"V";
        if (RegSetValueExW(hk, L"vv", 0, REG_SZ, (const BYTE*)v, sizeof(v)) != ERROR_SUCCESS)
            FailGle("SetValue valore virtuale su chiave reale");
        if (RegCreateKeyExW(hk, L"vchild", 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &hchild, NULL) != ERROR_SUCCESS || !hchild)
            FailGle("CreateKey sottochiave virtuale (parent reale)");
        // 11.3 letture unite: reale (mark) + virtuale (vv) + enum + info
        wchar_t rd[128] = {0}; DWORD sz = sizeof(rd), t = 0;
        if (RegQueryValueExW(hk, L"mark", NULL, &t, (LPBYTE)rd, &sz) != ERROR_SUCCESS || t != REG_SZ || _wcsicmp(rd, L"m") != 0)
            Fail("QueryValue reale (mark): passthrough reale fallito");
        rd[0] = 0; sz = sizeof(rd);
        if (RegQueryValueExW(hk, L"vv", NULL, &t, (LPBYTE)rd, &sz) != ERROR_SUCCESS || t != REG_SZ || _wcsicmp(rd, L"V") != 0)
            Fail("QueryValue virtuale (vv): DB fallito");
        {   // EnumKey: solo vchild (virtuale)
            wchar_t kn[128]; DWORD ksz = sizeof(kn);
            int n = 0;
            if (RegEnumKeyExW(hk, 0, kn, &ksz, NULL, NULL, NULL, NULL) != ERROR_SUCCESS || _wcsicmp(kn, L"vchild") != 0)
                Fail("EnumKey non mostra la sottochiave virtuale");
            n++;
            if (RegEnumKeyExW(hk, 1, kn, &ksz, NULL, NULL, NULL, NULL) != ERROR_NO_MORE_ITEMS)
                Fail("EnumKey: sottochiave inattesa in piu'");
            // EnumValue: 2 valori (mark reale + vv virtuale)
            wchar_t vn[64];
            int nv = 0;
            while (nv < 8)
            {
                DWORD vsz = (DWORD)sizeof(vn);
                if (RegEnumValueW(hk, (DWORD)nv, vn, &vsz, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                    break;
                nv++;
            }
            if (nv != 2) Fail("EnumValue: conteggio non unito (atteso 2)");
            // QueryInfo: nsub=1, nval=2 (unione)
            FILETIME ft = {};
            DWORD cls = 0, resv = 0, nsub = 0, mxsl = 0, mxcl = 0, nval = 0, mxvn = 0, mxd = 0, sdl = 0;
            if (RegQueryInfoKeyW(hk, NULL, &cls, &resv, &nsub, &mxsl, &mxcl, &nval, &mxvn, &mxd, &sdl, &ft) != ERROR_SUCCESS)
                FailGle("QueryInfoKeyW chiave reale");
            if (nsub != 1 || nval != 2) Fail("QueryInfo unione (atteso nsub=1, nval=2)");
        }
        // 11.4 reale intatto (figlio pulito): mark invariato, vv assente nel reale
        auto realval = [&](const std::wstring& sub, const wchar_t* name, const wchar_t* exp) -> DWORD
        {
            wchar_t exePath[MAX_PATH] = {0};
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --realval " + sub + L" " + name + L" " + exp;
            wchar_t cmdBuf[2048];
            wcsncpy_s(cmdBuf, _countof(cmdBuf), cmdLine.c_str(), _TRUNCATE);
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pinfo = {};
            if (!CreateProcessW(NULL, cmdBuf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pinfo))
                FailGle("CreateProcess (figlio pulito realval)");
            WaitForSingleObject(pinfo.hProcess, 10000);
            DWORD code = 0xFFFF;
            GetExitCodeProcess(pinfo.hProcess, &code);
            CloseHandle(pinfo.hProcess);
            CloseHandle(pinfo.hThread);
            return code;
        };
        if (realval(esub, L"mark", L"m") != 0) Fail("reale: 'mark' modificato/assente (scrittura virtuale ha toccato il reale)");
        if (realval(esub, L"vv", L"qualcosa") != 1) Fail("reale: 'vv' presente (il valore virtuale e' nel REALE)");
        Ok("e2e: scritture nel DB, letture unite, reale intatto");
        // 11.5 cancellazione + rianimazione
        if (RegDeleteValueW(hk, L"vv") != ERROR_SUCCESS)
            FailGle("DeleteValue(vv) (DB)");
        if (RegQueryValueExW(hk, L"vv", NULL, &t, (LPBYTE)rd, &sz) != ERROR_FILE_NOT_FOUND)
            Fail("vv ancora visibile dopo cancellazione virtuale");
        if (RegQueryValueExW(hk, L"mark", NULL, &t, (LPBYTE)rd, &sz) != ERROR_SUCCESS)
            Fail("mark perso dopo cancellazione di vv");
        wchar_t v2[] = L"2";
        if (RegSetValueExW(hk, L"vv", 0, REG_SZ, (const BYTE*)v2, sizeof(v2)) != ERROR_SUCCESS)
            FailGle("re-SetValue(vv) (rianimazione)");
        rd[0] = 0; sz = sizeof(rd);
        if (RegQueryValueExW(hk, L"vv", NULL, &t, (LPBYTE)rd, &sz) != ERROR_SUCCESS || _wcsicmp(rd, L"2") != 0)
            Fail("vv non ri-animata dopo re-set");
        Ok("e2e: cancellazione virtuale + rianimazione");
        // 11.6 pulizie: DB (vchild, vv) + reale (chiave con mark)
        RegCloseKey(hchild);
        if (RegDeleteKey(HKEY_CURRENT_USER, (esub + L"\\vchild").c_str()) != ERROR_SUCCESS)
            FailGle("DeleteKey(vchild) (DB)");
        RegCloseKey(hk);
        // La chiave reale ha una shadow nel DB (SetValue crea la riga chiave):
        // RegDeleteKey qui sarebbe virtuale. Pulizia reale via figlio pulito.
        {
            wchar_t exePath[MAX_PATH] = {0};
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --realdel " + esub;
            wchar_t cmdBuf[2048];
            wcsncpy_s(cmdBuf, _countof(cmdBuf), cmdLine.c_str(), _TRUNCATE);
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pinfo = {};
            if (!CreateProcessW(NULL, cmdBuf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pinfo))
                FailGle("CreateProcess (figlio pulito realdel)");
            WaitForSingleObject(pinfo.hProcess, 10000);
            DWORD code = 0xFFFF;
            GetExitCodeProcess(pinfo.hProcess, &code);
            CloseHandle(pinfo.hProcess);
            CloseHandle(pinfo.hThread);
            if (code != 0) Fail("--realdel: pulizia chiave reale fallita");
        }
        {
            wchar_t exePath[MAX_PATH] = {0};
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --realcheck " + esub;
            wchar_t cmdBuf[2048];
            wcsncpy_s(cmdBuf, _countof(cmdBuf), cmdLine.c_str(), _TRUNCATE);
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pinfo = {};
            if (!CreateProcessW(NULL, cmdBuf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pinfo))
                FailGle("CreateProcess (figlio pulito realcheck finale)");
            WaitForSingleObject(pinfo.hProcess, 10000);
            DWORD code = 0xFFFF;
            GetExitCodeProcess(pinfo.hProcess, &code);
            CloseHandle(pinfo.hProcess);
            CloseHandle(pinfo.hThread);
            if (code == 1) Fail("pulizia: chiave reale ancora presente");
            if (code != 0) Fail("figlio pulito finale: exit inattesa");
        }
        Ok("e2e: pulizie completate (reale + DB), test ri-eseguibile");
    }
    // ---------------------------------------------------------- summary
    std::wprintf(L"\n=== VirtTest: ambiente PRONTO (%d check ok) ===\n", s_pass);
    return 0;
}
