#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <optional>
#include <mutex>
#include <functional>
#include <chrono>

// Convenience aliases
using Clock = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;

// ---------------------------------------------------------------------------
// RoutingEntry
//   calsing    - std::string               : call sign / node identifier
//   tg         - int                       : talk group
//   tg_monitor - optional vector<int>      : monitored talk groups (optional)
//   trunk      - std::string               : trunk address
//   expires_at - optional TimePoint        : nullopt = immortal (no TTL)
// ---------------------------------------------------------------------------
struct RoutingEntry {
    std::string                      calsing;
    int                              tg;
    std::optional<std::vector<int>>  tg_monitor;   // optional
    std::string                      trunk;
    std::optional<TimePoint>         expires_at;   // nullopt = immortal

    // Without tg_monitor, without TTL
    RoutingEntry(std::string calsing_, int tg_, std::string trunk_)
        : calsing(std::move(calsing_))
        , tg(tg_)
        , tg_monitor(std::nullopt)
        , trunk(std::move(trunk_))
        , expires_at(std::nullopt)
    {
    }

    // With tg_monitor, without TTL
    RoutingEntry(std::string calsing_, int tg_,
        std::vector<int> tg_monitor_, std::string trunk_)
        : calsing(std::move(calsing_))
        , tg(tg_)
        , tg_monitor(std::move(tg_monitor_))
        , trunk(std::move(trunk_))
        , expires_at(std::nullopt)
    {
    }

    // -----------------------------------------------------------------------
    // TTL helpers
    // -----------------------------------------------------------------------

    bool has_ttl() const {
        return expires_at.has_value();
    }

    bool is_expired() const {
        return has_ttl() && Clock::now() >= *expires_at;
    }

    // Set (or reset) the TTL from now.
    void set_ttl(std::chrono::seconds s) {
        expires_at = Clock::now() + s;
    }

    // Alias for set_ttl â€“ call on heartbeat / re-registration.
    void refresh_ttl(std::chrono::seconds s) {
        set_ttl(s);
    }

    // Make this entry immortal (remove TTL).
    void clear_ttl() {
        expires_at = std::nullopt;
    }
};

// ---------------------------------------------------------------------------
// TrunkPacket â€“ what you receive from the trunk.
// Extend with whatever fields your protocol carries.
// ---------------------------------------------------------------------------
struct TrunkPacket {
    std::string trunk;   // source trunk address
    int         tg;      // talk group reported by the packet
    // add signal strength, timestamp, flags, ... here
};

// ---------------------------------------------------------------------------
// RoutingTable
//
//   Primary index  : calsing -> RoutingEntry
//   Secondary index: trunk   -> set<calsing>   (kept in sync automatically)
//
//   All public methods are thread-safe.
//   Multiple readers run concurrently; writers hold an exclusive lock.
// ---------------------------------------------------------------------------
class RoutingTable {
public:

    // -----------------------------------------------------------------------
    // Insert or overwrite an entry.
    // Both indexes are updated atomically under one exclusive lock.
    // Returns true if a new entry was created, false if replaced.
    // -----------------------------------------------------------------------
    bool upsert(RoutingEntry entry) {
        std::unique_lock lock(mutex_);

        auto existing = table_.find(entry.calsing);
        if (existing != table_.end()) {
            remove_from_trunk_index(existing->second.trunk, entry.calsing);
        }

        const std::string trunk = entry.trunk;
        const std::string calsing = entry.calsing;
        bool inserted = table_.insert_or_assign(calsing, std::move(entry)).second;
        trunk_index_[trunk].insert(calsing);
        return inserted;
    }

    // -----------------------------------------------------------------------
    // Convenience upsert that also sets a TTL on the entry.
    // -----------------------------------------------------------------------
    bool upsert(RoutingEntry entry, std::chrono::seconds ttl) {
        entry.set_ttl(ttl);
        return upsert(std::move(entry));
    }

    // -----------------------------------------------------------------------
    // Remove an entry by calsing.
    // Returns true if found and removed.
    // -----------------------------------------------------------------------
    bool remove(const std::string& calsing) {
        std::unique_lock lock(mutex_);
        auto it = table_.find(calsing);
        if (it == table_.end()) return false;
        remove_from_trunk_index(it->second.trunk, calsing);
        table_.erase(it);
        return true;
    }

    // -----------------------------------------------------------------------
    // Remove all entries registered under a given trunk address.
    // Returns the number of entries removed.
    // -----------------------------------------------------------------------
    std::size_t remove_by_trunk(const std::string& trunk) {
        std::unique_lock lock(mutex_);

        auto idx_it = trunk_index_.find(trunk);
        if (idx_it == trunk_index_.end()) return 0;

        std::size_t count = 0;
        auto callsigns = idx_it->second;   // copy to avoid iterator invalidation

        for (const auto& calsing : callsigns) {
            auto it = table_.find(calsing);
            if (it == table_.end()) continue;
            remove_from_trunk_index(it->second.trunk, calsing);
            table_.erase(it);
            ++count;
        }
        return count;
    }

    // -----------------------------------------------------------------------
    // TTL: reap all expired entries.
    // Call this from your periodic timer (e.g. every 10 s).
    // Returns the number of entries removed.
    // -----------------------------------------------------------------------
    std::size_t remove_expired() {
        std::unique_lock lock(mutex_);
        std::size_t count = 0;
        const auto  now = Clock::now();

        for (auto it = table_.begin(); it != table_.end(); ) {
            const auto& e = it->second;
            if (e.has_ttl() && now >= *e.expires_at) {
                remove_from_trunk_index(e.trunk, e.calsing);
                it = table_.erase(it);
                ++count;
            }
            else {
                ++it;
            }
        }
        return count;
    }

    // -----------------------------------------------------------------------
    // TTL: refresh (extend) the TTL of a single entry by calsing.
    // Call this when a heartbeat / keep-alive arrives for a node.
    // Returns true if the entry was found and updated.
    // -----------------------------------------------------------------------
    bool refresh_ttl(const std::string& calsing, std::chrono::seconds ttl) {
        return update(calsing, [ttl](RoutingEntry& e) {
            e.refresh_ttl(ttl);
            });
    }

    // -----------------------------------------------------------------------
    // TTL: refresh all entries registered under a given trunk address.
    // Useful when the trunk sends a single keep-alive for all its nodes.
    // Returns the number of entries updated.
    // -----------------------------------------------------------------------
    std::size_t refresh_ttl_by_trunk(const std::string& trunk,
        std::chrono::seconds ttl) {
        std::unique_lock lock(mutex_);

        auto idx_it = trunk_index_.find(trunk);
        if (idx_it == trunk_index_.end()) return 0;

        std::size_t count = 0;
        const auto  deadline = Clock::now() + ttl;

        for (const auto& calsing : idx_it->second) {
            auto entry_it = table_.find(calsing);
            if (entry_it == table_.end()) continue;
            entry_it->second.expires_at = deadline;
            ++count;
        }
        return count;
    }

    // -----------------------------------------------------------------------
    // Look up a single entry by calsing (copy returned for thread safety).
    // Returns nullopt if not found OR if the entry has already expired.
    // -----------------------------------------------------------------------
    std::optional<RoutingEntry> find(const std::string& calsing) const {
        std::shared_lock lock(mutex_);
        auto it = table_.find(calsing);
        if (it == table_.end())       return std::nullopt;
        if (it->second.is_expired())  return std::nullopt;
        return it->second;
    }

    // -----------------------------------------------------------------------
    // Update one entry in-place by calsing.
    //   fn : void(RoutingEntry&)
    //
    // NOTE: if your functor changes entry.trunk, call upsert() instead so
    //       the trunk index stays consistent.
    // Returns true if found and updated.
    // -----------------------------------------------------------------------
    template<typename Fn>
    bool update(const std::string& calsing, Fn&& fn) {
        std::unique_lock lock(mutex_);
        auto it = table_.find(calsing);
        if (it == table_.end()) return false;
        std::forward<Fn>(fn)(it->second);
        return true;
    }

    // -----------------------------------------------------------------------
    // Called when a packet arrives from a trunk node.
    //   fn : void(RoutingEntry&, const TrunkPacket&)
    //   Returns the number of entries updated.
    // -----------------------------------------------------------------------
    template<typename Fn>
    std::size_t update_by_trunk(const TrunkPacket& packet, Fn&& fn) {
        std::unique_lock lock(mutex_);

        auto idx_it = trunk_index_.find(packet.trunk);
        if (idx_it == trunk_index_.end()) return 0;

        std::size_t count = 0;
        for (const std::string& calsing : idx_it->second) {
            auto entry_it = table_.find(calsing);
            if (entry_it == table_.end()) continue;
            std::forward<Fn>(fn)(entry_it->second, packet);
            ++count;
        }
        return count;
    }

    // -----------------------------------------------------------------------
    // Read-only snapshot of all entries for a given trunk address.
    // Expired entries are excluded.
    // -----------------------------------------------------------------------
    std::vector<RoutingEntry> find_by_trunk(const std::string& trunk) const {
        std::shared_lock lock(mutex_);

        std::vector<RoutingEntry> out;
        auto idx_it = trunk_index_.find(trunk);
        if (idx_it == trunk_index_.end()) return out;

        out.reserve(idx_it->second.size());
        for (const std::string& calsing : idx_it->second) {
            auto entry_it = table_.find(calsing);
            if (entry_it != table_.end() && !entry_it->second.is_expired())
                out.push_back(entry_it->second);
        }
        return out;
    }

    // -----------------------------------------------------------------------
    // Full snapshot of every non-expired entry.
    // -----------------------------------------------------------------------
    std::vector<RoutingEntry> snapshot() const {
        std::shared_lock lock(mutex_);
        std::vector<RoutingEntry> out;
        out.reserve(table_.size());
        for (const auto& [key, val] : table_)
            if (!val.is_expired())
                out.push_back(val);
        return out;
    }

    // -----------------------------------------------------------------------
    // Read-only visitor over all non-expired entries.
    //   fn : void(const RoutingEntry&)
    // Keep fn lightweight â€“ it runs while holding the shared lock.
    // -----------------------------------------------------------------------
    template<typename Fn>
    void for_each(Fn&& fn) const {
        std::shared_lock lock(mutex_);
        for (const auto& [key, val] : table_)
            if (!val.is_expired())
                std::forward<Fn>(fn)(val);
    }

    // Returns count of live (non-expired) entries.
    std::size_t size() const {
        std::shared_lock lock(mutex_);
        std::size_t n = 0;
        for (const auto& [key, val] : table_)
            if (!val.is_expired()) ++n;
        return n;
    }

    void clear() {
        std::unique_lock lock(mutex_);
        table_.clear();
        trunk_index_.clear();
    }

private:
    void remove_from_trunk_index(const std::string& trunk,
        const std::string& calsing) {
        auto it = trunk_index_.find(trunk);
        if (it == trunk_index_.end()) return;
        it->second.erase(calsing);
        if (it->second.empty()) trunk_index_.erase(it);
    }

    mutable std::shared_mutex mutex_;

    std::unordered_map<std::string, RoutingEntry>                        table_;
    std::unordered_map<std::string, std::unordered_set<std::string>>     trunk_index_;
};
