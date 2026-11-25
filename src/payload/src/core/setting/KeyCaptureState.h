#pragma once
#include <array>
#include <optional>
namespace dt {
struct CapturedKey { int virtualKey = 0; bool cancelled = false; };
class KeyCaptureState {
public:
    using Snapshot = std::array<bool,256>;
    void start(const Snapshot& snapshot) { held = snapshot; capturing = true; }
    void cancel() { capturing = false; }
    bool active() const { return capturing; }
    std::optional<CapturedKey> poll(const Snapshot& snapshot, int menuKey) {
        if (!capturing) return {};
        for (int key=1; key<256; ++key) {
            const bool fresh = snapshot[key] && !held[key]; held[key] = snapshot[key];
            if (!fresh) continue;
            if (key==27) { cancel(); return CapturedKey{0,true}; }
            if (key==8 || key==46) { cancel(); return CapturedKey{0,false}; }
            if (key==menuKey || key==1 || key==3 || key==4) continue;
            cancel(); return CapturedKey{key,false};
        }
        return {};
    }
private:
    Snapshot held{};
    bool capturing = false;
};
}
