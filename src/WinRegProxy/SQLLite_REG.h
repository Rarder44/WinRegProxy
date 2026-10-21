#pragma once
#include <string>
#include <vector>
using namespace std;

struct sqlite3; // handle opaco SQLite (gestito in SQLLite_REG.cpp)

// Un valore di registro letto/enumerato dal DB (type/data come memorizzati).
struct RegValue
{
    string name;   // "" = valore predefinito
    int    type;   // REG_* (es. REG_SZ, REG_BINARY)
    string data;   // byte grezzi (blob)
};

// Info sulla chiave nella "vista DB" (quello che il DB sa).
struct RegKeyInfo
{
    int numSubkeys;   // sottochiavi immediate vive
    int numValues;    // valori vivi (includendo il predefinito)
};

/// <summary>
/// Permette di gestire la scrittura e la lettura di chiavi di registro in un database SQLite. 
/// Questa classe fornisce metodi per inserire, aggiornare, eliminare e recuperare valori di registro da un database SQLite.
/// </summary>

class SQLLite_REG
{
public:
	SQLLite_REG(string path);
	~SQLLite_REG();

	// true se openDB e' riuscito (db != nullptr). Se false, tutti i metodi qui
	// sotto restituiscono errore/empty -> l'adattatore degrada a pass-through.
	bool isOpen() const { return db != nullptr; }

	// true se la chiave esiste nel DB e non e' soft-deletata (ne lei ne' un
	// genitore). keyPath e' il percorso assoluto UTF-8 (es. "HKCU\\Software\\X").
	bool ExistKey(const string& keyPath);
	// Stato di un valore nel DB (issue 43; lo usa l'adattatore QueryValue):
	//   -1 = soft-deletato (o la chiave/una ascendente e' cancellata: nascosto);
	//    0 = assente;
	//    1 = presente e vivo.
	// DB non aperto -> 0 (nessun valore virtuale: l'adattatore va al reale).
	int ValueState(const string& keyPath, const string& name);

	// Stato della chiave per l'adattatore (issue 41): -1 = soft-deletata (o un
	// genitore lo e': la chiave e' "scomparsa"), 0 = assente nel DB (potrebbe
	// esistere nel registro reale), 1 = presente e viva solo nel DB (virtuale).
	// Stessa "salita sui genitori" di ExistKey, ma distingue "assente" da
	// "cancellata": e' lo stato che serve all'open per scegliere il ramo.
	int KeyState(const string& keyPath);

	// Crea la chiave (e tutte le chiavi genitrici, fino al token di hive) nel DB,
	// rianimando quelle soft-deletate. true = ok (chiave ora 'viva' nel DB).
	bool CreateKey(const string& keyPath);

	// Legge un valore. true = presente e non cancellato (ne il valore ne' la
	// chiave): *type/*data vengono riempiti (passare nullptr per quello che non
	// serve). false = assente o soft-deletato (o chiave cancellata): l'adapter fa
	// fallback al registro reale. name="" = valore predefinito.
	bool QueryValue(const string& keyPath, const string& name, int* type, string* data);

	// Scrive/sovrascrive un valore (upsert su key_path+name, is_deleted=0) e
	// riattiva la chiave (CreateKey implicito). true = ok. Rianima un valore
	// soft-deletato con lo stesso nome. data = byte grezzi (blob).
	bool SetValue(const string& keyPath, const string& name, int type, const string& data);

	// Soft delete di un valore (is_deleted=1, la riga resta nel DB).
	// true = una riga viva e' stata marcata; false = non c'era nulla da
	// cancellare (valore assente o gia' cancellato) -> l'adapter ritorna
	// ERROR_FILE_NOT_FOUND.
	bool DeleteValue(const string& keyPath, const string& name);

	// Soft delete della chiave E dell'intero sottoalbero (is_deleted=1 sulle
	// chiavi, non DELETE). I valori sotto NON sono marcati: restano invisibili
	// perch&eacute; ExistKey/QueryValue/Enum* filtrano sulla chiave viva.
	// true = chiave viva cancellata; false = assente/cancellata (no-op).
	bool DeleteKey(const string& keyPath);

	// Sottochiavi immediate (solo il segmento foglia, es. "B1"), ordinate
	// lessicograficamente, vive (non soft-deletate). Vettore vuoto = nessuna
	// sottochiave nel DB (l'adapter fa fallback/combinazione col reale in 47).
	vector<string> EnumKey(const string& keyPath);

	// Valori della chiave presenti nel DB, vivi (valore e chiave non cancellati),
	// in ordine per nome. Il valore predefinito (name="") NON è incluso (come
	// RegEnumValue reale). Vettore vuoto = nessuno (o chiave assente/cancellata).
	vector<RegValue> EnumValue(const string& keyPath);

	// Info sulla chiave nella vista DB (conteggi coerenti con lo stato virtuale,
	// usati dall'adapter in 49 per unire col reale). true = chiave viva (info
	// riempita); false = chiave assente/cancellata.
	bool QueryInfo(const string& keyPath, RegKeyInfo* info);

	//dove serve, usare gli LSTATUS come valore di rotrno
	//TODO: implementare i metodi per la gestione del database SQLite
	/*
		ExistKey	-	legge un valore dal DB, se non presente ritorna un errore.
		CreateKey   -	Crea una chiave virtuale nel DB.
		QueryValue	-   Legge un valore, se non presente nel DB ritorna un errore.
		SetValue	-   Scrive o sovrascrive un valore nel DB.
		DeleteValue	- 
			Registra la cancellazione virtuale del valore. 
			se cancellato un valore, non è più possibile leggerlo, ma rimane nel DB contrassegnata come "cancellata" (soft delete).
			ovviamente, se avviene una nuova SetValue con lo stesso nome, il valore viene ricreato e funzionante.

		DeleteKey	-  
				Registra la cancellazione virtuale della chiave e del relativo sottoalbero.
				Come con la DeleteValue, la chiave rimane nel DB contrassegnata come "cancellata" (soft delete), e non è più possibile leggerla o enumerarla.

		EnumKey		-	Enumera le sottochiavi presenti nel DB.
		EnumValue	-	Enumera i valori  presenti nel DB.
		QueryInfo	-   Restituisce informazioni sulla chiave nel DB.
	*/


private:
	string  dbPath;   // percorso del file .db (UTF-8)
	sqlite3* db;      // handle aperto; nullptr se openDB e' fallito
	void openDB();
	void closeDB();
};

