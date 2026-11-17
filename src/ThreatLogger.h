#ifndef ASELSAN_REHIS_THREAT_LOGGER_H
#define ASELSAN_REHIS_THREAT_LOGGER_H
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "Buffer.h"
#include "Constants.h"
#include "Threat.h"

// The only place the logger names its lock types. On the target RTOS, swap these
// for the OS mutex / condition variable (which provide priority inheritance there).
// Any replacement must work with std::lock_guard / std::unique_lock and offer
// wait(lock, pred), wait_for(lock, timeout, pred) and notify_one().
using LoggerMutex = std::mutex;
using LoggerConditionVariable = std::condition_variable;

struct ThreatLoggerConfig {
    const char* filePath { nullptr };
    std::chrono::milliseconds flushPeriod { constants::kDefaultFlushPeriod };            // max delay of a log line
    std::size_t flushThreshold { constants::kDefaultFlushThreshold };     // we will wake writer when item count is equal to this
    std::chrono::minutes utcOffset { constants::kDefaultUtcOffset };      // log time = UTC + utcOffset (fixed, no daylight saving)
};

class ThreatLogger final {
public:
    static void init(const ThreatLoggerConfig& cfg) noexcept;    // terminates the program on any init error
    static ThreatLogger& getInstance() noexcept;

    // avoid to copy and move
    ThreatLogger(ThreatLogger const&) = delete;
    void operator=(ThreatLogger const&) = delete;

    ThreatLogger(ThreatLogger&&) = delete;
    void operator=(ThreatLogger&&) = delete;

    ThreatLogger& operator<<(Threat const& threat) noexcept;

    void shutdown() noexcept;

    ~ThreatLogger();
private:
    ThreatLogger(const ThreatLoggerConfig& cfg, int destinationFd) noexcept;

    void writerLoop() noexcept;
    void writeBatch(std::size_t count, std::size_t dropped) noexcept;
    void appendLine(std::int64_t timestampMs, const char* text, std::size_t len) noexcept;
    void writeToDestination() noexcept;

    // Atomic: init() writes it (release), getInstance() reads it (acquire). Without atomic,
    // a getInstance() racing with init() would be a data race (undefined behavior).
    static std::atomic<ThreatLogger*> s_instance;
    // Set by the first init() call. Separate from s_instance: s_instance is written only at
    // the end of init(), so checking it would leave a gap where two threads both pass.
    static std::atomic<bool> s_initCalled;

    int m_destinationFd;                       // log file, or STDOUT_FILENO when no file is given
    std::chrono::milliseconds m_period;
    std::size_t m_flushThreshold;
    std::chrono::minutes m_utcOffset;

    LoggerMutex m_mutex;
    LoggerConditionVariable m_cv;
    Buffer m_buffer;
    bool m_stop { false };

    // Used only by the writer thread.
    static constexpr std::size_t kMaxLine { constants::kTimestampMaxLen + Buffer::kTextSize };  // longest formatted line
    std::array<Buffer::Slot, Buffer::kSlotCount> m_writerBuffer {};   // records copied out of m_buffer
    std::array<char, constants::kLineBufferSize> m_lineBuffer {};     // formatted lines waiting to be written
    std::size_t m_lineBufferLen { 0 };                                // bytes used in m_lineBuffer

    // Declared last: started after every other member is fully constructed.
    std::thread m_writer;
};

#endif //ASELSAN_REHIS_THREAT_LOGGER_H
