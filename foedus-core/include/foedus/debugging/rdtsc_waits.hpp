/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#ifndef FOEDUS_DEBUGGING_RDTSC_WAITS_HPP_
#define FOEDUS_DEBUGGING_RDTSC_WAITS_HPP_
/**
 * @file foedus/debugging/rdtsc_waits.hpp
 * @brief Implements an RDTSC (Real-time time stamp counter) wait to emulate latency on slower
 * devices.
 * @ingroup DEBUGGING
 */

#include <stdint.h>

namespace foedus {
namespace debugging {
/**
 * @brief Returns the current CPU cycle via x86 RDTSC.
 * @ingroup DEBUGGING
 * @todo ARMv8 equivalent of RDTSC??
 */
inline uint64_t get_rdtsc() {
  uint32_t low, high;
  asm volatile("rdtsc" : "=a" (low), "=d" (high));
  return (static_cast<uint64_t>(high) << 32) | low;
}

/**
 * @brief Wait until the given CPU cycles elapse.
 * @param[in] cycles CPU cycles to wait for
 * @ingroup DEBUGGING
 * @details
 * In case of context switch to a different CPU that has a very different timing (esp on NUMA),
 * we also check if the RDTSC value is not bogus. In that case, we exit the wait.
 * This is also a safety net for wrap-around.
 * Anyways, it's a rare case.
 */
inline void wait_rdtsc_cycles(uint64_t cycles) {
  uint64_t cycle_error = get_rdtsc() - cycles;
  uint64_t cycle_until = get_rdtsc() + cycles;
  while (true) {
    uint64_t current = get_rdtsc();
    if (current >= cycle_until || current <= cycle_error) {
      break;
    }
  }
}

}  // namespace debugging
}  // namespace foedus
#endif  // FOEDUS_DEBUGGING_RDTSC_WAITS_HPP_
