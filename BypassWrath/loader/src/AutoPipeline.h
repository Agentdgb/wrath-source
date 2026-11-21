#pragma once
// AutoPipeline.h -- named command registry + automatic sequential executor.
//
// Turns the loader from "run this flag" into "run once, do everything":
// every step is a named command, executed in registration order, with
// per-command criticality. A critical failure aborts the chain; a
// non-critical failure is recorded and the chain continues.
//
// Usage:
//   bp::CommandRegistry reg;
//   reg.Register({ L"scrub",  [&](std::wstring&){ LaunchScrubChild(log); return true; }, true });
//   reg.Register({ L"inject", [&](std::wstring& e){ return RunInject(bytes, pid) == 0; }, true });
//   auto results = reg.RunAll();

#include <windows.h>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

namespace bp {

struct CommandResult {
    std::wstring name;
    bool         ok = false;
    DWORD64      elapsedMs = 0;
    std::wstring error;
};

struct Command {
    std::wstring                       name;
    std::function<bool(std::wstring&)> fn;       // returns success, writes detail on failure
    bool                               critical = true;
};

namespace detail {

// __try lives here, away from destructible locals (C2712 under /EHsc).
inline bool InvokeSafe(const std::function<bool(std::wstring&)>& fn,
                       std::wstring& err) {
    __try {
        return fn(err);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        err = L"excepcion en comando";
        return false;
    }
}

} // namespace detail

class CommandRegistry {
public:
    void Register(Command c) {
        const std::wstring n = c.name;
        order_.push_back(n);
        map_.emplace(n, std::move(c));
    }

    CommandResult Run(const std::wstring& name) const {
        CommandResult r;
        r.name = name;

        const auto it = map_.find(name);
        if (it == map_.end()) {
            r.error = L"comando no registrado";
            return r;
        }

        std::wstring err;
        const DWORD64 t0 = GetTickCount64();
        const bool ok = detail::InvokeSafe(it->second.fn, err);
        r.elapsedMs = GetTickCount64() - t0;
        r.ok = ok;
        r.error = std::move(err);
        return r;
    }

    std::vector<CommandResult> RunAll() const {
        std::vector<CommandResult> out;
        out.reserve(order_.size());

        for (const auto& n : order_) {
            const auto it = map_.find(n);
            const bool critical = (it != map_.end()) && it->second.critical;

            CommandResult r = Run(n);
            out.push_back(std::move(r));

            if (!out.back().ok && critical)
                break;
        }
        return out;
    }

    std::vector<std::wstring> Names() const { return order_; }
    std::size_t Size() const noexcept { return order_.size(); }

private:
    std::vector<std::wstring>                   order_;
    std::unordered_map<std::wstring, Command>   map_;
};

} // namespace bp
