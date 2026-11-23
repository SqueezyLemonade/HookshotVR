// (OoT VR, research logs) "Log once per key, for at most Cap keys", at no cost past the cap or for a key this thread has
// already passed. The research lines of true stereo (a program that samples a two-layer texture, a routed program, a small
// draw moved for each eye, a camera read) each kept a locked set of the keys logged, and took its lock for every draw, also
// long after the set was full: a lock a draw, thousands a frame, for nothing.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_set>

namespace mkwvk::vr {

/// first(key) answers what `seen.size() < Cap && seen.insert(key).second` under the log's lock answered, so the same lines
/// are logged. closed(key) says, without the lock, that first(key) would answer false: the cap is reached, or this thread
/// has already put this key to the shared set. A caller that would do work only to log (a walk, a memory read) asks
/// closed() first and skips it
template <size_t Cap>
class OnceLog {
public:
    bool closed(uint64_t key) const {
        return full_.load(std::memory_order_relaxed) || passed().count(tag(key)) != 0;
    }
    bool first(uint64_t key) {
        if (closed(key)) return false;
        std::lock_guard lk(mu_);
        const bool fresh = seen_.size() < Cap && seen_.insert(key).second;
        if (seen_.size() >= Cap) full_.store(true, std::memory_order_relaxed);
        passed().insert(tag(key));   // (in the shared set now, or the set full: this thread never needs the lock for it again)
        return fresh;
    }

private:
    // (one set of passed keys a thread for all the logs of this Cap: each log's keys told apart by its own address)
    uint64_t tag(uint64_t key) const { return key ^ ((uint64_t)(uintptr_t)this * 0x9E3779B97F4A7C15ull); }
    static std::unordered_set<uint64_t>& passed() {
        thread_local std::unordered_set<uint64_t> keys;   // (at most Cap a log: a key is only added while the set has room)
        return keys;
    }
    std::mutex mu_;
    std::unordered_set<uint64_t> seen_;
    std::atomic<bool> full_{false};
};

}  // namespace mkwvk::vr
