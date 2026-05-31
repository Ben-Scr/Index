#pragma once
#include <atomic>
#include <cassert>
#include <functional>
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
            // assert() is removed in release builds, which would let the
            // documented self-deadlock pattern silently hang the process.
            // Use an unconditional check + throw so the bug surfaces on the
            // calling stack instead of stalling.
            ThrowIfDispatchingThisEvent("Event::Add");
            std::unique_lock lock(m_Mutex);
            const EventId id = EventId(++m_NextId.value);
            m_Listeners.push_back({ id, std::move(cb) });
            return id;
        }

        // Remove blocks until any in-flight Invoke on another thread has completed, so a
        // subscriber's destructor can safely call Remove and trust that no further
        // callbacks will fire against `this`. Without this, the previous snapshot-based
        // dispatch could call into a destroyed subscriber (the std::function copy in
        // the snapshot kept the captured `this` past Remove). Use a shared_mutex:
        // Invoke takes a shared lock so multiple dispatchers can run concurrently;
        // Remove takes an exclusive lock so it waits for all dispatchers to release.
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
            // Hold the shared lock across dispatch so a concurrent Remove (which takes
            // the unique lock) blocks until we're done. Callers therefore must NOT
            // Add/Remove/Clear from inside their own callback — that would deadlock.
            // Add/Remove/Clear call ThrowIfDispatchingThisEvent first, so the misuse
            // throws std::logic_error from the offending call site in every build
            // configuration instead of stalling a release process.
            std::shared_lock lock(m_Mutex);
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

        // Per-thread, per-instance dispatch depth. A single shared
        // atomic<thread::id> couldn't track multiple threads dispatching the
        // same Event concurrently (Invoke's shared_lock permits that) —
        // saving/restoring one id left a stale id under interleaving and caused
        // spurious throws from Add/Remove/Clear. A thread_local depth map is
        // correct under concurrency, recursion, and throwing listeners.
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
