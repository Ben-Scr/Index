#pragma once
#include <atomic>
#include <cassert>
#include <functional>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include "Collections/Ids.hpp"

namespace Index {
    template<typename... Args>
    class Event {
    public:
        using Callback = std::function<void(Args...)>;

        EventId Add(Callback cb) {
            ThrowIfDispatchingThisEvent("Event::Add");
            std::unique_lock lock(m_Mutex);
            const EventId id = EventId(++m_NextId.value);
            m_Listeners.push_back({ id, std::move(cb) });
            return id;
        }

        // Remove takes an exclusive lock (Invoke holds shared), so a destructor calling Remove is guaranteed no callback fires against `this` after it returns.
        bool Remove(EventId id) {
            ThrowIfDispatchingThisEvent("Event::Remove");
            std::unique_lock lock(m_Mutex);
            auto it = std::remove_if(m_Listeners.begin(), m_Listeners.end(), [id](const Entry& e) { return e.id == id; });
            const bool removed = (it != m_Listeners.end());
            m_Listeners.erase(it, m_Listeners.end());
            return removed;
        }

        void Clear() {
            ThrowIfDispatchingThisEvent("Event::Clear");
            std::unique_lock lock(m_Mutex);
            m_Listeners.clear();
        }

        bool HasListeners() const {
            std::shared_lock lock(m_Mutex);
            return !m_Listeners.empty();
        }

        void Invoke(Args... args) {
            // std::shared_mutex is NOT recursive: a thread that already holds the
            // shared lock and re-enters Invoke (e.g. a Log::OnLog listener that logs)
            // would take a second lock_shared on the same mutex — UB, and a hard
            // deadlock against any queued writer. On re-entry the outer Invoke still
            // holds the lock, so m_Listeners is already protected; iterate without
            // re-locking. (Add/Remove/Clear stay barred from inside a callback.)
            const bool reentrant = CallerIsDispatchingThisEvent();
            std::shared_lock<std::shared_mutex> lock(m_Mutex, std::defer_lock);
            if (!reentrant)
                lock.lock();
            auto& depths = DispatchDepth();
            ++depths[this];
            // RAII so the depth is decremented even if a listener throws.
            struct DepthGuard {
                std::unordered_map<const void*, int>& Map;
                const void* Key;
                ~DepthGuard() { if (--Map[Key] <= 0) Map.erase(Key); }
            } guard{ depths, this };
            for (const auto& e : m_Listeners) {
                e.cb(args...);
            }
            // dispatch depth is restored by DepthGuard above (throw-safe).
        }


    private:
        struct Entry {
            EventId id;
            Callback cb;
        };

        static std::unordered_map<const void*, int>& DispatchDepth() {
            thread_local std::unordered_map<const void*, int> depths;
            return depths;
        }

        bool CallerIsDispatchingThisEvent() const {
            auto& depths = DispatchDepth();
            auto it = depths.find(this);
            return it != depths.end() && it->second > 0;
        }

        void ThrowIfDispatchingThisEvent(const char* op) const {
            if (CallerIsDispatchingThisEvent()) {
                // Throwing surfaces the bug at the offending call site (and
                // unwinds cleanly through the dispatcher's shared_lock) rather
                // than deadlocking against our own unique_lock.
                throw std::logic_error(std::string(op) +
                    " called from inside its own callback — would self-deadlock.");
            }
        }

        mutable std::shared_mutex m_Mutex;
        std::vector<Entry> m_Listeners;
        EventId m_NextId;
        // Self-deadlock detection lives in the thread_local DispatchDepth()
        // map (see CallerIsDispatchingThisEvent / Invoke) — no per-instance
        // member is needed, and it stays correct under concurrent Invoke.
    };
}
