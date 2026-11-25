// src/mappings/MappingRegistry.h  (post-pivot: Fabric-only, all-embedded)
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace dt::mappings {

// Runtime name reference returned by lookups. Both members point at string
// literals owned by the generated TU (.rodata) or by the caller's arena;
// never allocated. empty() distinguishes "not found" from "found empty".
struct MethodRef {
    std::string_view name;
    std::string_view desc;
    [[nodiscard]] bool empty() const noexcept { return name.empty(); }
};

struct FieldRef {
    std::string_view name;
    std::string_view desc;
    [[nodiscard]] bool empty() const noexcept { return name.empty(); }
};

// Forward-declared so this header doesn't need to pull GenTable.h into every
// consumer. Callers work with the registry API, not the raw table.
namespace gen { struct VersionTable; }

// Central runtime name resolver. Modules and JniHelper speak canonical short
// names (e.g. "MinecraftClient", "ClientPlayerEntity", "PlayerMoveC2SPacket").
// The registry translates them into Fabric intermediary runtime names for the
// running MC version.
//
// Post-pivot: Fabric-only. All data is embedded in dt_core.dll as sorted
// constexpr arrays in .rdata; init() picks the right table by MC version
// string. No disk I/O, no heap allocation, no ankerl::unordered_dense.
//
// If the running MC version is not in the shipped set, init() falls back to
// gen::defaultVersionTable() (currently 1.21.11) and logs a warning naming
// both the requested and defaulted versions.
class MappingRegistry {
public:
    static MappingRegistry& get();

    // Picks the table for that MC version. If mcVersion is not in the index
    // the default table is bound and a diagnostic is logged. Idempotent —
    // safe to call again after a version-detect re-run.
    void init(std::string_view mcVersion);

    [[nodiscard]] bool             ready()     const noexcept { return table_ != nullptr; }
    [[nodiscard]] std::string_view mcVersion() const noexcept { return mcVer_; }

    // Canonical class name -> runtime binary name in JNI slash form
    // (e.g. "MinecraftClient" -> "net/minecraft/class_310").
    // Returns empty view when not in the table.
    [[nodiscard]] std::string_view classFor(std::string_view canon) const noexcept;

    // Canonical (owner, member) -> {runtime name, pre-remapped desc}.
    [[nodiscard]] MethodRef methodFor(std::string_view ownerCanon,
                                      std::string_view methodCanon) const noexcept;
    [[nodiscard]] FieldRef  fieldFor (std::string_view ownerCanon,
                                      std::string_view fieldCanon)  const noexcept;

    // Reverse: runtime class name (as seen in ClassFileLoadHook) -> canonical.
    // Returns empty view when the class is not one we transform.
    [[nodiscard]] std::string_view canonForRuntimeClass(std::string_view runtimeName) const noexcept;

    // Slow-path helper: rewrite a JVM descriptor whose L...; segments carry
    // canonical class names into one whose L...; segments carry the runtime
    // intermediary names for the current version. Allocates.
    // NOTE: prefer the pre-baked descriptors on MethodRef / FieldRef; this
    // exists only for call sites that build a canonical descriptor at runtime.
    [[nodiscard]] std::string remapDescriptor(std::string_view canonDesc) const;

private:
    MappingRegistry() = default;
    ~MappingRegistry() = default;
    MappingRegistry(const MappingRegistry&) = delete;
    MappingRegistry& operator=(const MappingRegistry&) = delete;

    const gen::VersionTable* table_ = nullptr;
    std::string              mcVer_;
};

} // namespace dt::mappings
