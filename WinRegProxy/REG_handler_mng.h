#pragma once

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
	REG_handler_mng() {};
	~REG_handler_mng() {};


public:
	static REG_handler_mng* getInstance() {
		if (instance == nullptr) {
			instance = new REG_handler_mng();
		}
		return instance;
	}



	//TODO: implementare i metodi per la gestione degli handler reali e virtuali
	/*
		registerRealHandler	- Registra un handler reale e restituisce un id fittizio.
		registerVirtualHandler	- Registra un handler virtuale e restituisce un id fittizio.
		removeHandler	- Chiude un handler reale o virtuale e rimuove l'associazione.
		getHandlerType	- Restituisce il tipo di handler (reale o virtuale) dato un id fittizio.

	*/
};

