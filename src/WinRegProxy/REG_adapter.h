#pragma once
#include "REG_handler_mng.h"	
#include "SQLLite_REG.h"

/// <summary>
/// gestisce l'instradamento delle richieste tra il registro reale e quello virtuale (sqlite)
/// Questa è la classe che permette di instradare o unire i risultati provenienti dal DB e dal REG reale
/// </summary>

class REG_adapter
{
public:
	// Singleton lazy: creata al PRIMO USO (da un detour, NON in DllMain,
	// per non fare file I/O sotto loader lock). self = HMODULE della DLL, usato
	// dal costruttore per calcolare il path del DB. Finché nessun detour la
	// chiama (wiring 60-67) non esiste e il DB non si crea. Se il DB non si apre
	// l'adattatore degrada a pass-through (decisione in issues/40).
	static REG_adapter* getInstance(HMODULE self) {
		if (instance == nullptr)
			instance = new REG_adapter(self);
		return instance;
	}

	// Open di una chiave (issue 41; lo chiama il detour open, issue 60).
	// keyPath = percorso assoluto UTF-8 con token di hive (es. "HKCU\\Software\\X").
	//   - chiave soft-deletata nel DB: out = ERROR_FILE_NOT_FOUND, false
	//     (vince sul reale: e' "scomparsa" anche se esiste nel registro reale)
	//   - chiave virtuale viva nel DB: id fittizio (record Virtual),
	//     out = ERROR_SUCCESS, true. Il registro reale non viene toccato.
	//   - altrimenti (o DB non aperto): open REALE; se riesce, l'HKEY reale e'
	//     registrato (record Real con keyPath) e se ne restituisce l'id fittizio
	//     (out = ERROR_SUCCESS, true); se fallisce, out = errore reale, false.
	// outId = 0 se l'open non e' riuscito.
	bool OpenKey(const std::string& keyPath, REGSAM access, LSTATUS& out, unsigned& outId);
	// CreateKey: crea/riattiva la chiave nel DB (scrittura SOLO nel DB, mai nel
	// registro reale; virtualizzazione globale, senza prefisso di sottoalbero: segue
	// 00-phase2). disposition = REG_CREATED_NEW_KEY se la chiave non c'era nel DB, altrimenti
	// REG_OPENED_EXISTING_KEY (il DB e' la fonte di coerenza). outId = id fittizio VIRTUALE
	// da restituire all'app. access/sam non e' parametrate: la chiave e' virtuale, non c'e'
	// reale ACL da rispettare. true = gestita; false = non gestita (DB non aperto o
	// CreateKey fallita: il detour passa al reale).
	bool CreateKey(const std::string& keyPath, DWORD* disposition, LSTATUS& out, unsigned& outId);

	// Issue 68: id fittizio del proxy (record reale o virtuale) -> path completo
	// (+sub, vuoto = la chiave stessa). false se l'id non e' gestito dal proxy:
	// il detour open/create deve passare al reale (comportamento attuale).
	bool ResolveId(HKEY id, const std::wstring& sub, std::string* out);

	// Read di un valore (issue 43; lo chiamano i detour query, issue 62).
	// id = id fittizio (REG_handler_mng) oppure root reale nota; sub = sottochiave
	// relativa (vuota = la chiave stessa); name = nome valore (vuota = predefinito).
	// Risolve "stato + dati" senza applicare le semantica dei buffer (le applica
	// il detour di 62, che conosce la famiglia: QVEx 2-pass, QV exact, GV +2).
	//   - valore virtualmente cancellato (o chiave cancellata):
	//     out->status = ERROR_FILE_NOT_FOUND, data vuota (vince sul reale);
	//   - valore presente nel DB: out->status = ERROR_SUCCESS, type + data
	//     (stringhe gia' convertite in wide, con NUL finale come nel reale);
	//   - valore assente nel DB: lettura REALE (open root + query, chiusura
	//     interna): out->status = codice reale (SUCCESS/NOT_FOUND/...), data
	//     (stringhe wide con NUL). Il registro reale NON viene mai modificato.
	// out->virtualHit = true se la risposta e' nata dal DB (scrittura/cancellazione
	// virtuale); false se nata dal reale.
	// true = gestita (out valida); false = non gestita (DB non aperto, o id
	// sconosciuto/non fittizio: il detour passa al reale con gli argomenti originali).
	struct RegQueryOut
	{
		LSTATUS     status;
		DWORD       type;
		std::string data;     // byte grezzi: stringhe = wide (con NUL finale)
		bool        virtualHit;
	};
	bool QueryValue(HKEY id, const std::wstring& sub, const std::wstring& name, RegQueryOut* out);

	// ===== Set (44): scrive/sovrascrive un valore SOLO nel DB (mai nel reale).
	// id = handle gestito (fittizio) oppure root reale (HKCU, HKLM, ...):
	// la firma copre le tre famiglie (SetValueEx*/SetValue* con id+name e
	// SetKeyValue* con root+sub+name; sub vuoto = la chiave stessa, name
	// vuoto = default value).
	// data = byte grezzi, gia' normalizzati UTF-16LE per i tipi stringa dal
	// detour (A->W); non-stringa (DWORD/BINARY): byte grezzi. L'adapter li
	// converte nella convenzione DB (stringhe: UTF-8 senza NUL; QueryValue
	// riacquista il NUL in uscita).
	// true = gestita (out valida; errore DB -> status ACCESS_DENIED);
	// false = non gestita (DB non aperto o id sconosciuto: il detour passa al
	// reale con gli argomenti originali).
	struct RegSetOut
	{
		LSTATUS     status;
		bool        virtualHit;
	};
	bool SetValue(HKEY id, const std::wstring& sub, const std::wstring& name, DWORD type, const std::string& data, RegSetOut* out);

	// ===== Delete (45/46): cancellazione virtuale SOLO nel DB (mai nel reale).
	// id = handle gestito (fittizio) oppure root reale; sub = sottochiave
	// relativa (vuoto = la chiave); per DeleteValue: name = nome valore
	// (vuoto = default value). La riga nel DB VINCE: presente (viva o gia'
	// cancellata) -> soft-delete nel DB, out->status = SUCCESS, true.
	// Assente dal DB:
	//   - record Real o root nota -> false: il detour passa al reale con gli
	//     argomenti originali (fase 1, semantica reale);
	//   - record Virtual -> out->status = ERROR_FILE_NOT_FOUND, true
	//     (non esiste: non c'e' un reale di fallback per le chiavi virtuali).
	struct RegDelOut
	{
		LSTATUS     status;
		bool        virtualHit;
	};
	bool DeleteValue(HKEY id, const std::wstring& sub, const std::wstring& name, RegDelOut* out);
	bool DeleteKey(HKEY id, const std::wstring& sub, RegDelOut* out);

	// ===== Enum (47/48): unione DB+reale. Flusso unico per chiavi reali,
	// virtuali e root: nomi reali (Real_RegEnum*, ordine reale) + nomi
	// solo-DB (ordine per nome), senza duplicati; i nomi soft-deleted nel DB
	// oscurano il reale (KeyState/ValueState == -1). out->status: SUCCESS
	// (name[/type/data] validi), ERROR_NO_MORE_ITEMS a fine, errore reale se la
	// chiave non esiste da nessuna parte. false/allowed=false -> il detour va
	// al reale con gli argomenti originali (id non gestito).
	struct RegEnumOut
	{
		LSTATUS     status;
		bool        virtualHit;   // false -> il detour va al reale
		std::wstring name;        // sottochiave (EnumKey) o valore (EnumValue)
		DWORD       type;         // EnumValue: REG_*
		std::vector<BYTE> data;   // EnumValue: dati in formato API
	};
	bool EnumKey(HKEY id, DWORD index, RegEnumOut* out);
	bool EnumValue(HKEY id, DWORD index, RegEnumOut* out);

	// ===== QueryInfo (49): info della chiave unita DB+reale. Stessa strategia
	//     di unione di 47/48: conteggi = reali non soft-deleted + solo-DB;
	//     max* = max sull'unione; lastWrite/seclen dalla parte reale (0 per
	//     chiave virtuale pura). out->status: SUCCESS (out validi),
	//     NOT_FOUND se cancellata virtualmente o assente da entrambe.
	//     false/allowed=false -> il detour va al reale con gli argomenti
	//     originali (id non gestito).
	//   - nomi in wchar SENZA NUL, dato in byte (come il reale)
	struct RegQueryInfoOut
	{
		LSTATUS     status;
		bool        virtualHit;   // false -> il detour va al reale
		DWORD       numSubkeys;
		DWORD       numValues;
		DWORD       maxSubkeyNameLen;
		DWORD       maxValueNameLen;
		DWORD       maxValueDataLen;
		DWORD       securityDescLen;
		FILETIME    lastWrite;
	};
	bool QueryInfo(HKEY id, RegQueryInfoOut* out);

	// ===== CloseKey (50): rilascia l'id fittizio. Record Real ->
	//     Real_RegCloseKey(rec.real) + removeHandler (out = status reale);
	//     record Virtual -> removeHandler, out = SUCCESS. false -> id non
	//     gestito (il detour chiude l'HKEY originale col reale).
	bool CloseKey(HKEY id, LSTATUS& out);
private:
	// Helper Enum (47/48): voce unita della lista e merge DB+reale.
	struct EnumEntryW
	{
		std::wstring        name;
		DWORD               type;
		std::vector<BYTE>   data;
	};
	static bool EnumRealKeys(const std::string& path, std::vector<EnumEntryW>* list, LSTATUS* openErr);
	static bool EnumRealValues(const std::string& path, std::vector<EnumEntryW>* list, LSTATUS* openErr);
	static void DbEnumKeys(SQLLite_REG* db, const std::string& path, std::vector<EnumEntryW>* list);
	static void DbEnumValues(SQLLite_REG* db, const std::string& path, std::vector<EnumEntryW>* list);
	static bool MergedEnumEntry(SQLLite_REG* db, const std::string& path, bool values,
		const std::vector<EnumEntryW>& realList, DWORD index, RegEnumOut* out);
	static REG_adapter* instance;

	// Crea/apre la SQLLite_REG sul path risolto (accanto alla DLL, ripiego %TEMP%).
	REG_adapter(HMODULE self);

	SQLLite_REG* sqlite_reg;
	REG_handler_mng* handler_mng;

	//per ciascuno dei metodi, occorre controllare se la chiave/hanlder è presente nel DB virtuale (sqlite) o meno, e instradare la richiesta di conseguenza.
	//tutti i metodi ritornano un LSTATUS, come le funzioni Reg* originali.
	// gli altri return vengono gestiti con passaggio di parametri per riferimento (es. PHKEY, LPBYTE, ecc.)
	// i metodi non devono fare alcuna distinzione tra ANSI e UNICODE, perché il DB virtuale (sqlite) memorizza le chiavi in formato UNICODE (UTF-16).
	// i metodi verranno già richiamati dai trampolini creati da MinHook, quindi non occorre preoccuparsi di fare alcuna distinzione tra ANSI e UNICODE. ( sarà il trampolino a fare la conversione dei parametri in caso di chiamata ANSI)

	
	//TODO: implementare i metodi per la gestione dell'instradamento delle richieste tra il registro reale e quello virtuale (sqlite)
	/*
		-OpenKey
			controlla se la key esiste nel DB virtuale, se si crea un handle virtuale in REG_handler_mng e lo restituisce.
			se no, richiama openKey del registro reale e registra l'handle in REG_handler_mng.
			se la key era stata cancellata virtualmente, ritorna un errore (come se la key non esistesse) indipendentemente dal fatto che esista o meno nel registro reale.

		- CreateKey 
			crea/apre la chiave virtuale nel DB e registra l'handle in REG_handler_mng.
			non fa nulla nel registro reale.

		-QueryValue	
			Legge un valore, se non presente nel DB, lo legge dal registro reale e ritorna l'errore del registro reale nel caso.
			Se il valore era stato cancellato virtualmente, ritorna un errore (come se il valore non esistesse) indipendentemente dal fatto che esista o meno nel registro reale.

		-SetValue	
			Scrive o sovrascrive un valore nel DB.
			non fa nulla nel registro reale.

		-DeleteValue 
			Registra nel DB la cancellazione virtuale del valore.


		-DeleteKey	
			Registra la cancellazione virtuale nel DB della chiave e del relativo sottoalbero.


		-EnumKey	
			Enumera le sottochiavi combinando DB e registro reale.

		-EnumValue	
			Enumera i valori combinando DB e registro reale.

		-QueryInfo	
			Restituisce informazioni coerenti sulla chiave risultante dall'unione di QueryInfo del DB e del registro reale.

		-CloseKey	
			Rilascia l'handle e le risorse associate.
			Se l'handle era virtuale, rimuove l'associazione in REG_handler_mng.
			se l'handle era reale, richiama CloseKey del registro reale e rimuove l'associazione in REG_handler_mng.

	*/


};

