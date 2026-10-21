#include "pch.h"
#include "REG_handler_mng.h"

// definizione del puntatore statico del singleton
REG_handler_mng* REG_handler_mng::instance = nullptr;

// ===== registerRealHandler =====
HKEY REG_handler_mng::registerRealHandler(HKEY real, const std::string& keyPath)
{
    // id fittizio unico e monotono (atomico). Base 0x1000: non collide con gli
    // HKEY predefiniti (0x80000001..5) né con NULL/0.
    LONG id = InterlockedIncrement(&m_nextId);
    HKEY fake = reinterpret_cast<HKEY>(static_cast<UINT_PTR>(id));

    // keyPath (percorso assoluto "HKxx\...") salvato anche per i reali: serve
    // a ResolveRoot (issue 60) quando un id fittizio reale viene usato come
    // parent per aprire un figlio.
    AcquireSRWLockExclusive(&m_lock);
    m_handlers[fake] = HandlerRecord{ HandlerType::Real, real, keyPath };
    ReleaseSRWLockExclusive(&m_lock);
    return fake;
}

// ===== getHandlerType =====
HandlerType REG_handler_mng::getHandlerType(HKEY id)
{
    AcquireSRWLockShared(&m_lock);
    auto it = m_handlers.find(id);
    HandlerType t = (it == m_handlers.end()) ? HandlerType::Unknown : it->second.type;
    ReleaseSRWLockShared(&m_lock);
    return t;
}

// ===== registerVirtualHandler =====
HKEY REG_handler_mng::registerVirtualHandler(const std::string& keyPath)
{
    LONG id = InterlockedIncrement(&m_nextId);
    HKEY fake = reinterpret_cast<HKEY>(static_cast<UINT_PTR>(id));

    AcquireSRWLockExclusive(&m_lock);
    m_handlers[fake] = HandlerRecord{ HandlerType::Virtual, nullptr, keyPath };
    ReleaseSRWLockExclusive(&m_lock);
    return fake;
}

// ===== getHandler =====
bool REG_handler_mng::getHandler(HKEY id, HandlerRecord* out)
{
    if (!out) return false;
    AcquireSRWLockShared(&m_lock);
    auto it = m_handlers.find(id);
    if (it == m_handlers.end()) { ReleaseSRWLockShared(&m_lock); return false; }
    *out = it->second;
    ReleaseSRWLockShared(&m_lock);
    return true;
}

// ===== removeHandler =====
bool REG_handler_mng::removeHandler(HKEY id)
{
    AcquireSRWLockExclusive(&m_lock);
    auto it = m_handlers.find(id);
    if (it == m_handlers.end()) { ReleaseSRWLockExclusive(&m_lock); return false; }
    m_handlers.erase(it);   // il keyPath (string) viene liberato col record
    ReleaseSRWLockExclusive(&m_lock);
    return true;
}
