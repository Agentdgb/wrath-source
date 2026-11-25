#pragma once
#include <cstdint>

namespace dt {

enum class Priority : std::int8_t {
    Lowest  = -100,
    Low     =  -50,
    Normal  =    0,
    High    =   50,
    Highest =  100,
    Monitor =  127,   // observers only; never cancels
};

} // namespace dt
