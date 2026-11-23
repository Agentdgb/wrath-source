#include "core/ClientState.h"
#include "core/event/EventBus.h"
#include "core/config/ConfigManager.h"
#include "core/config/ShareCode.h"
#include "core/config/ProfileStorage.h"
#include "core/social/FriendList.h"
#include "modules/combat/ThrowPotPlan.h"
#include "modules/combat/ThrowPotTiming.h"
#include "core/module/ModuleManager.h"
#include "util/AtomicOnce.h"
#include "util/StringScrub.h"
#include "util/TimeBudget.h"
#include "vgui/bridge.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <latch>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int testValue = 0;
int guiValue = 0;
int loadCalls = 0;
bool failGuiOnce = false;
const auto uiThread = std::this_thread::get_id();
std::atomic<bool> snapshotOnWrongThread{false};
nlohmann::json readJson(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return nlohmann::json::parse(vanta::storage::decodeProfileData(bytes));
}
}

// Test doubles isolate actual config persistence from Minecraft and OpenGL.
namespace vanta {
ModuleManager& ModuleManager::get() { static ModuleManager manager; return manager; }
nlohmann::json ModuleManager::saveAll() const {
    if (std::this_thread::get_id() != uiThread) snapshotOnWrongThread.store(true);
    return {{"test", testValue}};
}
void ModuleManager::loadAll(const nlohmann::json& data) { ++loadCalls; testValue = data.value("test", 0); }
namespace vgui::bridge {
void saveGuiState(nlohmann::json& data) { data["#test_gui"] = guiValue; }
void loadGuiState(const nlohmann::json& data) {
    guiValue = data.value("#test_gui", 0);
    if (failGuiOnce) { failGuiOnce = false; throw std::runtime_error("injected GUI apply failure"); }
}
}
}

void testEvents() {
    struct CancelEvent : vanta::Event, vanta::Cancellable {};
    auto& bus = vanta::EventBus::get();
    std::vector<int> order;
    auto low = bus.subscribe<CancelEvent>([&](auto&) { order.push_back(1); }, vanta::Priority::Low);
    auto monitor = bus.subscribe<CancelEvent>([&](auto& event) {
        require(event.cancelled, "monitor ran before cancellation"); order.push_back(3);
    }, vanta::Priority::Monitor);
    auto high = bus.subscribe<CancelEvent>([&](auto& event) { order.push_back(2); event.cancel(); }, vanta::Priority::High);
    CancelEvent event;
    require(!bus.post(event), "cancelled event reported as surviving");
    require(order == std::vector<int>{2, 3}, "first dispatch priority/monitor order incorrect");
    high.reset(); monitor.reset();
    order.clear();
    CancelEvent next;
    require(bus.post(next) && order == std::vector<int>{1}, "unsubscribed listener still ran");
    low.reset();
    struct Plain : vanta::Event {};
    std::unique_ptr<vanta::IListenerToken> self;
    int calls = 0;
    self = bus.subscribe<Plain>([&] { ++calls; self.reset(); });
    Plain plain;
    bus.post(plain); bus.post(plain);
    require(calls == 1, "self-unsubscribe failed");
    int concurrent = 0;
    auto token = bus.subscribe<Plain>([&] { ++concurrent; });
    std::jthread first([&] { for (int i=0; i<1000; ++i) { Plain e; bus.post(e); } });
    std::jthread second([&] { for (int i=0; i<1000; ++i) { Plain e; bus.post(e); } });
    first.join(); second.join();
    require(concurrent == 2000, "concurrent dispatch lost updates");
    bus.stop(); bus.post(plain);
    require(concurrent == 2000, "stopped bus called listener");
    bus.start();
}

void testOnce() {
    vanta::AtomicOnce once;
    std::atomic<int> runs{0};
    std::jthread first([&] { once.run([&] { ++runs; std::this_thread::sleep_for(10ms); }); });
    std::jthread second([&] { once.run([&] { ++runs; }); });
    first.join(); second.join();
    require(runs == 1 && once.done(), "once initialization ran more than once");
    vanta::AtomicOnce failed;
    try { failed.run([] { throw std::runtime_error("expected"); }); } catch (...) {}
    require(failed.failed() && !failed.run([] {}), "failed initialization retried unexpectedly");
}

void testMemory() {
    std::string text = "short";
    text.reserve(4096);
    vanta::util::scrubString(text);
    require(text.empty(), "string not cleared");
    std::vector<std::string> strings{"one", std::string(1024, 'x')};
    vanta::util::scrubStringVector(strings);
    require(strings.empty(), "string vector not cleared");
    vanta::util::TimeBudget budget(1ms);
    std::this_thread::sleep_for(3ms);
    require(budget.expired(), "time budget did not expire");
}

void testPotPlan() {
    using namespace vanta::modules::throwpot;
    const std::array<Potion, 3> weak{{{0,4},{1,4},{2,4}}};
    require(randomPlan(weak, 6, 20, 0).count == 2, "low health did not prioritize two");
    require(randomPlan(weak, 16, 20, 65536).count == 1, "high health did not select one");
    const std::array<Potion, 2> strong{{{0,8},{1,8}}};
    require(randomPlan(strong, 3, 20, 0).count == 2, "first screenshot health did not select two strong pots");
    require(randomPlan(strong, 16, 20, 0).count == 1, "second screenshot health did not select one strong pot");
    require(randomPlan(strong, 6, 20, 65536).count == 2, "second potion did not complete healing");
    require(randomPlan(strong, 4, 20, 0).count == 2, "exact full heal with two pots rejected");
    require(randomPlan(strong, 12, 20, 0).count == 1, "exact full heal with one pot rejected");
    require(randomPlan(weak, 12, 20, 0).count == 2, "two weaker pots did not complete healing");
    for (float health = 1; health <= 20; health += 0.25f) {
        for (unsigned seed : {0u, 65536u, 0xffffffffu}) {
            const auto plan = randomPlan(weak, health, 20, seed);
            require(plan.count <= 2, "too many throws planned");
            require(plan.count == (health >= 20 ? 0u : health + 4 >= 20 ? 1u : 2u), "wrong potion count for missing health");
            require(plan.count < 2 || plan.potions[0].slot != plan.potions[1].slot, "duplicate potion slot");
        }
    }
    require(!randomPlan(weak, 0, 20, 65536).count, "dead player planned throws");
    require(!randomPlan(weak, NAN, 20, 65536).count, "invalid health planned throws");
    require(!randomPlan(strong, 20, 20, 0).count, "full health planned a potion");
    require(needsHealing(16, 20) && !needsHealing(20, 20), "live health recheck incorrect");

    require(doublePlan(strong, 3, 20).count == 2, "Double Pot did not plan two at low health");
    require(doublePlan(strong, 12, 20).count == 1, "Double Pot wasted a second full-heal potion");
    require(doublePlan(strong, 19, 20).count == 1, "Double Pot wasted a potion near full health");
    require(!doublePlan(strong, 20, 20).count, "Double Pot threw at full health");
    require(!doublePlan(strong, NAN, 20).count && !doublePlan(strong, 0, 20).count,
        "Double Pot planned for invalid health or a dead player");
    const std::array<Potion, 3> mixed{{{4,4},{2,8},{6,8}}};
    const auto doubleMixed = doublePlan(mixed, 1, 20);
    require(doubleMixed.count == 2 && doubleMixed.potions[0].healing == 8
        && doubleMixed.potions[1].healing == 8
        && doubleMixed.potions[0].slot != doubleMixed.potions[1].slot,
        "Double Pot did not choose two distinct strongest potions");
    const std::array<Potion, 1> lastPotion{{{3,8}}};
    require(doublePlan(lastPotion, 1, 20).count == 1, "Double Pot ignored the last available potion");
    require(!doublePlan({}, 1, 20).count, "Double Pot planned without potions");
    const Plan ordered{{Potion{2, 8}, Potion{6, 8}}, 2};
    const auto heldFirst = preferHeld(ordered, 6);
    require(heldFirst.count == 2 && heldFirst.potions[0].slot == 6
        && heldFirst.potions[1].slot == 2, "already-held potion was not prioritized");
    const auto unchanged = preferHeld(ordered, 4);
    require(unchanged.potions[0].slot == 2 && unchanged.potions[1].slot == 6,
        "held preference added an unplanned potion");
    const Plan single{{Potion{2, 8}, Potion{}}, 1};
    require(preferHeld(single, 6).count == 1 && preferHeld(single, 6).potions[0].slot == 2,
        "held preference expanded a single-potion plan");
}

void testPotTiming() {
    using namespace vanta::modules::throwpot;
    for (int cooldown = 1; cooldown <= 10; ++cooldown) {
        const int base = cooldown * 50;
        require(throwDelayMs(cooldown, false) == base, "Double Pot ignored cooldown");
        for (auto seed : {0u, 1u, 0xffffffffu}) {
            const int delay = throwDelayMs(cooldown, true, seed);
            require(delay >= base && delay <= base + base / 4, "cooldown outside bounds");
            require(delay >= slotPauseMs, "minimum cooldown bypassed");
            if (cooldown == 1)
                require(delay == 50, "fastest cooldown missed the next tick due to random delay");
        }
    }
    require(throwDelayMs(0, false) == 50 && throwDelayMs(11, false) == 500,
        "invalid cooldown was not bounded");
}

void testShareCode() {
    using namespace vanta::config;
    const SharedProfile source{"portable", {{"unicode", "config española"}, {"nested", {{"value", 12}}}}};
    const auto first = encodeShareCode(source), second = encodeShareCode(source);
    require(first != second, "share identifiers are not random");
    const auto result = decodeShareCode(" \n" + first + "\r\n ");
    require(result.name == source.name && result.settings == source.settings, "share round trip failed");
    auto damaged = first; damaged[30] = damaged[30] == 'A' ? 'B' : 'A';
    for (const auto& invalid : {damaged, std::string("WRATH2") + first.substr(6), std::string("WRATH1."), std::string(600000, 'x')}) {
        bool rejected = false;
        try { decodeShareCode(invalid); } catch (...) { rejected = true; }
        require(rejected, "invalid share code accepted");
    }
    bool rejected = false;
    try { encodeShareCode({"huge", {{"text", std::string(300000, 'x')}}}); } catch (...) { rejected = true; }
    require(rejected, "oversized config accepted");
}

void testConfig() {
    const auto root = std::filesystem::current_path() / ("config-regression-" + std::to_string(GetCurrentProcessId()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{root};
    auto& config = vanta::ConfigManager::get();
    config.init(root);
    testValue = 10;
    config.saveProfile("default");
    require(readJson(root / "profiles/default.json")["test"] == 10, "explicit save failed");
    config.saveProfile("../outside");
    require(!std::filesystem::exists(root / "outside.json"), "profile escaped its directory");
    testValue = 20;
    config.markDirty();
    std::this_thread::sleep_for(420ms);
    config.pumpAutosave();
    testValue = 30;
    config.saveProfile("default");
    std::this_thread::sleep_for(30ms);
    require(readJson(root / "profiles/default.json")["test"] == 30, "queued stale save overwrote newer save");
    require(!snapshotOnWrongThread.load(), "autosave accessed live state on worker");
    config.loadProfile("other");
    testValue = 40; config.markDirty();
    std::this_thread::sleep_for(420ms); config.pumpAutosave();
    require(config.deleteProfile("other"), "profile delete failed");
    require(!vanta::ConfigManager::validName("CON.txt") && !vanta::ConfigManager::validName("../outside"), "unsafe config name accepted");
    config.saveProfile("rename-me");
    require(config.renameProfile("rename-me", "renamed"), "inactive rename failed");
    require(!std::filesystem::exists(root / "profiles/rename-me.json"), "old renamed profile remains");
    require(!config.renameProfile("renamed", "default"), "rename overwrote existing profile");
    config.loadProfile("renamed");
    testValue = 77; config.markDirty();
    std::this_thread::sleep_for(420ms); config.pumpAutosave();
    require(config.renameProfile("renamed", "Active"), "active rename failed");
    require(config.renameProfile("Active", "active"), "case-only rename failed");
    std::this_thread::sleep_for(30ms);
    require(!std::filesystem::exists(root / "profiles/renamed.json"), "queued writer recreated renamed profile");
    require(readJson(root / "profiles/active.json")["test"] == 77, "rename lost current edits");
    HANDLE locked = CreateFileW((root / "profiles/active.json").c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(locked != INVALID_HANDLE_VALUE, "could not lock rename fixture");
    testValue = 78;
    const bool failedRename = config.renameProfile("active", "locked-rename");
    CloseHandle(locked);
    require(!failedRename && config.currentProfile() == "active", "rename ignored failed save of active edits");
    testValue = 88;
    const auto code = config.shareProfile("active");
    const auto imported = config.importProfile(code);
    require(imported == "active-2", "import overwrote existing profile");
    require(readJson(root / "profiles/active-2.json")["test"] == 88, "sharing lost unsaved edits");
    require(config.importProfile(code) == "active-3", "duplicate import did not get unique name");
    bool rejected = false;
    try { config.importProfile(vanta::config::encodeShareCode({"invalid", {{"test", "wrong type"}}})); }
    catch (...) { rejected = true; }
    require(rejected && !std::filesystem::exists(root / "profiles/invalid.json"), "invalid setting import wrote a profile");
    require(!config.deleteProfile("DEFAULT"), "case variant bypassed default protection");

    // Invalid profiles must be rejected before any live state is touched.
    const auto writeProfile = [&](const char* name, const nlohmann::json& data) {
        std::ofstream file(root / "profiles" / (std::string(name) + ".json"), std::ios::binary);
        file << vanta::storage::encodeProfileData(data.dump());
        require(file.good(), "could not write profile fixture");
    };
    const auto originalProfile = config.currentProfile();
    const auto callsBefore = loadCalls;
    writeProfile("bad-types", {{"test", 99}, {"#test_gui", "invalid"}});
    config.loadProfile("bad-types");
    require(loadCalls == callsBefore && testValue == 88 && config.currentProfile() == originalProfile,
        "invalid profile partially changed state");
    writeProfile("bad-friends", {{"test", 99}, {"#friends", {123}}});
    config.loadProfile("bad-friends");
    require(loadCalls == callsBefore && testValue == 88, "invalid friends applied module settings");

    guiValue = 17;
    vanta::FriendList::get().replaceAll({"OriginalFriend"});
    writeProfile("apply-failure", {{"test", 99}, {"#test_gui", 42}, {"#friends", {"NewFriend"}}});
    failGuiOnce = true;
    config.loadProfile("apply-failure");
    require(testValue == 88 && guiValue == 17 && config.currentProfile() == originalProfile
        && vanta::FriendList::get().contains("OriginalFriend")
        && !vanta::FriendList::get().contains("NewFriend") && !config.isLoading(),
        "application failure did not restore modules, GUI and friends");

    // A failed autosave must retry without a new edit or another pump.
    require(config.saveProfile("active"), "could not prepare autosave fixture");
    HANDLE blockedSave = CreateFileW((root / "profiles/active.json").c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(blockedSave != INVALID_HANDLE_VALUE, "could not lock autosave fixture");
    testValue = 91; config.markDirty();
    std::this_thread::sleep_for(420ms); config.pumpAutosave();
    std::this_thread::sleep_for(1100ms);
    config.loadProfile("default");
    require(config.currentProfile() == "active" && testValue == 91,
        "profile switch discarded changes after a failed save");
    CloseHandle(blockedSave);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    bool retried = false;
    while (std::chrono::steady_clock::now() < deadline) {
        // The Windows replacement can briefly deny opening the destination.
        try {
            if (readJson(root / "profiles/active.json")["test"] == 91) { retried = true; break; }
        } catch (const nlohmann::json::exception&) {}
        std::this_thread::sleep_for(20ms);
    }
    require(retried, "failed autosave was not retried automatically");

    config.loadProfile("default"); testValue = 40;
    config.shutdown();
    require(!std::filesystem::exists(root / "profiles/other.json"), "queued save recreated deleted profile");
    require(!std::filesystem::exists(root / "profiles/default.json.tmp"), "temporary save file left behind");
    require(readJson(root / "profiles/default.json")["test"] == 40, "shutdown did not flush final edits");
}

int main() {
    try {
        testEvents(); testOnce(); testMemory(); testPotPlan(); testPotTiming(); testShareCode(); testConfig();
        std::cout << "Event ordering, cancellation, concurrent dispatch, initialization, memory and config regression checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        vanta::ConfigManager::get().shutdown();
        std::cerr << error.what() << '\n'; return 1;
    }
}
