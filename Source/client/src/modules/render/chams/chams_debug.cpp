#include "chams_debug.h"

namespace vanta::modules::chams::dbg {

::vanta::desklog::Channel ch()
{
    // desklog::open is idempotent per basename and takes its own lock, so this
    // needs no guard of its own beyond the function-local static.
    static ::vanta::desklog::Channel c = ::vanta::desklog::open("vanta_chams");
    return c;
}

} // namespace vanta::modules::chams::dbg
