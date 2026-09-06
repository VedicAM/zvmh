#ifndef SYSTEM_REGISTRY_H
#define SYSTEM_REGISTRY_H

#include "systemcontext.h"
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

// SystemContextRegistry: scoped registration of context sources. register_context
// returns a move-only RAII handle that removes the entry on destruction, mirroring
// the Effect acquireRelease/Scope semantics. Duplicate keys are rejected.

namespace sysctx {

class RegistrationHandle {
public:
    explicit RegistrationHandle(std::function<void()> cleanup)
        : cleanup_(std::move(cleanup)) {}

    ~RegistrationHandle() {
        if (cleanup_) cleanup_();
    }

    RegistrationHandle(const RegistrationHandle&) = delete;
    RegistrationHandle& operator=(const RegistrationHandle&) = delete;
    RegistrationHandle(RegistrationHandle&& other) noexcept
        : cleanup_(std::move(other.cleanup_)) {
        other.cleanup_ = nullptr;
    }
    RegistrationHandle& operator=(RegistrationHandle&& other) noexcept {
        if (this != &other) {
            if (cleanup_) cleanup_();
            cleanup_ = std::move(other.cleanup_);
            other.cleanup_ = nullptr;
        }
        return *this;
    }

private:
    std::function<void()> cleanup_;
};

class SystemContextRegistry {
public:
    using Loader = std::function<SystemContext()>;

    RegistrationHandle register_context(std::string key, Loader loader) {
        Key k(key);
        if (std::any_of(entries_.begin(), entries_.end(),
                        [&](const Entry& e) { return e.key == key; })) {
            throw std::runtime_error("Duplicate system context entry key: " + key);
        }
        entries_.push_back({std::move(key), std::move(loader)});
        const std::string registered_key = entries_.back().key;
        return RegistrationHandle([this, registered_key]() {
            entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                [&](const Entry& e) { return e.key == registered_key; }),
                entries_.end());
        });
    }

    SystemContext load() const {
        std::vector<Entry> sorted = entries_;
        std::sort(sorted.begin(), sorted.end(),
                  [](const Entry& a, const Entry& b) { return a.key < b.key; });
        std::vector<SystemContext> contexts;
        contexts.reserve(sorted.size());
        for (const auto& entry : sorted) contexts.push_back(entry.loader());
        return combine(std::move(contexts));
    }

private:
    struct Entry {
        std::string key;
        Loader loader;
    };
    std::vector<Entry> entries_;
};

}  // namespace sysctx

#endif