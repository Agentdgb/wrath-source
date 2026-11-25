#include "mappings/MappingRegistry.h"

#include "mappings/generated/GenTable.h"
#include "mappings/generated/VersionIndex.h"
#include "util/Hash.h"
#include "util/Log.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace dt::mappings {

// ── singleton ────────────────────────────────────────────────────────────────
MappingRegistry& MappingRegistry::get() {
    static MappingRegistry g;
    return g;
}

// ── init ─────────────────────────────────────────────────────────────────────
void MappingRegistry::init(std::string_view mcVersion) {
    mcVer_.assign(mcVersion);
    const gen::VersionTable* t = gen::findVersionTable(mcVersion);
    if (!t) {
        t = gen::defaultVersionTable();
        VLOG_WARN("map",
            "No mapping table for MC {} — defaulting to {} (shipped set may be stale)",
            mcVersion, t ? t->mcVersion : std::string_view{"<none>"});
    }
    table_ = t;
    if (table_) {
        VLOG_INFO("map",
            "MappingRegistry bound to {} ({} classes, {} methods, {} fields, {} reverse)",
            table_->mcVersion,
            table_->classCount, table_->methodCount, table_->fieldCount, table_->reverseClassCount);
    } else {
        VLOG_ERR("map", "MappingRegistry has no table bound — Java hooks will not resolve");
    }
}

// ── binary-search helpers ────────────────────────────────────────────────────
namespace {

const gen::ClassEntry* findClass(const gen::VersionTable* t, std::uint64_t h) noexcept {
    if (!t || t->classCount == 0) return nullptr;
    auto* first = t->classes;
    auto* last  = t->classes + t->classCount;
    auto* it = std::lower_bound(first, last, h,
        [](const gen::ClassEntry& e, std::uint64_t v){ return e.canonHash < v; });
    return (it != last && it->canonHash == h) ? it : nullptr;
}

const gen::MemberEntry* findMember(const gen::MemberEntry* arr, std::size_t n,
                                   std::uint64_t ownerH, std::uint64_t nameH) noexcept {
    if (!arr || n == 0) return nullptr;
    auto* first = arr;
    auto* last  = arr + n;
    // Arrays are sorted primarily by ownerHash, secondarily by nameHash.
    auto* it = std::lower_bound(first, last, ownerH,
        [](const gen::MemberEntry& e, std::uint64_t v){ return e.ownerHash < v; });
    while (it != last && it->ownerHash == ownerH) {
        if (it->nameHash == nameH) return it;
        ++it;
    }
    return nullptr;
}

const gen::ReverseClassEntry* findReverse(const gen::VersionTable* t, std::uint64_t h) noexcept {
    if (!t || t->reverseClassCount == 0) return nullptr;
    auto* first = t->reverseClasses;
    auto* last  = t->reverseClasses + t->reverseClassCount;
    auto* it = std::lower_bound(first, last, h,
        [](const gen::ReverseClassEntry& e, std::uint64_t v){ return e.runtimeHash < v; });
    return (it != last && it->runtimeHash == h) ? it : nullptr;
}

} // namespace

// ── lookups ──────────────────────────────────────────────────────────────────
std::string_view MappingRegistry::classFor(std::string_view canon) const noexcept {
    auto* e = findClass(table_, fnv1a64(canon));
    return e ? e->runtime : std::string_view{};
}

MethodRef MappingRegistry::methodFor(std::string_view owner, std::string_view name) const noexcept {
    auto* e = findMember(table_ ? table_->methods : nullptr,
                         table_ ? table_->methodCount : 0,
                         fnv1a64(owner), fnv1a64(name));
    return e ? MethodRef{e->name, e->desc} : MethodRef{};
}

FieldRef MappingRegistry::fieldFor(std::string_view owner, std::string_view name) const noexcept {
    auto* e = findMember(table_ ? table_->fields : nullptr,
                         table_ ? table_->fieldCount : 0,
                         fnv1a64(owner), fnv1a64(name));
    return e ? FieldRef{e->name, e->desc} : FieldRef{};
}

std::string_view MappingRegistry::canonForRuntimeClass(std::string_view runtimeName) const noexcept {
    auto* e = findReverse(table_, fnv1a64(runtimeName));
    return e ? e->canon : std::string_view{};
}

std::string MappingRegistry::remapDescriptor(std::string_view canonDesc) const {
    std::string out;
    out.reserve(canonDesc.size() + 8);
    for (std::size_t i = 0; i < canonDesc.size(); ++i) {
        char c = canonDesc[i];
        if (c != 'L') { out.push_back(c); continue; }
        auto end = canonDesc.find(';', i + 1);
        if (end == std::string_view::npos) { out.push_back(c); continue; }
        auto canonName = canonDesc.substr(i + 1, end - i - 1);
        auto runtime   = classFor(canonName);
        out.push_back('L');
        if (runtime.empty()) out.append(canonName.begin(), canonName.end());
        else                 out.append(runtime.begin(),   runtime.end());
        out.push_back(';');
        i = end;
    }
    return out;
}

} // namespace dt::mappings
