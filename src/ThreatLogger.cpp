#include "ThreatLogger.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

// Timing probes. Defined only in the profiling build (rehis_core_profiled);
// in the product build TL_PROFILE(...) expands to nothing.
#ifdef THREAT_LOGGER_PROFILING
#include "LoggerProfiler.h"
#define TL_PROFILE(...) __VA_ARGS__
#else
#define TL_PROFILE(...)
#endif

std::atomic<bool> ThreatLogger::s_initCalled { false };
std::atomic<ThreatLogger*> ThreatLogger::s_instance { nullptr };

// Helpers
namespace {

std::int64_t CurrentTimeMS() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// snprintf contract -> real number of bytes in the buffer.
std::size_t writtenLength(int n, std::size_t cap) noexcept {
    if (n < 0 || cap == 0) {
        return 0;
    }
    const auto wanted = static_cast<std::size_t>(n);
    if (wanted >= cap) {
        return cap - 1;                        // truncated: snprintf kept cap - 1 chars + '\0'
    }
    return wanted;
}

// "[YYYY.MM.DD_HH:MM:SS.MMM] " in local time (UTC + utcOffset), with the calendar types of <chrono>.
// Not localtime_r: it reads the time zone (TZ variable, system zone file) and can allocate
// from the heap on any call, not only the first one (see notes).
std::size_t msToTimestamp(std::int64_t ms, std::chrono::minutes utcOffset, char* out, std::size_t cap) noexcept {
    using namespace std::chrono;
    const local_time<milliseconds> time { milliseconds { ms } + utcOffset };
    const local_days day = floor<days>(time);
    const year_month_day date { day };
    const hh_mm_ss<milliseconds> clock { time - day };

    const int n = std::snprintf(out, cap, "[%04d.%02u.%02u_%02d:%02d:%02d.%03d] ",
                                static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                                static_cast<unsigned>(date.day()), static_cast<int>(clock.hours().count()),
                                static_cast<int>(clock.minutes().count()), static_cast<int>(clock.seconds().count()),
                                static_cast<int>(clock.subseconds().count()));
    return writtenLength(n, cap);
}

int openDestination(const char* path) noexcept {
    if (path == nullptr) {
        std::fprintf(stderr, "[ThreatLogger] no log file given, logging to console\n");
        return STDOUT_FILENO;
    }
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "[ThreatLogger] cannot open log file '%s' for writing (%s), logging to console\n",
                     path, std::strerror(errno));
        return STDOUT_FILENO;
    }
    return fd;
}

// A wrong init sequence (init twice, getInstance before init) is a programming error:
// print the reason to stderr and abort. The check runs in every build, release included.
[[noreturn]] void fatal(const char* reason) noexcept {
    std::fprintf(stderr, "[ThreatLogger] %s, terminating\n", reason);
    std::abort();
}

} // namespace

// ---------------------------------------------------------------- lifecycle

void ThreatLogger::init(const ThreatLoggerConfig& cfg) noexcept {
    if (s_initCalled.exchange(true, std::memory_order_acq_rel)) {
        fatal("init() called twice");
    }

    static ThreatLogger logger(cfg, openDestination(cfg.filePath));
    s_instance.store(&logger, std::memory_order_release);   // publish the fully built logger
}

ThreatLogger& ThreatLogger::getInstance() noexcept {
    // acquire pairs with the release in init(): if we see the pointer, we also see
    // everything init() did before publishing it.
    ThreatLogger* instance = s_instance.load(std::memory_order_acquire);
    if (instance == nullptr) {
        fatal("getInstance() called before init() finished");
    }
    return *instance;
}

ThreatLogger::ThreatLogger(const ThreatLoggerConfig& cfg, int destinationFd) noexcept
    : m_destinationFd(destinationFd),
      m_period(cfg.flushPeriod),
      m_flushThreshold(cfg.flushThreshold),
      m_utcOffset(cfg.utcOffset) {

    // running thread safely after all values are assigned
    m_writer = std::thread(&ThreatLogger::writerLoop, this);
}

ThreatLogger::~ThreatLogger() {
    shutdown();
}

void ThreatLogger::shutdown() noexcept {
    {
        std::lock_guard<LoggerMutex> lock(m_mutex);
        if (m_stop) {
            return;
        }
        m_stop = true;
    }
    m_cv.notify_one();

    if (m_writer.joinable()) {
        m_writer.join();                       // writer writes the remaining records before exiting
    }
    if (m_destinationFd != STDOUT_FILENO) {
        ::close(m_destinationFd);
    }
}

// Returns ThreatLogger& to support chaining: logger << threat1 << threat2 ...
ThreatLogger& ThreatLogger::operator<<(const Threat& threat) noexcept {
    TL_PROFILE(const std::int64_t tStart = profiling::nowNs();)
    Buffer::Slot slot {};
    slot.timestampMs = CurrentTimeMS();
    const int n = threat.format(slot.text.data(), slot.text.size());
    slot.len = static_cast<std::uint16_t>(writtenLength(n, slot.text.size()));
    TL_PROFILE(const std::int64_t tFormatted = profiling::nowNs();)
    TL_PROFILE(std::int64_t tLocked = 0; std::int64_t tUnlocking = 0;)

    bool wakeWriter = false;
    {
        std::lock_guard<LoggerMutex> lock(m_mutex);
        TL_PROFILE(tLocked = profiling::nowNs();)
        if (m_stop) { // if shutting down, ignore
            return *this;
        }
        m_buffer.push(slot);

        // Wake the writer when:
        // 1) the first slot arrives: the writer starts its flush period timer
        // 2) the slot count reaches the flush threshold: write early, before overflow
        const std::size_t count = m_buffer.count();
        if (count == m_flushThreshold || count == 1) {
            wakeWriter = true;
        }
        TL_PROFILE(tUnlocking = profiling::nowNs();)
    }
    if (wakeWriter) {
        m_cv.notify_one();
    }
    TL_PROFILE(profiling::recordProducer(tStart, tFormatted, tLocked, tUnlocking, profiling::nowNs());)
    return  *this;
}

// ---------------------------------------------------------------- writer side

void ThreatLogger::writerLoop() noexcept {
    while (true) {
        std::size_t count = 0;
        std::size_t dropped = 0;
        bool exiting = false;
        TL_PROFILE(profiling::WakeReason reason = profiling::WakeReason::Timeout;)
        TL_PROFILE(std::int64_t tCopyStart = 0; std::int64_t tCopyEnd = 0;)
        {
            std::unique_lock<LoggerMutex> lock(m_mutex);

            // 1) Empty buffer: sleep without a timeout, zero CPU while idle.
            m_cv.wait(lock, [this] { return m_stop || m_buffer.count() > 0; });

            // 2) Something arrived: wait for the period, the threshold or shutdown.
            m_cv.wait_for(lock, m_period, [this] {
                return m_stop || m_buffer.count() >= m_flushThreshold;
            });
            TL_PROFILE(
                tCopyStart = profiling::nowNs();
                reason = m_stop ? profiling::WakeReason::Shutdown
                       : m_buffer.count() >= m_flushThreshold
                             ? profiling::WakeReason::Threshold
                             : profiling::WakeReason::Timeout;
            )

            // 3) Under the lock: only copy records into m_writerBuffer.
            while (count < m_writerBuffer.size()) {
                if (!m_buffer.pop(m_writerBuffer[count])) { // ring buffer is empty
                    break;
                }
                ++count;
            }
            dropped = m_buffer.takeDropped();
            exiting = m_stop;
            TL_PROFILE(tCopyEnd = profiling::nowNs();)
        }

        // 4) Outside the lock: formatting and I/O. Producers are never blocked by this.
        TL_PROFILE(const std::int64_t tWriteStart = profiling::nowNs();)
        writeBatch(count, dropped);
        TL_PROFILE(
            const std::int64_t tWriteEnd = profiling::nowNs();
            profiling::recordBatch(reason, count, dropped, tCopyEnd - tCopyStart, tWriteEnd - tWriteStart);
            const std::int64_t writtenAtNs = profiling::wallNowNs();
            for (std::size_t i = 0; i < count; ++i) {
                profiling::recordEndToEnd(writtenAtNs - m_writerBuffer[i].timestampMs * 1'000'000);
            }
        )

        if (exiting) {
            return;
        }
    }
}

void ThreatLogger::writeBatch(std::size_t count, std::size_t dropped) noexcept {
    if (dropped > 0) {                         // report overwritten records as a log line
        char msg[96];
        const int n = std::snprintf(msg, sizeof msg,
                                    "[LOGGER] %zu record(s) dropped: buffer overflow", dropped);
        appendLine(CurrentTimeMS(), msg, writtenLength(n, sizeof msg));
    }

    for (std::size_t i = 0; i < count; ++i) {
        const Buffer::Slot& slot = m_writerBuffer[i];
        appendLine(slot.timestampMs, slot.text.data(), slot.len);
    }

    writeToDestination();
}

// Adds "[timestamp] text\n" to m_lineBuffer.
void ThreatLogger::appendLine(std::int64_t timestampMs, const char* text, std::size_t len) noexcept {
    if (m_lineBuffer.size() - m_lineBufferLen < kMaxLine) {
        writeToDestination();                  // the longest line may not fit: empty the line buffer first
    }
    m_lineBufferLen += msToTimestamp(timestampMs, m_utcOffset, m_lineBuffer.data() + m_lineBufferLen,
                                     m_lineBuffer.size() - m_lineBufferLen);   // record's own time, not now
    std::memcpy(m_lineBuffer.data() + m_lineBufferLen, text, len);
    m_lineBufferLen += len;
    m_lineBuffer[m_lineBufferLen++] = '\n';
}

// Writes the whole line buffer to the destination (log file or stdout) and empties it.
void ThreatLogger::writeToDestination() noexcept {
    std::size_t offset = 0;
    while (offset < m_lineBufferLen) {
        const ssize_t written = ::write(m_destinationFd, m_lineBuffer.data() + offset,
                                        m_lineBufferLen - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;                      // interrupted by a signal: retry
            }
            break;                             // I/O error: the rest of the line buffer is lost
        }
        offset += static_cast<std::size_t>(written);   // handle partial writes
    }
    m_lineBufferLen = 0;
}