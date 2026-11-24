#pragma once
#include <atomic>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace vanta {

class ConfigManager {
public:
    static ConfigManager& get();

    void init(std::filesystem::path baseDir);
    void loadProfile(const std::string& profile);
    bool saveProfile(const std::string& profile) const;
    bool deleteProfile(const std::string& profile);
    bool renameProfile(const std::string& from, const std::string& to);
    static bool validName(const std::string& name);
    std::string shareProfile(const std::string& profile) const;
    std::string importProfile(const std::string& code);
    std::vector<std::string> listProfiles() const;

    const std::string&           currentProfile() const noexcept { return profile_; }
    const std::filesystem::path& baseDir()       const noexcept { return base_; }

    // Called by Setting::draw or widgets on user changes. Coalesced by the autosave thread.
    void markDirty() noexcept;
    // Called on the render/UI thread: workers never read modules or GUI state.
    void pumpAutosave();
    void shutdown();

    bool isLoading() const noexcept { return loading_.load(std::memory_order_relaxed); }
    bool isInitialized() const noexcept { return initialized_.load(std::memory_order_relaxed); }

private:
    std::filesystem::path base_;
    std::string           profile_ = "default";
    std::atomic<bool>     loading_{false};
    std::atomic<bool>     initialized_{false};
    mutable std::mutex    saveMx_;
    mutable std::uint64_t nextWrite_ = 0; // clientStateMutex
    mutable std::unordered_map<std::string, std::uint64_t> committed_; // saveMx_
    bool writeSnapshot(const std::filesystem::path& base, const std::string& profile,
                       const nlohmann::json& snapshot, std::uint64_t sequence) const;
    struct Impl;
    Impl*                 impl_ = nullptr;
};

} // namespace vanta
