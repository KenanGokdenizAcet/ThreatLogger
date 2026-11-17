#ifndef ASELSAN_REHIS_LOGGER_PROFILER_H
#define ASELSAN_REHIS_LOGGER_PROFILER_H
// Timing probes for ThreatLogger.
//
// Only compiled when THREAT_LOGGER_PROFILING is defined (the rehis_core_profiled
// library that the benchmark links). The product build never includes this file,
// so the logger pays nothing for it there.
//
// Recording never allocates and never locks: storage is fixed-size, each producer
// thread writes only to its own slot and the writer section is touched only by the
// writer thread. Reports must be read after ThreatLogger::shutdown(), when every
// thread has been joined.

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace profiling {

enum class WakeReason : std::uint8_t { Timeout, Threshold, Shutdown };

// Monotonic clock, for durations.
std::int64_t nowNs() noexcept;
// Wall clock, comparable with Buffer::Slot::timestampMs (end-to-end latency).
std::int64_t wallNowNs() noexcept;

// Optional: label the calling producer thread in the report.
void nameThisThread(const char* name) noexcept;

// One operator<< call. All arguments are nowNs() values taken in this order.
void recordProducer(std::int64_t start, std::int64_t formatted, std::int64_t locked,
                    std::int64_t unlocking, std::int64_t end) noexcept;

// One writer wake-up: why it woke, how many records it copied and how long each phase took.
// copyHoldNs: time the writer held the mutex while copying records into m_writerBuffer.
void recordBatch(WakeReason reason, std::size_t count, std::size_t dropped,
                 std::int64_t copyHoldNs, std::int64_t writeNs) noexcept;

// One record: time from its timestamp until write() returned.
void recordEndToEnd(std::int64_t latencyNs) noexcept;

// Reporting: call only after ThreatLogger::shutdown().
void printReport(std::FILE* out) noexcept;
// Writes producer.csv, writer.csv and e2e.csv into an existing directory.
bool writeCsv(const char* directory) noexcept;

} // namespace profiling

#endif //ASELSAN_REHIS_LOGGER_PROFILER_H
