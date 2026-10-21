#pragma once
#include <windows.h>
#include <map>
#include <string>

// tipo di handler: reale (handle OS) o virtuale (chiave nel DB)
enum class HandlerType { Unknown, Real, Virtual };

// record di un id handler fittizio: cosa c'è dietro.
struct HandlerRecord
{
	HandlerType type;      // Real o Virtual
	HKEY        real;      // Real: handle reale; Virtual: nullptr
	std::string keyPath;   // Virtual: percorso chiave (UTF-8, gestito in 31); Real: vuoto
};

//gestione unica di handler reali e virtuali
//gestisce gli handler aperti virtuali e reali in una mappa che memorizza
//- id handler fittizzio -> generato dalla libreria per mascherare l'handler reale/virtuali
//- tipo handler -> reale o virtuale
// SE handler reale -> - salvataggio id handler reale
// SE handler virtuale -> - salvataggio chiave di registro ( e tutte le info che servono ) associata all'handler
class REG_handler_mng
{
	static REG_handler_mng* instance;

private:
	REG_handler_mng() {}
	~REG_handler_mng() {}

	std::map<HKEY, HandlerRecord> m_handlers;   // id fittizio -> record
	SRWLOCK m_lock = SRWLOCK_INIT;              // la mappa è toccata da tutti i detour (multi-thread)
	volatile LONG m_nextId = 0x1000;            // base per gli id fittizi (lontano da HKEY predefiniti 0x80000001+ e da NULL); atomico

public:
	static REG_handler_mng* getInstance() {
		if (instance == nullptr) {
			instance = new REG_handler_mng();
		}
		return instance;
	}

	// Registra un handle reale e restituisce un id fittizio unico e monotono.
	// (L'adapter passa all'app l'id fittizio; l'HKEY reale resta nel record.)
	// keyPath = percorso assoluto UTF-8 della chiave ("HKxx\..."): serve a
	// ResolveRoot (issue 60) se questo id fittizio viene usato come parent.
	HKEY registerRealHandler(HKEY real, const std::string& keyPath);

	// Tipo di handler (reale/virtuale) dato un id fittizio; Unknown se sconosciuto.
	// Implementato in 30 (servire al test di 30); verificato/completato in 33.
	HandlerType getHandlerType(HKEY id);

	// Registra un handler virtuale (nessun HKEY reale) e restituisce un id
	// fittizio unico; memorizza il percorso chiave (UTF-8, usato sul DB).
	HKEY registerVirtualHandler(const std::string& keyPath);

	// Copia del record per un id fittizio (true se presente, *out riempito).
	// Usata dall'adapter (34+) per sapere HKEY reale o percorso chiave; anche dai test.
	bool getHandler(HKEY id, HandlerRecord* out);

	// Rimuove l'associazione di un id fittizio (true se rimosso, false se assente,
	// che è anche il caso di doppia remove). NON chiude l'HKEY reale (RegCloseKey):
	// la chiusura è dell'adapter (50/67).
	bool removeHandler(HKEY id);
};
