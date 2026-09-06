#ifndef SYSTEMCONTEXT_H
#define SYSTEMCONTEXT_H

#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>
#include <nlohmann/json.hpp>

// SystemContext: independently refreshable typed context sources.
//
// A Source<A> describes how to observe, compare, and render one value. make()
// closes it over A into an opaque SystemContext that composes uniformly with
// contexts built from other value types. initialize() builds the immutable
// baseline + durable snapshot; reconcile() compares the current generation
// against a previous snapshot, producing an update text; replace() rebuilds a
// complete generation. Returning Unavailable means observation failed
// temporarily: initialization blocks on it, replacement preserves the admitted
// snapshot.

namespace sysctx {

using Value = nlohmann::json;

struct Unavailable {};

class Key {
public:
    static bool is_valid(const std::string& s) {
        static const std::regex re(R"(^[a-z0-9][a-z0-9._-]*\/[a-z0-9][a-z0-9._/-]*$)");
        std::smatch m;
        return std::regex_match(s, m, re);
    }

    Key() = default;
    explicit Key(std::string value) : value_(std::move(value)) {
        if (!is_valid(value_)) throw std::invalid_argument("Invalid system context key: " + value_);
    }

    const std::string& str() const { return value_; }
    bool operator<(const Key& other) const { return value_ < other.value_; }
    bool operator==(const Key& other) const { return value_ == other.value_; }

private:
    std::string value_;
};

class InitializationBlocked {
public:
    explicit InitializationBlocked(std::vector<Key> keys) : keys_(std::move(keys)) {}
    const std::vector<Key>& keys() const { return keys_; }
    std::string message() const {
        if (keys_.empty()) return "System context initialization blocked by unavailable sources";
        std::string keys;
        for (size_t i = 0; i < keys_.size(); ++i) {
            if (i) keys += ", ";
            keys += keys_[i].str();
        }
        return "System context initialization blocked by unavailable sources: " + keys;
    }

private:
    std::vector<Key> keys_;
};

struct SourceSnapshot {
    Value value;
    std::optional<std::string> removed;
};

using Snapshot = std::map<std::string, SourceSnapshot>;

struct Generation {
    std::string baseline;
    Snapshot snapshot;
};

template<typename A>
struct Source {
    Key key;
    std::function<std::variant<Unavailable, A>()> load;
    std::function<std::string(const A&)> baseline;
    std::function<std::string(const A&, const A&)> update;
    std::function<std::optional<std::string>(const A&)> removed = {};
    std::function<Value(const A&)> to_json = [](const A& v) { return Value(v); };
    std::function<std::optional<A>(const Value&)> from_json = [](const Value& v) {
        try {
            return std::optional<A>(v.get<A>());
        } catch (...) {
            return std::optional<A>();
        }
    };
    std::function<bool(const A&, const A&)> equivalent = [](const A& a, const A& b) { return a == b; };
};

struct ReconcileResult {
    enum class Tag { Unchanged, Updated, ReplacementReady, ReplacementBlocked, Replace };
    Tag tag = Tag::Unchanged;
    std::string text;
    Snapshot snapshot;
    std::optional<Generation> generation;
};

struct ReplacementResult {
    enum class Tag { Ready, Blocked };
    Tag tag = Tag::Blocked;
    std::optional<Generation> generation;
};

namespace detail {

struct Rendered {
    std::string text;
    SourceSnapshot snapshot;
};

enum class ComparedTag { Incompatible, Unchanged, Updated };

struct Compared {
    ComparedTag tag = ComparedTag::Unchanged;
    std::function<Rendered()> render;
};

class Loaded {
public:
    Loaded() = default;
    Loaded(std::function<Rendered()> baseline, std::function<Compared(const Value&)> compare)
        : baseline_(std::move(baseline)), compare_(std::move(compare)) {}

    Rendered baseline() const { return baseline_(); }
    Compared compare(const Value& previous) const { return compare_(previous); }

private:
    std::function<Rendered()> baseline_;
    std::function<Compared(const Value&)> compare_;
};

using LoadResult = std::variant<Unavailable, Loaded>;

class PackedSource {
public:
    PackedSource(Key key, std::function<LoadResult()> load)
        : key_(std::move(key)), load_(std::move(load)) {}

    const Key& key() const { return key_; }
    LoadResult load() const { return load_(); }

private:
    Key key_;
    std::function<LoadResult()> load_;
};

struct ObservationEntry {
    enum class Tag { Available, Unavailable };
    Tag tag;
    Key key;
    std::optional<Loaded> loaded;
};

}  // namespace detail

class SystemContext {
public:
    SystemContext() = default;
    explicit SystemContext(std::vector<detail::PackedSource> sources)
        : sources_(std::move(sources)) {}

    std::vector<detail::PackedSource>& sources() { return sources_; }
    const std::vector<detail::PackedSource>& sources() const { return sources_; }

private:
    std::vector<detail::PackedSource> sources_;
};

inline SystemContext empty_context() {
    return SystemContext();
}

template<typename A>
SystemContext make(const Source<A>& source) {
    auto to_json = source.to_json;
    auto from_json = source.from_json;
    auto equivalent = source.equivalent;

    detail::PackedSource packed(
        source.key,
        [source, to_json, from_json, equivalent]() -> detail::LoadResult {
            auto result = source.load();
            if (std::holds_alternative<Unavailable>(result)) {
                return std::get<Unavailable>(result);
            }
            A value = std::get<A>(result);
            const Key key = source.key;
            const Source<A> src = source;

            auto make_snapshot = [to_json, src, value]() -> SourceSnapshot {
                SourceSnapshot snap;
                snap.value = to_json(value);
                if (src.removed) snap.removed = src.removed(value);
                return snap;
            };

            return detail::Loaded(
                [src, key, value, make_snapshot]() -> detail::Rendered {
                    detail::Rendered r;
                    r.text = require_text(key, "baseline", src.baseline(value));
                    r.snapshot = make_snapshot();
                    return r;
                },
                [src, key, from_json, equivalent, value, make_snapshot](const Value& previous) -> detail::Compared {
                    auto decoded = from_json(previous);
                    if (!decoded) return {detail::ComparedTag::Incompatible, {}};
                    if (equivalent(decoded.value(), value)) return {detail::ComparedTag::Unchanged, {}};
                    detail::Compared c;
                    c.tag = detail::ComparedTag::Updated;
                    c.render = [src, key, value, decoded_value = *decoded, make_snapshot]() mutable -> detail::Rendered {
                        detail::Rendered r;
                        r.text = require_text(key, "update", src.update(decoded_value, value));
                        r.snapshot = make_snapshot();
                        return r;
                    };
                    return c;
                });
        });

    return SystemContext(std::vector<detail::PackedSource>{std::move(packed)});
}

inline SystemContext combine(std::vector<SystemContext> values) {
    std::vector<detail::PackedSource> sources;
    std::set<std::string> seen;
    for (auto& value : values) {
        for (auto& src : value.sources()) {
            const std::string key = src.key().str();
            if (!seen.insert(key).second) {
                throw std::runtime_error("Duplicate system context key: " + key);
            }
            sources.push_back(std::move(src));
        }
    }
    return SystemContext(std::move(sources));
}

inline std::string render(const std::vector<std::string>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += "\n\n";
        out += parts[i];
    }
    return out;
}

inline std::string require_text(const Key& key, const char* kind, std::string text) {
    if (text.empty()) {
        throw std::runtime_error("System context source " + key.str() + " rendered an empty " + kind);
    }
    return text;
}

inline std::vector<detail::ObservationEntry> observe(const SystemContext& value) {
    std::vector<detail::ObservationEntry> entries;
    entries.reserve(value.sources().size());
    for (const auto& source : value.sources()) {
        auto result = source.load();
        if (std::holds_alternative<Unavailable>(result)) {
            entries.push_back({detail::ObservationEntry::Tag::Unavailable, source.key(), std::nullopt});
        } else {
            entries.push_back({detail::ObservationEntry::Tag::Available, source.key(),
                               std::optional<detail::Loaded>(std::move(std::get<detail::Loaded>(result)))});
        }
    }
    return entries;
}

inline Generation initialize_observation(const std::vector<detail::ObservationEntry>& entries) {
    std::vector<std::string> parts;
    Snapshot snapshot;
    for (const auto& entry : entries) {
        if (entry.tag != detail::ObservationEntry::Tag::Available || !entry.loaded) continue;
        auto rendered = entry.loaded->baseline();
        parts.push_back(rendered.text);
        snapshot[entry.key.str()] = rendered.snapshot;
    }
    return {render(parts), std::move(snapshot)};
}

inline std::variant<Generation, InitializationBlocked> initialize(const SystemContext& value) {
    auto entries = observe(value);
    std::vector<Key> unavailable;
    for (const auto& entry : entries) {
        if (entry.tag == detail::ObservationEntry::Tag::Unavailable) unavailable.push_back(entry.key);
    }
    if (!unavailable.empty()) return InitializationBlocked(std::move(unavailable));
    return initialize_observation(entries);
}

inline detail::ObservationEntry::Tag entry_tag(const detail::ObservationEntry& entry) {
    return entry.tag;
}

inline ReconcileResult reconcile_observation(const std::vector<detail::ObservationEntry>& entries,
                                             const Snapshot& previous) {
    std::set<std::string> keys;
    std::map<std::string, detail::Compared> comparisons;

    for (const auto& entry : entries) {
        keys.insert(entry.key.str());
        if (entry_tag(entry) != detail::ObservationEntry::Tag::Available || !entry.loaded) continue;
        auto stored = previous.find(entry.key.str());
        if (stored == previous.end()) continue;
        auto compared = entry.loaded->compare(stored->second.value);
        if (compared.tag == detail::ComparedTag::Incompatible) {
            ReconcileResult r;
            r.tag = ReconcileResult::Tag::Replace;
            return r;
        }
        comparisons[entry.key.str()] = compared;
    }

    for (const auto& [key, snap] : previous) {
        if (keys.count(key)) continue;
        if (!snap.removed) {
            ReconcileResult r;
            r.tag = ReconcileResult::Tag::Replace;
            return r;
        }
    }

    Snapshot snapshot;
    std::vector<std::string> updates;
    for (const auto& entry : entries) {
        const std::string key = entry.key.str();
        auto stored = previous.find(key);
        if (entry_tag(entry) != detail::ObservationEntry::Tag::Available) {
            if (stored != previous.end()) snapshot[key] = stored->second;
            continue;
        }
        if (stored == previous.end()) {
            auto rendered = entry.loaded->baseline();
            updates.push_back(rendered.text);
            snapshot[key] = rendered.snapshot;
            continue;
        }
        auto cit = comparisons.find(key);
        if (cit == comparisons.end() || cit->second.tag == detail::ComparedTag::Unchanged) {
            snapshot[key] = stored->second;
            continue;
        }
        auto rendered = cit->second.render();
        updates.push_back(rendered.text);
        snapshot[key] = rendered.snapshot;
    }

    for (const auto& [key, snap] : previous) {
        if (keys.count(key)) continue;
        updates.push_back(snap.removed.value());
    }

    ReconcileResult r;
    if (updates.empty()) {
        r.tag = ReconcileResult::Tag::Unchanged;
        r.snapshot = std::move(snapshot);
        return r;
    }
    r.tag = ReconcileResult::Tag::Updated;
    r.text = render(updates);
    r.snapshot = std::move(snapshot);
    return r;
}

inline ReplacementResult replace_observation(const std::vector<detail::ObservationEntry>& entries,
                                             const Snapshot& previous) {
    for (const auto& entry : entries) {
        if (entry_tag(entry) == detail::ObservationEntry::Tag::Unavailable &&
            previous.count(entry.key.str())) {
            return {ReplacementResult::Tag::Blocked, std::nullopt};
        }
    }
    return {ReplacementResult::Tag::Ready, initialize_observation(entries)};
}

inline ReconcileResult reconcile(const SystemContext& value, const Snapshot& previous) {
    auto entries = observe(value);
    auto obs = reconcile_observation(entries, previous);
    if (obs.tag != ReconcileResult::Tag::Replace) return obs;
    auto rep = replace_observation(entries, previous);
    if (rep.tag == ReplacementResult::Tag::Ready) {
        return {ReconcileResult::Tag::ReplacementReady, "", {}, std::move(rep.generation)};
    }
    return {ReconcileResult::Tag::ReplacementBlocked, "", {}, std::nullopt};
}

inline ReplacementResult replace(const SystemContext& value, const Snapshot& previous) {
    return replace_observation(observe(value), previous);
}

}  // namespace sysctx

#endif