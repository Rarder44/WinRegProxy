#pragma once
#include <string>
using namespace std;

/// <summary>
/// Permette di gestire la scrittura e la lettura di chiavi di registro in un database SQLite. 
/// Questa classe fornisce metodi per inserire, aggiornare, eliminare e recuperare valori di registro da un database SQLite.
/// </summary>

class SQLLite_REG
{
public:
	SQLLite_REG(string path);
	~SQLLite_REG();

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
	string  dbPath;
	void openDB();
	void closeDB();
};

