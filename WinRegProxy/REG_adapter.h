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
	REG_adapter() {
		handler_mng = REG_handler_mng::getInstance();
	}
private:
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

