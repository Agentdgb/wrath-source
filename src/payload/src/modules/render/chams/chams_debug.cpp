#include "chams_debug.h"

namespace dt::modules::chams::dbg {

::dt::desklog::Channel ch()
{
    // desklog::open is idempotent per basename and takes its own lock, so this
    // needs no guard of its own beyond the function-local static.
    static ::dt::desklog::Channel c = ::dt::desklog::open("gl_entity_trace");
    return c;
}

} // namespace dt::modules::chams::dbg

