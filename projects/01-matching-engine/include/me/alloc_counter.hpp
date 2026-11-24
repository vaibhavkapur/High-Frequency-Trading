// Heap allocation instrumentation.
//
// Linking the `me_alloc_counter` library into a binary replaces the global
// operator new/delete with counting versions. It is linked only into the
// allocation test and the benchmark harnesses, never into the engine itself.
#pragma once

#include <cstdint>

namespace me {

// Number of successful operator new calls made by this process so far.
std::uint64_t alloc_count() noexcept;

}  // namespace me
