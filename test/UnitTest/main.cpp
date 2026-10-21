// main.cpp - ambiente di test di unita' di WinRegProxy (fase 2)
//
// Compila DIRETTAMENTE le classi (SQLLite_REG, REG_handler_mng) + sqlite3,
// NON carica la DLL. Ogni issue aggiunge una sezione qui (fail fast: stop al
// primo errore, un test per funzione).
//
// Sezioni:
//   1. sqlite  - sqlite3 e' usabile: open su DB temporaneo, CREATE + insert +
//                select + close, poi rimozione del file (issue 10)
//   2. sqlreg-init - SQLLite_REG: ctor crea file+schema, dtor chiude, idempotenza
//                (riapertura), tabelle 'keys'/'values' presenti (issue 20)
//   3. existkey    - ExistKey: inesistente=no; presente=sì; soft-deleted=no;
//                figlia di chiave cancellata=no (issue 21)
//   4. createkey   - CreateKey: crea+esiste, idempotenza (niente righe doppie),
//                rianimazione dopo soft-delete (issue 22)
//   5. queryvalue  - QueryValue: assente=no; round-trip type+data; soft-delete=no;
//                chiave-cancellata=no (issue 23)
//   6. setvalue    - SetValue: nuovo (round-trip + chiave implicita); sovrascrivi
//                (1 riga); rianimazione dopo soft-delete (issue 24)
//   7. deletevalue - DeleteValue: soft-delete (QueryValue=no), riga ancora nel DB
//                con is_deleted=1, doppio/inesistente delete = no-op (issue 25)
//   8. deletekey   - DeleteKey: sottoalbero non visibile, righe chiavi is_deleted=1
//                (valori non marcati), rianimazione di A senza A\B (issue 26)
//   9. enumkey     - EnumKey: sottochiavi immediate (non i discendenti profondi),
//                vuoto, soft-delete esclusa, ordine lessicografico (issue 27)
//  10. enumvalue   - EnumValue: x+y (non il default), soft-delete escluso,
//                chiave-cancellata vuoto, ordine per nome (issue 28)
//  11. queryinfo   - QueryInfo: conteggi subkeys/valori coerenti, decrementi
//                dopo delete, chiave assente -> false (issue 29)
//  12. hmng-real   - REG_handler_mng: registerRealHandler -> id fittizio unico,
//                getHandlerType == Real, id sconosciuto == Unknown (issue 30)
//  13. hmng-virtual- registerVirtualHandler -> id fittizio, tipo Virtual,
//                accessor getHandler recupera keyPath/HKEY (issue 31)
//  14. hmng-remove - removeHandler: rimuove reale/virtuale, doppio/inesistente
//                = no-op false (issue 32)
//  15. hmng-gettype- getHandlerType: Real(+HKEY originale), Virtual(+percorso),
//                sconosciuto (issue 33; funzione già implementata in 30)
//  16. keystate    - KeyState: assente=0, virtuale viva=1, soft-deletata=-1,
//                sotto-cancellata=-1, DB non aperto=0 (prereq issue 41)
//  17. valuestate  - ValueState: presente=1, assente=0, cancellato=-1,
//                chiave-cancellata=-1, DB non aperto=0 (prereq issue 43)
//
// Nota: char-based (sqlite3 e' char/UTF-8); niente UNICODE wrapper qui.

#include <windows.h>
#include <stdio.h>
#include <string>
#include "sqlite3.h"
#include "SQLLite_REG.h"
#include "REG_handler_mng.h"

// ======================================================================= stato
static int s_pass = 0;

static void Ok(const char* section, const char* msg)
{
    ++s_pass;
    printf("[ OK ] %s: %s\n", section, msg);
    fflush(stdout);
}

static void Fail(const char* section, const char* msg)
{
    printf("[FAIL] %s: %s\n", section, msg);
    printf("\nRiassunto: %d superati, 1 fallito (stop al primo errore)\n", s_pass);
    fflush(stdout);
    exit(1);
}

// ======================================================================= 1. sqlite
static void TestSqlite()
{
    // DB temporaneo in %TEMP% (per la convenzione sul path del DB reale,
    // vedi issue 10/40: accanto alla DLL, ripiego %TEMP%)
    char tmp[MAX_PATH] = "";
    GetTempPathA(MAX_PATH, tmp);
    std::string db = std::string(tmp) + "winregproxy_smoke.db";
    DeleteFileA(db.c_str());   // pulisce run precedenti

    sqlite3* dbh = nullptr;
    int rc = sqlite3_open(db.c_str(), &dbh);
    if (rc != SQLITE_OK)
        Fail("sqlite", "sqlite3_open fallita");

    char* err = nullptr;
    rc = sqlite3_exec(dbh,
        "CREATE TABLE IF NOT EXISTS t (id INTEGER PRIMARY KEY, v TEXT); "
        "INSERT INTO t (v) VALUES ('hello');",
        nullptr, nullptr, &err);
    if (rc != SQLITE_OK)
    {
        std::string e = err ? err : "?";
        if (err) sqlite3_free(err);
        Fail("sqlite", e.c_str());
    }

    sqlite3_stmt* st = nullptr;
    rc = sqlite3_prepare_v2(dbh, "SELECT v FROM t WHERE id=1", -1, &st, nullptr);
    if (rc != SQLITE_OK)
        Fail("sqlite", "sqlite3_prepare fallito");
    if (sqlite3_step(st) != SQLITE_ROW)
        Fail("sqlite", "step: nessun riga");
    const unsigned char* v = sqlite3_column_text(st, 0);
    if (!v || std::string((const char*)v) != "hello")
        Fail("sqlite", "select: valore non coerente");
    sqlite3_finalize(st);

    rc = sqlite3_close(dbh);
    if (rc != SQLITE_OK)
        Fail("sqlite", "sqlite3_close fallita");

    DeleteFileA(db.c_str());
    Ok("sqlite", "open/create/insert/select/close su DB temporaneo ok");
}

// ======================================================================= 2. sqlreg-init (issue 20)
static void TestSqlregInit()
{
    char tmp[MAX_PATH] = "";
    GetTempPathA(MAX_PATH, tmp);
    std::string db = std::string(tmp) + "winregproxy_sqlreg.db";
    DeleteFileA(db.c_str());

    {
        SQLLite_REG reg(db);   // costruttore: open + schema
        if (GetFileAttributesA(db.c_str()) == INVALID_FILE_ATTRIBUTES)
            Fail("sqlreg-init", "costruttore non ha creato il file .db");
    }  // distruttore: closeDB

    // idempotenza: riapertura sul file esistente non fallisce
    {
        SQLLite_REG reg(db);
        if (GetFileAttributesA(db.c_str()) == INVALID_FILE_ATTRIBUTES)
            Fail("sqlreg-init", "riapertura: file .db sparito");
    }

    // black-box: le tabelle sono state create (controllate su sqlite_master)
    sqlite3* h = nullptr;
    if (sqlite3_open(db.c_str(), &h) != SQLITE_OK)
        Fail("sqlreg-init", "verifica: non riesco a riaprire il db");
    auto table_exists = [&](const char* t) -> bool {
        sqlite3_stmt* st = nullptr;
        bool found = false;
        if (sqlite3_prepare_v2(h, "SELECT name FROM sqlite_master WHERE type='table' AND name=?",
                -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(st, 1, t, -1, SQLITE_STATIC);
            found = (sqlite3_step(st) == SQLITE_ROW);
            sqlite3_finalize(st);
        }
        return found;
    };
    if (!table_exists("keys"))   Fail("sqlreg-init", "tabella 'keys' mancante");
    if (!table_exists("values")) Fail("sqlreg-init", "tabella 'values' mancante");
    sqlite3_close(h);

    DeleteFileA(db.c_str());
    Ok("sqlreg-init", "ctor/dtor + schema (keys, values) + idempotenza ok");
}

// ======================================================================= 3. existkey (issue 21)
static void TestSqlregExistKey()
{
    char tmp[MAX_PATH] = "";
    GetTempPathA(MAX_PATH, tmp);
    std::string db = std::string(tmp) + "winregproxy_exist.db";
    DeleteFileA(db.c_str());

    SQLLite_REG reg(db);

    // connessione di servizio (stesso file) per impostare lo stato a mano
    sqlite3* h = nullptr;
    if (sqlite3_open(db.c_str(), &h) != SQLITE_OK)
        Fail("existkey", "non riesco ad aprire il db di servizio");
    auto exec = [&](const char* s) { sqlite3_exec(h, s, nullptr, nullptr, nullptr); };

    // 1) inesistente -> no
    if (reg.ExistKey("HKCU\\NonEsiste\\X"))
        Fail("existkey", "chiave inesistente riportata esistente");

    // 2) presente (insert diretto) -> sì
    exec("INSERT INTO keys(key_path) VALUES('HKCU\\A\\B')");
    if (!reg.ExistKey("HKCU\\A\\B"))
        Fail("existkey", "chiave presente riportata inesistente");

    // 3) soft-deletata -> no
    exec("UPDATE keys SET is_deleted=1 WHERE key_path='HKCU\\A\\B'");
    if (reg.ExistKey("HKCU\\A\\B"))
        Fail("existkey", "chiave soft-deletata riportata esistente");
    exec("UPDATE keys SET is_deleted=0 WHERE key_path='HKCU\\A\\B'");

    // 4) genitore cancellato -> la figlia 'non esiste' (senza toccare la riga figlia)
    exec("INSERT INTO keys(key_path) VALUES('HKCU\\A')");
    exec("UPDATE keys SET is_deleted=1 WHERE key_path='HKCU\\A'");
    if (reg.ExistKey("HKCU\\A\\B"))
        Fail("existkey", "figlia di chiave cancellata riportata esistente");

    sqlite3_close(h);
    DeleteFileA(db.c_str());
    Ok("existkey", "presente / soft-delete / genitore-cancellato gestiti");
}

// ======================================================================= 4. createkey (issue 22)
static void TestSqlregCreateKey()
{
    char tmp[MAX_PATH] = "";
    GetTempPathA(MAX_PATH, tmp);
    std::string db = std::string(tmp) + "winregproxy_createkey.db";
    DeleteFileA(db.c_str());
    SQLLite_REG reg(db);

    // 1) crea -> esiste
    if (!reg.CreateKey("HKCU\\X")) Fail("createkey", "CreateKey fallito");
    if (!reg.ExistKey("HKCU\\X")) Fail("createkey", "chiave creata non visibile");

    // 2) doppia creazione: una sola riga
    if (!reg.CreateKey("HKCU\\X")) Fail("createkey", "re-CreateKey fallito");
    {
        sqlite3* h = nullptr; sqlite3_open(db.c_str(), &h);
        sqlite3_stmt* st = nullptr; int n = 0;
        if (sqlite3_prepare_v2(h, "SELECT count(*) FROM keys WHERE key_path='HKCU\\X'",
                -1, &st, nullptr) == SQLITE_OK)
        { sqlite3_step(st); n = sqlite3_column_int(st, 0); sqlite3_finalize(st); }
        sqlite3_close(h);
        if (n != 1) Fail("createkey", "numero righe chiave != 1 (doppie?)");
    }

    // 3) rianimazione (DeleteKey/26 non pronta: simulo la soft-delete a mano)
    {
        sqlite3* h = nullptr; sqlite3_open(db.c_str(), &h);
        sqlite3_exec(h, "UPDATE keys SET is_deleted=1 WHERE key_path='HKCU\\X'",
            nullptr, nullptr, nullptr);
        sqlite3_close(h);
    }
    if (reg.ExistKey("HKCU\\X")) Fail("createkey", "chiave soft-deleted ancora visibile");
    if (!reg.CreateKey("HKCU\\X")) Fail("createkey", "re-CreateKey (rianimazione) fallito");
    if (!reg.ExistKey("HKCU\\X")) Fail("createkey", "chiave non rianimata");

    DeleteFileA(db.c_str());
    Ok("createkey", "crea / idempotenza / rianimazione ok");
}

// ======================================================================= 5. queryvalue (issue 23)
static void TestSqlregQueryValue()
{
    char tmp[MAX_PATH] = "";
    GetTempPathA(MAX_PATH, tmp);
    std::string db = std::string(tmp) + "winregproxy_qv.db";
    DeleteFileA(db.c_str());
    SQLLite_REG reg(db);
    reg.CreateKey("HKCU\\Q");

    // 1) assente
    { int t; std::string d; if (reg.QueryValue("HKCU\\Q", "nope", &t, &d))
        Fail("queryvalue", "valore assente trovato"); }

    // 2) round-trip (popolato con insert SQL: SetValue/24 non ancora pronto)
    {
        sqlite3* h = nullptr; sqlite3_open(db.c_str(), &h);
        // x'414243' = "ABC" (3 byte)
        sqlite3_exec(h,
            "INSERT INTO \"values\"(key_path,name,type,data) "
            "VALUES('HKCU\\Q','myname',1,x'414243')",
            nullptr, nullptr, nullptr);
        sqlite3_close(h);
    }
    { int t = -1; std::string d;
      if (!reg.QueryValue("HKCU\\Q", "myname", &t, &d))
          Fail("queryvalue", "valore presente non trovato");
      if (t != 1)      Fail("queryvalue", "type errato");
      if (d != "ABC")  Fail("queryvalue", "data errata"); }

    // 3) valore soft-deletato -> non presente
    {
        sqlite3* h = nullptr; sqlite3_open(db.c_str(), &h);
        sqlite3_exec(h, "UPDATE \"values\" SET is_deleted=1 WHERE key_path='HKCU\\Q' AND name='myname'",
            nullptr, nullptr, nullptr);
        sqlite3_close(h);
    }
    { int t; std::string d; if (reg.QueryValue("HKCU\\Q", "myname", &t, &d))
        Fail("queryvalue", "valore cancellato ancora letto"); }

    // 4) chiave cancellata -> valore non presente (riga value 'alive')
    {
        sqlite3* h = nullptr; sqlite3_open(db.c_str(), &h);
        sqlite3_exec(h, "UPDATE \"values\" SET is_deleted=0 WHERE key_path='HKCU\\Q' AND name='myname'",
            nullptr, nullptr, nullptr);
        sqlite3_exec(h, "UPDATE keys SET is_deleted=1 WHERE key_path='HKCU\\Q'",
            nullptr, nullptr, nullptr);
        sqlite3_close(h);
    }
    { int t; std::string d; if (reg.QueryValue("HKCU\\Q", "myname", &t, &d))
        Fail("queryvalue", "valore sotto chiave cancellata ancora letto"); }

    DeleteFileA(db.c_str());
    Ok("queryvalue", "assente / round-trip / soft-delete / chiave-cancellata ok");
}

// ======================================================================= 6. setvalue (issue 24)
static void TestSqlregSetValue()
{
    char tmp[MAX_PATH] = "";
    GetTempPathA(MAX_PATH, tmp);
    std::string db = std::string(tmp) + "winregproxy_sv.db";
    DeleteFileA(db.c_str());
    SQLLite_REG reg(db);

    // 1) nuovo -> round-trip + chiave implicita
    if (!reg.SetValue("HKCU\\S", "k", 1, "hello")) Fail("setvalue", "SetValue fallito");
    if (!reg.ExistKey("HKCU\\S")) Fail("setvalue", "chiave implicita non creata");
    { int t = -1; std::string d;
      if (!reg.QueryValue("HKCU\\S", "k", &t, &d)) Fail("setvalue", "non letto");
      if (t != 1 || d != "hello") Fail("setvalue", "round-trip errato"); }

    // 2) sovrascritto, una sola riga
    if (!reg.SetValue("HKCU\\S", "k", 1, "world")) Fail("setvalue", "2o SetValue fallito");
    { int t; std::string d; reg.QueryValue("HKCU\\S", "k", &t, &d);
      if (d != "world") Fail("setvalue", "non sovrascritto"); }
    {
        sqlite3* h = nullptr; sqlite3_open(db.c_str(), &h);
        sqlite3_stmt* st = nullptr; int n = 0;
        if (sqlite3_prepare_v2(h,
                "SELECT count(*) FROM \"values\" WHERE key_path='HKCU\\S' AND name='k'",
                -1, &st, nullptr) == SQLITE_OK)
        { sqlite3_step(st); n = sqlite3_column_int(st, 0); sqlite3_finalize(st); }
        sqlite3_close(h);
        if (n != 1) Fail("setvalue", "righe value !=1 (doppie?)");
    }

    // 3) rianimazione (DeleteValue/25 non pronto: simulo la soft-delete)
    {
        sqlite3* h = nullptr; sqlite3_open(db.c_str(), &h);
        sqlite3_exec(h, "UPDATE \"values\" SET is_deleted=1 WHERE key_path='HKCU\\S' AND name='k'",
            nullptr, nullptr, nullptr);
        sqlite3_close(h);
    }
    { int t; std::string d; if (reg.QueryValue("HKCU\\S", "k", &t, &d))
        Fail("setvalue", "valore cancellato ancora letto"); }
    if (!reg.SetValue("HKCU\\S", "k", 1, "again")) Fail("setvalue", "re-SetValue (rianimazione) fallito");
    { int t; std::string d;
      if (!reg.QueryValue("HKCU\\S", "k", &t, &d)) Fail("setvalue", "valore non rianimato");
      if (d != "again") Fail("setvalue", "valore rianimato errato"); }

    DeleteFileA(db.c_str());
    Ok("setvalue", "nuovo / sovrascrivi / rianimazione ok");
}

// ======================================================================= 7. deletevalue (issue 25)
static void TestSqlregDeleteValue()
{
    char tmp[MAX_PATH] = "";
    GetTempPathA(MAX_PATH, tmp);
    std::string db = std::string(tmp) + "winregproxy_dv.db";
    DeleteFileA(db.c_str());
    SQLLite_REG reg(db);
    reg.SetValue("HKCU\\D", "v", 1, "val");

    // 1) soft delete -> non leggibile
    if (!reg.DeleteValue("HKCU\\D", "v")) Fail("deletevalue", "DeleteValue fallito");
    { int t; std::string d; if (reg.QueryValue("HKCU\\D", "v", &t, &d))
        Fail("deletevalue", "valore cancellato ancora letto"); }

    // 2) la riga esiste ancora con is_deleted=1
    {
        sqlite3* h = nullptr; sqlite3_open(db.c_str(), &h);
        sqlite3_stmt* st = nullptr; int cnt = 0, del = 0;
        if (sqlite3_prepare_v2(h,
                "SELECT count(*), sum(is_deleted) FROM \"values\" "
                "WHERE key_path='HKCU\\D' AND name='v'",
                -1, &st, nullptr) == SQLITE_OK)
        { sqlite3_step(st); cnt = sqlite3_column_int(st, 0); del = sqlite3_column_int(st, 1); sqlite3_finalize(st); }
        sqlite3_close(h);
        if (cnt != 1)  Fail("deletevalue", "riga mancante (DELETE invece di soft-delete?)");
        if (del != 1)  Fail("deletevalue", "is_deleted != 1");
    }

    // 3) doppio delete (già cancellato) -> false (no-op)
    if (reg.DeleteValue("HKCU\\D", "v")) Fail("deletevalue", "2o delete su già cancellato -> deve dare false");

    // 4) delete su valore inesistente -> false
    if (reg.DeleteValue("HKCU\\D", "nessuno")) Fail("deletevalue", "delete su inesistente -> deve dare false");

    DeleteFileA(db.c_str());
    Ok("deletevalue", "soft-delete / riga presente / no-op ok");
}

// ======================================================================= 8. deletekey (issue 26)
static void TestSqlregDeleteKey()
{
    char tmp[MAX_PATH] = "";
    GetTempPathA(MAX_PATH, tmp);
    std::string db = std::string(tmp) + "winregproxy_dk.db";
    DeleteFileA(db.c_str());
    SQLLite_REG reg(db);
    reg.CreateKey("HKCU\\A\\B");          // crea HKCU\A e HKCU\A\B
    reg.SetValue("HKCU\\A", "va", 1, "1");
    reg.SetValue("HKCU\\A\\B", "vb", 1, "2");

    // 1) DeleteKey(A): sottoalbero non più visibile
    if (!reg.DeleteKey("HKCU\\A"))        Fail("deletekey", "DeleteKey fallito");
    if (reg.ExistKey("HKCU\\A"))          Fail("deletekey", "A ancora esistente");
    if (reg.ExistKey("HKCU\\A\\B"))       Fail("deletekey", "A\\B ancora esistente");
    { int t; std::string d; if (reg.QueryValue("HKCU\\A", "va", &t, &d))
        Fail("deletekey", "valore su A ancora letto"); }
    { int t; std::string d; if (reg.QueryValue("HKCU\\A\\B", "vb", &t, &d))
        Fail("deletekey", "valore su A\\B ancora letto"); }

    // 2) le righe chiave restano con is_deleted=1 (i valori NON marcati)
    {
        sqlite3* h = nullptr; sqlite3_open(db.c_str(), &h);
        auto getInt = [&](const char* sql) -> int {
            sqlite3_stmt* st = nullptr; int v = -1;
            if (sqlite3_prepare_v2(h, sql, -1, &st, nullptr) == SQLITE_OK &&
                sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
            if (st) sqlite3_finalize(st);
            return v;
        };
        int kA = getInt("SELECT is_deleted FROM keys WHERE key_path='HKCU\\A'");
        int kB = getInt("SELECT is_deleted FROM keys WHERE key_path='HKCU\\A\\B'");
        int vA = getInt("SELECT is_deleted FROM \"values\" WHERE key_path='HKCU\\A' AND name='va'");
        sqlite3_close(h);
        if (kA != 1) Fail("deletekey", "chiave A non marcata is_deleted=1");
        if (kB != 1) Fail("deletekey", "chiave A\\B non marcata is_deleted=1");
        if (vA != 0) Fail("deletekey", "valore marcato (non dovuto: restano is_deleted=0)");
    }

    // 3) rianimazione di A: A torna, A\B resta assente (semantica registro reale)
    if (!reg.CreateKey("HKCU\\A"))      Fail("deletekey", "re-CreateKey(A) fallito");
    if (!reg.ExistKey("HKCU\\A"))       Fail("deletekey", "A non rianimata");
    if (reg.ExistKey("HKCU\\A\\B"))     Fail("deletekey", "A\\B rianimata (non dovuto)");

    DeleteFileA(db.c_str());
    Ok("deletekey", "sottoalbero / valori non marcati / rianimazione ok");
}

// ======================================================================= 9. enumkey (issue 27)
static void TestSqlregEnumKey()
{
    char tmp[MAX_PATH] = "";
    GetTempPathA(MAX_PATH, tmp);
    std::string db = std::string(tmp) + "winregproxy_ek.db";
    DeleteFileA(db.c_str());
    SQLLite_REG reg(db);
    reg.CreateKey("HKCU\\A\\B1\\C");   // crea A, A\B1, A\B1\C
    reg.CreateKey("HKCU\\A\\B2");

    // 1) EnumKey(A) = B1, B2 (non C: C è sotto B1, non immediato)
    {
        std::vector<std::string> v = reg.EnumKey("HKCU\\A");
        if (v.size() != 2)             Fail("enumkey", "numero sottochiavi != 2");
        else { if (v[0] != "B1") Fail("enumkey", "v[0] != B1");
               if (v[1] != "B2") Fail("enumkey", "v[1] != B2"); }
    }

    // 2) chiave senza sottochiavi -> vuoto
    if (!reg.EnumKey("HKCU\\A\\B1\\C").empty())
        Fail("enumkey", "C non dovrebbe avere sottochiavi");

    // 3) sottochiave soft-deletata non appare
    reg.DeleteKey("HKCU\\A\\B2");
    {
        std::vector<std::string> v = reg.EnumKey("HKCU\\A");
        if (v.size() != 1 || v[0] != "B1")
            Fail("enumkey", "B2 cancellata ancora elencata");
    }

    // 4) ordine lessicografico deterministico
    reg.CreateKey("HKCU\\Z\\b");
    reg.CreateKey("HKCU\\Z\\a");
    reg.CreateKey("HKCU\\Z\\c");
    {
        std::vector<std::string> v = reg.EnumKey("HKCU\\Z");
        if (v.size() != 3 || v[0] != "a" || v[1] != "b" || v[2] != "c")
            Fail("enumkey", "ordine lessicografico errato");
    }

    DeleteFileA(db.c_str());
    Ok("enumkey", "immediate / vuoto / soft-delete / ordine ok");
}

// ===================================================================== 10. enumvalue (issue 28)
static void TestSqlregEnumValue()
{
    char tmp[MAX_PATH] = "";
    GetTempPathA(MAX_PATH, tmp);
    std::string db = std::string(tmp) + "winregproxy_ev.db";
    DeleteFileA(db.c_str());
    SQLLite_REG reg(db);
    reg.SetValue("HKCU\\A", "x", 1, "vx");
    reg.SetValue("HKCU\\A", "y", 2, std::string("\x01\x02", 2));  // REG_BINARY
    reg.SetValue("HKCU\\A", "", 1, "default");                    // predefinito: escludere

    // 1) EnumValue(A) = x, y (non il default)
    {
        std::vector<RegValue> v = reg.EnumValue("HKCU\\A");
        if (v.size() != 2)
            Fail("enumvalue", "numero valori != 2 (il default non deve esserci)");
        else {
            if (v[0].name != "x" || v[0].type != 1 || v[0].data != "vx")
                Fail("enumvalue", "valore x errato");
            if (v[1].name != "y" || v[1].type != 2 || v[1].data != std::string("\x01\x02", 2))
                Fail("enumvalue", "valore y errato");
        }
    }

    // 2) valore soft-deletato non appare
    reg.DeleteValue("HKCU\\A", "y");
    {
        std::vector<RegValue> v = reg.EnumValue("HKCU\\A");
        if (v.size() != 1 || v[0].name != "x")
            Fail("enumvalue", "valore cancellato ancora elencato");
    }

    // 3) chiave cancellata -> vuoto
    reg.DeleteKey("HKCU\\A");
    {
        std::vector<RegValue> v = reg.EnumValue("HKCU\\A");
        if (!v.empty()) Fail("enumvalue", "chiave cancellata: valori ancora elencati");
    }

    DeleteFileA(db.c_str());
    Ok("enumvalue", "x,y / default escluso / soft-delete / chiave-cancellata ok");
}

// ===================================================================== 11. queryinfo (issue 29)
static void TestSqlregQueryInfo()
{
    char tmp[MAX_PATH] = "";
    GetTempPathA(MAX_PATH, tmp);
    std::string db = std::string(tmp) + "winregproxy_qi.db";
    DeleteFileA(db.c_str());
    SQLLite_REG reg(db);
    reg.CreateKey("HKCU\\A\\B1");
    reg.CreateKey("HKCU\\A\\B2");
    reg.SetValue("HKCU\\A", "x", 1, "1");
    reg.SetValue("HKCU\\A", "y", 1, "2");
    reg.SetValue("HKCU\\A", "", 1, "def");

    // 1) conteggi attesi: 2 subkeys, 3 valori (x, y, default)
    {
        RegKeyInfo info;
        if (!reg.QueryInfo("HKCU\\A", &info)) Fail("queryinfo", "chiave viva -> QueryInfo=false");
        if (info.numSubkeys != 2) Fail("queryinfo", "numSubkeys != 2");
        if (info.numValues != 3)  Fail("queryinfo", "numValues != 3");
    }

    // 2) dopo DeleteValue e DeleteKey i conteggi calano
    reg.DeleteValue("HKCU\\A", "y");
    reg.DeleteKey("HKCU\\A\\B2");
    {
        RegKeyInfo info;
        if (!reg.QueryInfo("HKCU\\A", &info)) Fail("queryinfo", "chiave viva -> QueryInfo=false");
        if (info.numSubkeys != 1) Fail("queryinfo", "numSubkeys != 1 dopo delete");
        if (info.numValues != 2)  Fail("queryinfo", "numValues != 2 dopo delete");
    }

    // 3) chiave assente -> false
    {
        RegKeyInfo info;
        if (reg.QueryInfo("HKCU\\NONSENSE", &info))
            Fail("queryinfo", "chiave assente -> QueryInfo deve dare false");
    }

    DeleteFileA(db.c_str());
    Ok("queryinfo", "conteggi / decrementi / assente ok");
}

// ================================================================ 12. hmng-real (issue 30)
static void TestHandlerMngReal()
{
    REG_handler_mng* m = REG_handler_mng::getInstance();
    HKEY a = (HKEY)0x1234;
    HKEY b = (HKEY)0x5678;
    HKEY fa = m->registerRealHandler(a, "HKCU\\A");
    HKEY fb = m->registerRealHandler(b, "HKCU\\B");

    if (fa == nullptr)                       Fail("hmng-real", "id fittizio A == null");
    if (fb == nullptr)                       Fail("hmng-real", "id fittizio B == null");
    if (fa == fb)                            Fail("hmng-real", "due id fittizi identici");
    if (m->getHandlerType(fa) != HandlerType::Real) Fail("hmng-real", "tipo A != Real");
    if (m->getHandlerType(fb) != HandlerType::Real) Fail("hmng-real", "tipo B != Real");
    if (m->getHandlerType((HKEY)0xDEAD) != HandlerType::Unknown)
        Fail("hmng-real", "id sconosciuto != Unknown");

    Ok("hmng-real", "registro reale / id distinti / tipo ok");
}

// ==================================================== 13. hmng-virtual (issue 31)
static void TestHandlerMngVirtual()
{
    REG_handler_mng* m = REG_handler_mng::getInstance();
    HKEY fv  = m->registerVirtualHandler("HKCU\\Software\\X");
    HKEY fr  = m->registerRealHandler((HKEY)0xABCD, "HKCU\\Software\\X");
    HKEY fv2 = m->registerVirtualHandler("HKCU\\Software\\Y");

    if (fv == nullptr)  Fail("hmng-virtual", "id virtuale A == null");
    if (fv == fv2)      Fail("hmng-virtual", "due id virtuali identici");
    if (fv == fr)       Fail("hmng-virtual", "id virtuale == id reale");
    if (m->getHandlerType(fv) != HandlerType::Virtual) Fail("hmng-virtual", "tipo != Virtual");

    // accessor: percorso recuperabile
    HandlerRecord rec;
    if (!m->getHandler(fv, &rec))                Fail("hmng-virtual", "getHandler fallito");
    if (rec.type != HandlerType::Virtual)        Fail("hmng-virtual", "rec.type != Virtual");
    if (rec.keyPath != "HKCU\\Software\\X")     Fail("hmng-virtual", "keyPath errato");

    // accessor per reale: HKEY recuperabile
    if (!m->getHandler(fr, &rec))                Fail("hmng-virtual", "getHandler(reale) fallito");
    if (rec.type != HandlerType::Real)           Fail("hmng-virtual", "reale: tipo != Real");
    if (rec.real != (HKEY)0xABCD)                Fail("hmng-virtual", "reale: HKEY errato");

    // accessor per id sconosciuto -> false
    if (m->getHandler((HKEY)0x1, &rec))          Fail("hmng-virtual", "getHandler(sconosciuto) == true");

    Ok("hmng-virtual", "registrazione virtuale / accessor path / accessor reale ok");
}

// ======================================================= 14. hmng-remove (issue 32)
static void TestHandlerMngRemove()
{
    REG_handler_mng* m = REG_handler_mng::getInstance();
    HKEY fr = m->registerRealHandler((HKEY)0x1111, "HKCU\\R");
    HKEY fv = m->registerVirtualHandler("HKCU\\R");

    // remove reale -> rimosso, doppio remove -> false
    if (!m->removeHandler(fr))                    Fail("hmng-remove", "remove(reale) fallito");
    if (m->getHandlerType(fr) != HandlerType::Unknown) Fail("hmng-remove", "reale ancora presente");
    if (m->removeHandler(fr))                     Fail("hmng-remove", "doppia remove == true");

    // remove virtuale
    if (!m->removeHandler(fv))                    Fail("hmng-remove", "remove(virtuale) fallito");
    if (m->getHandlerType(fv) != HandlerType::Unknown) Fail("hmng-remove", "virtuale ancora presente");

    // remove inesistente -> false (no-op)
    if (m->removeHandler((HKEY)0x9999))          Fail("hmng-remove", "remove inesistente == true");

    Ok("hmng-remove", "remove reale/virtuale / doppio / inesistente ok");
}

// ==================================================== 15. hmng-gettype (issue 33)
static void TestHandlerMngGetType()
{
    REG_handler_mng* m = REG_handler_mng::getInstance();
    HandlerRecord rec;

    // Reale: tipo + HKEY originale recuperabile
    HKEY fr = m->registerRealHandler((HKEY)0x7777, "HKCU\\G");
    if (m->getHandlerType(fr) != HandlerType::Real) Fail("hmng-gettype", "tipo reale != Real");
    if (!m->getHandler(fr, &rec))                   Fail("hmng-gettype", "getHandler(reale) fallito");
    if (rec.real != (HKEY)0x7777)                   Fail("hmng-gettype", "HKEY originale errato");

    // Virtuale: tipo + percorso recuperabile
    HKEY fv = m->registerVirtualHandler("HKLM\\V");
    if (m->getHandlerType(fv) != HandlerType::Virtual) Fail("hmng-gettype", "tipo virtuale != Virtual");
    if (!m->getHandler(fv, &rec))                      Fail("hmng-gettype", "getHandler(virtuale) fallito");
    if (rec.keyPath != "HKLM\\V")                      Fail("hmng-gettype", "percorso errato");

    // Sconosciuto
    if (m->getHandlerType((HKEY)0x2) != HandlerType::Unknown) Fail("hmng-gettype", "sconosciuto != Unknown");

    Ok("hmng-gettype", "Real(+HKEY) / Virtual(+path) / sconosciuto ok");
}

// ======================================================================= 16. keystate (issue 41)
static void TestSqlregKeyState()
{
    char tmp[MAX_PATH] = "";
    GetTempPathA(MAX_PATH, tmp);
    std::string db = std::string(tmp) + "winregproxy_keystate.db";
    DeleteFileA(db.c_str());

    SQLLite_REG reg(db);

    // connessione di servizio (stesso file) per impostare lo stato a mano
    sqlite3* h = nullptr;
    if (sqlite3_open(db.c_str(), &h) != SQLITE_OK)
        Fail("keystate", "non riesco ad aprire il db di servizio");
    auto exec = [&](const char* s) { sqlite3_exec(h, s, nullptr, nullptr, nullptr); };

    // 1) inesistente -> 0 (assente nel DB: potrebbe esistere nel reale)
    if (reg.KeyState("HKCU\\NonEsiste\\X") != 0)
        Fail("keystate", "chiave inesistente -> stato != 0");

    // 2) foglia virtuale viva (con genitore) -> 1; genitore non nel DB -> 0
    exec("INSERT INTO keys(key_path) VALUES('HKCU\\A\\B')");
    if (reg.KeyState("HKCU\\A\\B") != 1)
        Fail("keystate", "chiave virtuale viva -> stato != 1");
    if (reg.KeyState("HKCU\\A") != 0)
        Fail("keystate", "genitore non nel DB -> stato != 0");

    // 3) foglia soft-deletata -> -1 (rianimata: torna 1)
    exec("UPDATE keys SET is_deleted=1 WHERE key_path='HKCU\\A\\B'");
    if (reg.KeyState("HKCU\\A\\B") != -1)
        Fail("keystate", "chiave cancellata -> stato != -1");
    exec("UPDATE keys SET is_deleted=0 WHERE key_path='HKCU\\A\\B'");
    if (reg.KeyState("HKCU\\A\\B") != 1)
        Fail("keystate", "chiave rianimata -> stato != 1");

    // 4) sotto chiavi di una cancellata -> -1 anche se la riga figlia e' viva;
    //    la cancellata in se' -> -1
    exec("INSERT INTO keys(key_path) VALUES('HKCU\\A')");
    exec("UPDATE keys SET is_deleted=1 WHERE key_path='HKCU\\A'");
    if (reg.KeyState("HKCU\\A") != -1)
        Fail("keystate", "chiave cancellata (se stessa) -> stato != -1");
    if (reg.KeyState("HKCU\\A\\B") != -1)
        Fail("keystate", "figlia di chiave cancellata -> stato != -1");

    sqlite3_close(h);
    DeleteFileA(db.c_str());

    // 5) DB non apribile -> sempre 0 (l'adapter degrada a pass-through)
    SQLLite_REG bad(std::string(tmp) + "__dir_assente_winregproxy__\\x.db");
    if (bad.isOpen())
        Fail("keystate", "DB in percorso invalido aperto (inaspettato)");
    if (bad.KeyState("HKCU\\A") != 0)
        Fail("keystate", "DB non aperto -> stato != 0");

    Ok("keystate", "assente=0 / viva=1 / cancellata=-1 / sotto-cancellata=-1 / chiuso=0 ok");
}

// ======================================================================= 17. valuestate (issue 43)
static void TestSqlregValueState()
{
    char tmp[MAX_PATH] = "";
    GetTempPathA(MAX_PATH, tmp);
    std::string db = std::string(tmp) + "winregproxy_valuestate.db";
    DeleteFileA(db.c_str());

    SQLLite_REG reg(db);

    reg.CreateKey("HKCU\\VirtualTest\\ValueState");
    reg.SetValue("HKCU\\VirtualTest\\ValueState", "v", REG_SZ,
                 { (unsigned char)0x00, 0x00, 0x00, 0x00 });

    // 1) presente non cancellato -> 1
    if (reg.ValueState("HKCU\\VirtualTest\\ValueState", "v") != 1)
        Fail("valuestate", "valore presente -> stato != 1");
    // 2) mai esistito / chiave assente / nome vuoto (default) -> 0
    if (reg.ValueState("HKCU\\VirtualTest\\ValueState", "ghost") != 0)
        Fail("valuestate", "valore mai esistito -> stato != 0");
    if (reg.ValueState("HKCU\\VirtualTest\\Ghost", "v") != 0)
        Fail("valuestate", "chiave assente -> stato != 0");
    if (reg.ValueState("HKCU\\VirtualTest\\ValueState", "") != 0)
        Fail("valuestate", "default assente -> stato != 0");
    // 3) cancellato -> -1 (e la riga resta nel DB, soft-delete)
    reg.DeleteValue("HKCU\\VirtualTest\\ValueState", "v");
    if (reg.ValueState("HKCU\\VirtualTest\\ValueState", "v") != -1)
        Fail("valuestate", "valore cancellato -> stato != -1");
    // 4) chiave cancellata -> -1 anche per i valori
    reg.DeleteKey("HKCU\\VirtualTest\\ValueState");
    if (reg.ValueState("HKCU\\VirtualTest\\ValueState", "v") != -1)
        Fail("valuestate", "chiave cancellata -> stato != -1");

    DeleteFileA(db.c_str());

    // 5) DB non aperto -> sempre 0 (l'adapter degrada a pass-through)
    SQLLite_REG bad(std::string(tmp) + "__dir_assente_winregproxy__\\x.db");
    if (bad.isOpen())
        Fail("valuestate", "DB in percorso invalido aperto (inaspettato)");
    if (bad.ValueState("HKCU\\A", "v") != 0)
        Fail("valuestate", "DB non aperto -> stato != 0");

    Ok("valuestate", "presente=1 / assente=0 / cancellato=-1 / chiave-cancellata=-1 / chiuso=0 ok");
}

// ======================================================================= main
int main()
{
    printf("WinRegProxy - ambiente di test di unita' (fase 2)\n");
    TestSqlite();
    TestSqlregInit();
    TestSqlregExistKey();
    TestSqlregCreateKey();
    TestSqlregQueryValue();
    TestSqlregSetValue();
    TestSqlregDeleteValue();
    TestSqlregDeleteKey();
    TestSqlregEnumKey();
    TestSqlregEnumValue();
    TestSqlregQueryInfo();
    TestHandlerMngReal();
    TestHandlerMngVirtual();
    TestHandlerMngRemove();
    TestHandlerMngGetType();
    TestSqlregKeyState();
    TestSqlregValueState();

    printf("\nRiassunto: %d sezioni superate, 0 fallite\n", s_pass);
    fflush(stdout);
    return 0;
}
