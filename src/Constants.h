#ifndef ASELSAN_REHIS_CONSTANTS_H
#define ASELSAN_REHIS_CONSTANTS_H
#include <chrono>
#include <cstddef>

namespace constants {

// ------------------------------ Buffer ----------------------------------
// Max characters of one formatted threat (including '\0'). Must cover the longest format() output of every Threat type
inline constexpr std::size_t kTextSize { 128 };
// Number of slots in the ring buffer.
inline constexpr std::size_t kSlotCount { 128 };

// ------------------------------ ThreatLogger defaults ----------------------------------
// Max delay of a log line when the buffer does not reach the threshold.
inline constexpr std::chrono::milliseconds kDefaultFlushPeriod { 200 };
// Producer wakes the writer early when this many records are waiting.
inline constexpr std::size_t kDefaultFlushThreshold { kSlotCount * 3 / 4 };
// Log timestamps are UTC + this offset. Default: Turkey time, UTC+3
inline constexpr std::chrono::minutes kDefaultUtcOffset { std::chrono::hours { 3 } };

// ------------------------------ writer output ----------------------------------
// Room reserved for "[YYYY.MM.DD_HH:MM:SS.MMM] " in front of every line.
inline constexpr std::size_t kTimestampMaxLen { 32 };
// Writer's line buffer: formatted lines are collected here, one write() per this many bytes at most.
inline constexpr std::size_t kLineBufferSize { 8192 };

} // namespace constants

#endif //ASELSAN_REHIS_CONSTANTS_H
