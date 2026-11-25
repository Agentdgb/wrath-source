#include "core/config/ConfigManager.h"
#include "core/config/ProfileStorage.h"
#include "core/ClientState.h"
#include "core/config/ShareCode.h"
#include "core/module/ModuleManager.h"
#include "core/social/FriendList.h"
#include "util/Log.h"
#include "vgui/bridge.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <windows.h>

namespace dt {
namespace {
std::string profileKey(std::string name) {
    for (auto& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return name;
}
bool validProfile(const std::string& name) {
    auto stem = profileKey(name.substr(0, name.find('.')));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul"
        || (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt"))
            && stem[3] >= '1' && stem[3] <= '9')) return false;
    return !name.empty() && name.size() <= 128 && name.front() != ' ' && name != "." && name != ".."
        && name.back() != '.' && name.back() != ' '
        && name.find_first_of("/\\:<>\"|?*") == std::string::npos
        && std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; });
}

bool replaceFile(const std::filesystem::path& path, const std::string& data) {
    auto temporary = path;
    temporary += L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::size_t offset = 0;
    bool ok = true;
    while (offset < data.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(data.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(file, data.data() + offset, chunk, &written, nullptr) || !written) {
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    if (!CloseHandle(file)) ok = false;
    if (ok) ok = MoveFileExW(temporary.c_str(), path.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(temporary.c_str());
    return ok;
}

bool compatibleSettings(const nlohmann::json& value, const nlohmann::json& model) {
    if (model.is_object()) {
        if (!value.is_object()) return false;
        for (auto it = model.begin(); it != model.end(); ++it)
            if (value.contains(it.key()) && !compatibleSettings(value[it.key()], it.value())) return false;
        return true;
    }
    if (model.is_array()) {
        if (!value.is_array()) return false;
        if (!model.empty()) for (const auto& item : value)
            if (!compatibleSettings(item, model.front())) return false;
        return true;
    }
    if (model.is_boolean()) return value.is_boolean();
    if (model.is_string()) return value.is_string() && value.get_ref<const std::string&>().size() <= 4096;
    if (model.is_number()) {
        if (!value.is_number()) return false;
        const auto number = value.get<double>();
        if (!std::isfinite(number)) return false;
        if (model.is_number_unsigned()) return value.is_number_integer() && number >= 0
            && number <= (std::numeric_limits<unsigned>::max)();
        if (model.is_number_integer()) return value.is_number_integer()
            && number >= (std::numeric_limits<int>::min)() && number <= (std::numeric_limits<int>::max)();
        return std::abs(number) <= (std::numeric_limits<float>::max)();
    }
    return value.type() == model.type();
}

nlohmann::json captureSettings() {
    auto snapshot = ModuleManager::get().saveAll();
    snapshot["#owner"] = storage::profileOwnerToken();
    snapshot["#friends"] = FriendList::get().all();
    vgui::bridge::saveGuiState(snapshot);
    return snapshot;
}

void validateSettings(const nlohmann::json& settings, const nlohmann::json& model) {
    if (!settings.is_object() || !compatibleSettings(settings, model))
        throw std::runtime_error("Invalid setting values in profile");
    for (const auto& module : ModuleManager::get().all()) {
        const auto found = settings.find(std::string(module->name()));
        if (found == settings.end()) continue;
        if (!found->is_object()
            || (found->contains("enabled") && !(*found)["enabled"].is_boolean())
            || (found->contains("settings") && !(*found)["settings"].is_object()))
            throw std::runtime_error("Invalid module settings in profile");
    }
    for (const auto* key : {"#theme", "#client", "#hud"})
        if (settings.contains(key) && !settings[key].is_object())
            throw std::runtime_error("Invalid interface settings in profile");
    if (settings.contains("#friends")) {
        if (!settings["#friends"].is_array()) throw std::runtime_error("Invalid friends list");
        for (const auto& name : settings["#friends"])
            if (!name.is_string() || name.get_ref<const std::string&>().size() > 128)
                throw std::runtime_error("Invalid friend name");
    }
}

void applySettings(const nlohmann::json& settings) {
    ModuleManager::get().loadAll(settings);
    std::vector<std::string> friends;
    if (auto it = settings.find("#friends"); it != settings.end())
        for (const auto& name : *it) friends.push_back(name.get<std::string>());
    FriendList::get().replaceAll(std::move(friends));
    vgui::bridge::loadGuiState(settings);
}
}

struct ConfigManager::Impl {
    struct Job {
        std::filesystem::path base;
        std::string profile;
        nlohmann::json snapshot;
        std::uint64_t sequence;
    };
    bool dirty = false; // clientStateMutex
    std::chrono::steady_clock::time_point changed;
    bool running = true; // mx
    std::mutex mx;
    std::condition_variable cv;
    std::deque<Job> pending;
    std::string writingProfile; // mx; includes snapshots currently outside the queue
    std::thread worker;
};

ConfigManager& ConfigManager::get() { static ConfigManager instance; return instance; }

void ConfigManager::init(std::filesystem::path baseDir) {
    std::lock_guard state(clientStateMutex());
    if (initialized_.load()) return;
    if (baseDir.empty() || !baseDir.is_absolute() || baseDir == baseDir.root_path()) {
        base_.clear();
        VLOG_ERR("cfg", "No safe profile storage directory available");
        return;
    }
    base_ = std::move(baseDir);
    std::error_code ec;
    std::filesystem::create_directories(base_ / "profiles", ec);
    if (ec) { base_.clear(); return; }
    std::ifstream last(base_ / "last_profile.txt");
    std::string name;
    if (std::getline(last, name)) {
        if (!name.empty() && name.back() == '\r') name.pop_back();
        if (validProfile(name)) profile_ = std::move(name);
    }
    impl_ = new Impl;
    try {
        impl_->worker = std::thread([this, worker = impl_] {
            for (;;) {
                std::unique_lock lock(worker->mx);
                worker->cv.wait(lock, [&] { return !worker->pending.empty() || !worker->running; });
                if (worker->pending.empty()) return;
                auto job = std::move(worker->pending.front());
                worker->pending.pop_front();
                worker->writingProfile = job.profile;
                lock.unlock();
                const bool saved = writeSnapshot(job.base, job.profile, job.snapshot, job.sequence);
                lock.lock();
                worker->writingProfile.clear();
                if (!saved) {
                    if (worker->running) {
                        // Keep the failed snapshot unless a newer one is already queued.
                        const auto newer = std::find_if(worker->pending.begin(), worker->pending.end(),
                            [&](const auto& queued) { return profileKey(queued.profile) == profileKey(job.profile); });
                        if (newer == worker->pending.end()) worker->pending.push_back(std::move(job));
                        worker->cv.wait_for(lock, std::chrono::seconds(1), [&] { return !worker->running; });
                    }
                }
            }
        });
    } catch (...) { delete impl_; impl_ = nullptr; throw; }
    initialized_.store(true);
}

void ConfigManager::loadProfile(const std::string& profile) {
    std::lock_guard state(clientStateMutex());
    if (base_.empty() || !initialized_.load()) return;
    if (!validProfile(profile)) { VLOG_ERR("cfg", "Invalid profile name"); return; }
    // Flush unsaved edits, including jobs already taken by the worker.
    bool needsFlush = impl_ && impl_->dirty;
    if (impl_) {
        std::lock_guard queue(impl_->mx);
        needsFlush = needsFlush || profileKey(impl_->writingProfile) == profileKey(profile_)
            || std::any_of(impl_->pending.begin(), impl_->pending.end(),
                [&](const auto& job) { return profileKey(job.profile) == profileKey(profile_); });
    }
    if (needsFlush && !saveProfile(profile_)) return;
    const std::string requested = profile;
    const auto path = base_ / "profiles" / (profile + ".json");
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) { VLOG_ERR("cfg", "Profile could not be accessed"); return; }
        if (saveProfile(requested)) {
            profile_ = requested;
            if (impl_) impl_->dirty = false;
        }
        return;
    }
    loading_.store(true);
    struct LoadingGuard { std::atomic<bool>& flag; ~LoadingGuard() { flag.store(false); } } guard{loading_};
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("Profile could not be opened");
        std::string raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::string decoded = storage::decodeProfileData(raw);
        nlohmann::json settings = nlohmann::json::parse(decoded, nullptr, false);
        if (!settings.is_object()) { VLOG_ERR("cfg", "Profile must contain an object"); return; }
        const auto previous = captureSettings();
        validateSettings(settings, previous);
        try {
            applySettings(settings);
        } catch (...) {
            // Restore values and enabled states if an application callback fails.
            try { applySettings(previous); }
            catch (...) { VLOG_ERR("cfg", "Profile rollback failed; client state requires attention"); }
            throw;
        }
        profile_ = requested;
        if (impl_) impl_->dirty = false;
        std::lock_guard io(saveMx_);
        replaceFile(base_ / "last_profile.txt", profile_);
    } catch ([[maybe_unused]] const std::exception& error) { VLOG_ERR("cfg", "Load failed: {}", error.what()); }
      catch (...) { VLOG_ERR("cfg", "Load failed with an unknown exception"); }
}

bool ConfigManager::writeSnapshot(const std::filesystem::path& base, const std::string& profile,
    const nlohmann::json& snapshot, std::uint64_t sequence) const {
    try {
        const auto serialized = snapshot.dump(2);
        const auto encoded = storage::encodeProfileData(serialized);
        std::lock_guard io(saveMx_);
        auto& committed = committed_[profileKey(profile)];
        if (sequence <= committed) return true;
        std::error_code ec;
        const auto directory = base / "profiles";
        std::filesystem::create_directories(directory, ec);
        if (ec || !replaceFile(directory / (profile + ".json"), encoded)) {
            VLOG_ERR("cfg", "Could not commit profile '{}'", profile);
            return false;
        }
        committed = sequence;
        return true;
    } catch ([[maybe_unused]] const std::exception& error) { VLOG_ERR("cfg", "Save failed: {}", error.what()); }
    return false;
}

bool ConfigManager::saveProfile(const std::string& profile) const {
    nlohmann::json snapshot;
    std::filesystem::path base;
    std::string name;
    std::uint64_t sequence;
    {
        std::lock_guard state(clientStateMutex());
        if (base_.empty() || !validProfile(profile)) return false;
        name = profile;
        base = base_;
        snapshot = captureSettings();
        sequence = ++nextWrite_;
    }
    if (!writeSnapshot(base, name, snapshot, sequence)) return false;
    std::lock_guard io(saveMx_);
    if (!replaceFile(base / "last_profile.txt", name)) {
        VLOG_ERR("cfg", "Could not save active profile");
        return false;
    }
    return true;
}

void ConfigManager::markDirty() noexcept {
    std::lock_guard state(clientStateMutex());
    if (!impl_ || !initialized_.load() || loading_.load()) return;
    impl_->dirty = true;
    impl_->changed = std::chrono::steady_clock::now();
}

void ConfigManager::pumpAutosave() {
    std::lock_guard state(clientStateMutex());
    if (!impl_ || !impl_->dirty || loading_.load()
        || std::chrono::steady_clock::now() - impl_->changed < std::chrono::milliseconds(400)) return;
    Impl::Job job{base_, profile_, captureSettings(), ++nextWrite_};
    {
        std::lock_guard queue(impl_->mx);
        if (!impl_->running) return;
        const auto previous = std::find_if(impl_->pending.begin(), impl_->pending.end(),
            [&](const auto& queued) { return queued.profile == job.profile; });
        if (previous != impl_->pending.end()) *previous = std::move(job);
        else impl_->pending.push_back(std::move(job));
    }
    impl_->dirty = false;
    impl_->cv.notify_one();
}

void ConfigManager::shutdown() {
    Impl* worker;
    {
        std::lock_guard state(clientStateMutex());
        worker = impl_;
        if (!worker) return;
        initialized_.store(false);
        impl_ = nullptr;
        std::lock_guard queue(worker->mx);
        worker->running = false;
    }
    worker->cv.notify_all();
    if (worker->worker.joinable()) worker->worker.join();
    delete worker;
    if (!ModuleManager::get().isPanicking()) saveProfile(profile_);
}

bool ConfigManager::deleteProfile(const std::string& profile) {
    std::lock_guard state(clientStateMutex());
    if (!validProfile(profile) || profileKey(profile) == "default" || base_.empty()) return false;
    if (impl_) {
        std::lock_guard queue(impl_->mx);
        std::erase_if(impl_->pending, [&](const auto& job) { return profileKey(job.profile) == profileKey(profile); });
    }
    std::lock_guard io(saveMx_);
    committed_[profileKey(profile)] = ++nextWrite_;
    std::error_code ec;
    const bool removed = std::filesystem::remove(base_ / "profiles" / (profile + ".json"), ec);
    if (!ec && removed && profileKey(profile_) == profileKey(profile)) {
        profile_ = "default";
        if (impl_) impl_->dirty = false;
        replaceFile(base_ / "last_profile.txt", profile_);
    }
    return !ec && removed;
}

std::vector<std::string> ConfigManager::listProfiles() const {
    std::lock_guard state(clientStateMutex());
    std::vector<std::string> profiles;
    try {
        const auto directory = base_ / "profiles";
        if (!base_.empty() && std::filesystem::exists(directory))
            for (const auto& entry : std::filesystem::directory_iterator(directory))
                if (entry.is_regular_file() && entry.path().extension() == ".json")
                    profiles.push_back(entry.path().stem().string());
    } catch ([[maybe_unused]] const std::exception& error) { VLOG_WARN("cfg", "List failed: {}", error.what()); }
    std::sort(profiles.begin(), profiles.end(), [](const auto& a, const auto& b) {
        if (a == b) return false;
        if (a == "default") return true;
        if (b == "default") return false;
        return a < b;
    });
    return profiles;
}

bool ConfigManager::validName(const std::string& name) { return validProfile(name); }

bool ConfigManager::renameProfile(const std::string& from, const std::string& to) {
    std::lock_guard state(clientStateMutex());
    const std::string oldName = from, newName = to;
    if (base_.empty() || !validProfile(oldName) || !validProfile(newName)) return false;
    if (oldName == newName) return true;
    const bool active = profileKey(profile_) == profileKey(oldName);
    if (active) saveProfile(oldName);
    std::lock_guard io(saveMx_);
    if (active && committed_[profileKey(oldName)] != nextWrite_) return false;
    const auto source = base_ / "profiles" / (oldName + ".json");
    const auto target = base_ / "profiles" / (newName + ".json");
    std::error_code ec;
    if (!std::filesystem::exists(source, ec) || ec) return false;
    if (profileKey(oldName) != profileKey(newName) && std::filesystem::exists(target, ec)) return false;
    if (ec) return false;
    const auto temporary = base_ / "profiles" / (oldName + ".rename.tmp");
    if (std::filesystem::exists(temporary, ec) || ec) return false;
    if (!MoveFileExW(source.c_str(), temporary.c_str(), MOVEFILE_WRITE_THROUGH)) return false;
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        MoveFileExW(temporary.c_str(), source.c_str(), MOVEFILE_WRITE_THROUGH);
        return false;
    }
    if (active && !replaceFile(base_ / "last_profile.txt", newName)) {
        MoveFileExW(target.c_str(), source.c_str(), MOVEFILE_WRITE_THROUGH);
        return false;
    }
    // Invalidate jobs already taken by the writer, then remove queued old names.
    committed_[profileKey(oldName)] = ++nextWrite_;
    committed_[profileKey(newName)] = nextWrite_;
    if (impl_) {
        std::lock_guard queue(impl_->mx);
        std::erase_if(impl_->pending, [&](const auto& job) { return profileKey(job.profile) == profileKey(oldName); });
        if (active) impl_->dirty = false;
    }
    if (active) profile_ = newName;
    return true;
}

std::string ConfigManager::shareProfile(const std::string& profile) const {
    std::lock_guard state(clientStateMutex());
    if (base_.empty() || !validProfile(profile)) throw std::runtime_error("Invalid config name");
    nlohmann::json snapshot;
    if (profileKey(profile) == profileKey(profile_)) snapshot = captureSettings();
    else {
        std::lock_guard io(saveMx_);
        std::ifstream input(base_ / "profiles" / (profile + ".json"), std::ios::binary);
        if (!input) throw std::runtime_error("Config could not be opened");
        std::string raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::string decoded = storage::decodeProfileData(raw);
        snapshot = nlohmann::json::parse(decoded, nullptr, false);
        if (!snapshot.is_object()) throw std::runtime_error("Config corrupted");
    }
    return config::encodeShareCode({profile, std::move(snapshot)});
}

std::string ConfigManager::importProfile(const std::string& code) {
    auto shared = config::decodeShareCode(code);
    if (!validProfile(shared.name)) throw std::runtime_error("Invalid config name in code");
    std::lock_guard state(clientStateMutex());
    if (base_.empty()) throw std::runtime_error("Configs are not initialized");
    validateSettings(shared.settings, captureSettings());
    shared.settings["#owner"] = storage::profileOwnerToken();
    std::lock_guard io(saveMx_);
    const auto directory = base_ / "profiles";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) throw std::runtime_error("Config directory could not be opened");
    std::string name = shared.name;
    for (unsigned suffix = 2; std::filesystem::exists(directory / (name + ".json")); ++suffix) {
        if (suffix > 10000) throw std::runtime_error("No free config name");
        name = shared.name.substr(0, 116) + "-" + std::to_string(suffix);
    }
    const auto serialized = shared.settings.dump(2);
    const auto encoded = storage::encodeProfileData(serialized);
    if (!replaceFile(directory / (name + ".json"), encoded))
        throw std::runtime_error("Imported config could not be saved");
    committed_[profileKey(name)] = ++nextWrite_;
    return name;
}
}
