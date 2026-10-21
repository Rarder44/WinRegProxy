// SQLLite_REG.cpp
// Storage della virtualizzazione in un file SQLite. Il registro reale NON viene
// mai toccato: solo letture/scritture sul file .db.
//
// Codifica (decisione issue 20):
//   key_path / name -> TEXT UTF-8 (il detour passa UTF-16; il round-trip UTF-16<->
//                      UTF-8 e' lossless, e TEXT rende facili enum/prefix e ordine).
//   data            -> BLOB (byte grezzi del valore).
//   is_deleted      -> soft delete (0=no, 1=sì): la riga resta nel DB.
// Errore = dato: se openDB fallisce db resta nullptr e i metodi restituiscono
// errore. Nessuna eccezione attraversa i livelli.
#include "pch.h"
#include "SQLLite_REG.h"
#include <algorithm>
#include "sqlite3.h"

// ===== schema =====
// keys:   key_path e' il percorso ASSOLUTO con token di hive (es. 'HKCU\Software\X').
//         SQLLite_REG lo tratta come opaco (chiave primaria = la chiave virtuale).
// values: name='' e' il valore predefinito. PK composito (key_path, name).
static const char* kSchemaSql =
    "CREATE TABLE IF NOT EXISTS keys ("
    "  key_path   TEXT PRIMARY KEY,"
    "  is_deleted INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS \"values\" ("
    "  key_path   TEXT NOT NULL,"
    "  name       TEXT NOT NULL,"
    "  type       INTEGER NOT NULL,"
    "  data       BLOB,"
    "  is_deleted INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (key_path, name)"
    ");";

// ===== costruttore / distruttore =====
SQLLite_REG::SQLLite_REG(string path) : dbPath(path), db(nullptr)
{
    openDB();
}

SQLLite_REG::~SQLLite_REG()
{
    closeDB();
}

// ===== open / close =====
void SQLLite_REG::openDB()
{
    int rc = sqlite3_open_v2(dbPath.c_str(), &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK)
    {
        if (db) { sqlite3_close(db); db = nullptr; }  // non apribile
        return;
    }

    char* err = nullptr;
    if (sqlite3_exec(db, kSchemaSql, nullptr, nullptr, &err) != SQLITE_OK)
    {
        if (err) sqlite3_free(err);
        sqlite3_close(db);
        db = nullptr;  // schema fallito: chiudi, segna come non aperto
        return;
    }
    // a questo punto: db aperto e schema garantito
}

void SQLLite_REG::closeDB()
{
    if (db)
    {
        sqlite3_close(db);
        db = nullptr;
    }
}

// ===== ExistKey =====
// Ritorna 0/1 (is_deleted) se la riga chiave esiste nel DB, -1 se assente.
static int KeyDeleted(sqlite3* db, const string& keyPath)
{
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT is_deleted FROM keys WHERE key_path=?",
            -1, &st, nullptr) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, keyPath.c_str(), keyPath.size(), SQLITE_TRANSIENT);
    int v = -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

bool SQLLite_REG::ExistKey(const string& keyPath)
{
    if (!db) return false;

    int d = KeyDeleted(db, keyPath);
    if (d < 0)  return false;   // non presente nel DB
    if (d != 0) return false;   // presente ma soft-deletata

    // Nessun genitore cancellato: la cancellazione di una chiave nasconde il
    // suo sottoalbero senza toccare le righe figlie.
    size_t first = keyPath.find('\\');
    if (first == string::npos) return true;   // solo il token di hive
    string p = keyPath;
    while (p.size() > first)
    {
        size_t i = p.rfind('\\');
        if (i <= first) break;      // nessun genitore intermedio oltre il token
        p = p.substr(0, i);
        if (KeyDeleted(db, p) == 1) return false;  // genitore soft-deletato
    }
    return true;
}

// ===== KeyState =====
// -1 cancellata (o sotto chiave cancellata) / 0 assente nel DB / 1 virtuale viva.
// Stessa salita sui genitori di ExistKey, ma qui "assente" (0) e' un risultato
// a se' stante: serve all'open per decidere il ramo (reale vs virtuale).
int SQLLite_REG::KeyState(const string& keyPath)
{
    if (!db) return 0;   // DB non aperto: l'adattatore degrada a pass-through

    int d = KeyDeleted(db, keyPath);
    if (d == 1) return -1;   // foglia soft-deletata: "scomparsa"

    // Genitore cancellato: il sottoalbero e' nascosto senza toccare le righe figlie.
    size_t first = keyPath.find('\\');
    if (first != string::npos)
    {
        string p = keyPath;
        while (p.size() > first)
        {
            size_t i = p.rfind('\\');
            if (i <= first) break;      // nessun genitore intermedio oltre il token
            p = p.substr(0, i);
            if (KeyDeleted(db, p) == 1) return -1;  // genitore soft-deletato
        }
    }
    return (d < 0) ? 0 : 1;   // -1/assente nel DB -> 0; 0/viva -> 1
}

// ===== ValueState (43) =====
// Stato di un valore: -1 cancellato (o chiave/ascendente cancellata: nascosto),
// 0 assente, 1 vivo. DB non aperto -> 0 (degrado a reale).
int SQLLite_REG::ValueState(const string& keyPath, const string& name)
{
    if (!db) return 0;
    if (KeyState(keyPath) == -1) return -1;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT is_deleted FROM \"values\" WHERE key_path=? AND name=?",
            -1, &st, nullptr) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, keyPath.c_str(), keyPath.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, name.c_str(), name.size(), SQLITE_TRANSIENT);
    int v = 0;
    if (sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int(st, 0) ? -1 : 1;
    sqlite3_finalize(st);
    return v;
}
// ===== CreateKey =====
bool SQLLite_REG::CreateKey(const string& keyPath)
{
    if (!db) return false;

    const char* sql =
        "INSERT INTO keys(key_path, is_deleted) VALUES(?, 0) "
        "ON CONFLICT(key_path) DO UPDATE SET is_deleted=0";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
        return false;

    auto upsert = [&](const string& kp) -> bool {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_text(st, 1, kp.c_str(), kp.size(), SQLITE_TRANSIENT);
        return sqlite3_step(st) == SQLITE_DONE;
    };

    bool ok = upsert(keyPath);   // foglia

    // Genitori (foglia->su), fino a prima del token di hive:
    // creare HKCU\A\B\C implica anche HKCU\A e HKCU\A\B.
    size_t first = keyPath.find('\\');
    string p = keyPath;
    while (ok && p.size() > first)
    {
        size_t i = p.rfind('\\');
        if (i <= first) break;
        p = p.substr(0, i);
        ok = upsert(p);
    }

    sqlite3_finalize(st);
    return ok;
}

// ===== QueryValue =====
bool SQLLite_REG::QueryValue(const string& keyPath, const string& name, int* type, string* data)
{
    if (!db) return false;
    if (!ExistKey(keyPath)) return false;   // chiave assente/cancellata -> non presente

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT type, data FROM \"values\" WHERE key_path=? AND name=? AND is_deleted=0",
            -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, keyPath.c_str(), keyPath.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, name.c_str(), name.size(), SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW)
    {
        found = true;
        if (type) *type = sqlite3_column_int(st, 0);
        if (data)
        {
            const unsigned char* b = (const unsigned char*)sqlite3_column_blob(st, 1);
            int n = sqlite3_column_bytes(st, 1);
            data->assign(b ? (const char*)b : "", n);   // blob nullo -> stringa vuota
        }
    }
    sqlite3_finalize(st);
    return found;
}

// ===== SetValue =====
bool SQLLite_REG::SetValue(const string& keyPath, const string& name, int type, const string& data)
{
    if (!db) return false;
    if (!CreateKey(keyPath)) return false;   // chiave implicita (altrimenti il valore non si leggerebbe)

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO \"values\"(key_path,name,type,data,is_deleted) VALUES(?,?,?,?,0) "
            "ON CONFLICT(key_path,name) DO UPDATE SET "
            "type=excluded.type, data=excluded.data, is_deleted=0",
            -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, keyPath.c_str(), keyPath.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, name.c_str(), name.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, type);
    sqlite3_bind_blob(st, 4, data.data(), (int)data.size(), SQLITE_TRANSIENT);

    bool ok = (sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    return ok;
}

// ===== DeleteValue =====
bool SQLLite_REG::DeleteValue(const string& keyPath, const string& name)
{
    if (!db) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "UPDATE \"values\" SET is_deleted=1 "
            "WHERE key_path=? AND name=? AND is_deleted=0",
            -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, keyPath.c_str(), keyPath.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, name.c_str(), name.size(), SQLITE_TRANSIENT);
    bool changed = (sqlite3_step(st) == SQLITE_DONE) && (sqlite3_changes(db) > 0);
    sqlite3_finalize(st);
    return changed;   // false = no-op (non c'era nulla di vivo da cancellare)
}

// ===== DeleteKey =====
bool SQLLite_REG::DeleteKey(const string& keyPath)
{
    if (!db) return false;
    if (!ExistKey(keyPath)) return false;   // assente/cancellata -> no-op (non tocchiamo il reale)

    // chiavi = keyPath + tutte le discendenti (keyPath + '\\' come prefisso).
    // instr() = ricerca sottomstringa senza wildcards (evita i problemi di LIKE
    // sui '_'/'%' e l'ambiguità byte/char di substr).
    const string childPrefix = keyPath + "\\";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "UPDATE keys SET is_deleted=1 "
            "WHERE is_deleted=0 AND (key_path=? OR instr(key_path, ?)=1)",
            -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, keyPath.c_str(), keyPath.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, childPrefix.c_str(), childPrefix.size(), SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(st) == SQLITE_DONE) && (sqlite3_changes(db) > 0);
    sqlite3_finalize(st);
    return ok;
}

// ===== EnumKey =====
std::vector<std::string> SQLLite_REG::EnumKey(const string& keyPath)
{
    std::vector<std::string> names;
    if (!db) return names;

    const string childPrefix = keyPath + "\\";   // chiavi sotto keyPath
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT key_path FROM keys WHERE is_deleted=0 AND instr(key_path, ?)=1",
            -1, &st, nullptr) != SQLITE_OK)
        return names;
    sqlite3_bind_text(st, 1, childPrefix.c_str(), childPrefix.size(), SQLITE_TRANSIENT);

    while (sqlite3_step(st) == SQLITE_ROW)
    {
        const char* kp = (const char*)sqlite3_column_text(st, 0);
        if (!kp) continue;
        string full = kp;
        size_t start = childPrefix.size();
        if (full.find('\\', start) != string::npos) continue;  // non immediato (più profondo)
        names.push_back(full.substr(start));                     // segmento foglia
    }
    sqlite3_finalize(st);

    sort(names.begin(), names.end());   // ordine deterministico (lessicografico)
    return names;
}

// ===== EnumValue =====
std::vector<RegValue> SQLLite_REG::EnumValue(const string& keyPath)
{
    std::vector<RegValue> values;
    if (!db) return values;
    // Solo cancellazione (chiave o genitore soft-deleted) -> niente. La chiave
    // REALE non ha riga in "keys": i suoi valori DB vanno comunque enumerati
    // (stessa regola di ValueState, 43: KeyState 0 = reale, non un blocco).
    if (KeyState(keyPath) == -1) return values;

    // Escluso il valore predefinito (name=''), come RegEnumValue reale.
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT name, type, data FROM \"values\" "
            "WHERE key_path=? AND is_deleted=0 AND name<>'' ORDER BY name",
            -1, &st, nullptr) != SQLITE_OK)
        return values;
    sqlite3_bind_text(st, 1, keyPath.c_str(), keyPath.size(), SQLITE_TRANSIENT);

    while (sqlite3_step(st) == SQLITE_ROW)
    {
        RegValue rv;
        const char* nm = (const char*)sqlite3_column_text(st, 0);
        rv.name = nm ? nm : "";
        rv.type = sqlite3_column_int(st, 1);
        const unsigned char* b = (const unsigned char*)sqlite3_column_blob(st, 2);
        int n = sqlite3_column_bytes(st, 2);
        rv.data.assign(b ? (const char*)b : "", n);
        values.push_back(rv);
    }
    sqlite3_finalize(st);
    return values;
}

// ===== QueryInfo =====
bool SQLLite_REG::QueryInfo(const string& keyPath, RegKeyInfo* info)
{
    if (!db) return false;
    if (!ExistKey(keyPath)) return false;   // assente/cancellata

    if (info)
    {
        info->numSubkeys = (int)EnumKey(keyPath).size();   // immediate vive (DRY: riuso EnumKey)

        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
                "SELECT count(*) FROM \"values\" WHERE key_path=? AND is_deleted=0",
                -1, &st, nullptr) != SQLITE_OK)
            return false;
        sqlite3_bind_text(st, 1, keyPath.c_str(), keyPath.size(), SQLITE_TRANSIENT);
        int n = 0;
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
        info->numValues = n;   // vivi, includendo il predefinito
    }
    return true;
}
