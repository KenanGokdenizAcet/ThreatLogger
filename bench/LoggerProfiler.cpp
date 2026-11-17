#include "LoggerProfiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <string>
#include <vector>

namespace profiling {
namespace {

// ---------------------------------------------------------------- storage

constexpr int kMaxProducers { 8 };
constexpr int kMaxSamplesPerProducer { 8192 };
constexpr int kMaxBatches { 8192 };
constexpr int kMaxEndToEnd { 65536 };

struct ProducerSample {
    std::int64_t formatNs;
    std::int64_t lockWaitNs;
    std::int64_t holdNs;
    std::int64_t totalNs;
};

struct ProducerData {
    const char* name { nullptr };
    std::int64_t calls { 0 };                  // every call, also past sample capacity
    int stored { 0 };
    std::array<ProducerSample, kMaxSamplesPerProducer> samples {};
};

struct BatchSample {
    WakeReason reason;
    std::size_t count;
    std::size_t dropped;
    std::int64_t copyHoldNs;
    std::int64_t writeNs;
};

std::array<ProducerData, kMaxProducers> g_producers {};
std::atomic<int> g_producerCount { 0 };
thread_local int t_producerIndex { -1 };

// Writer-thread-only.
std::array<BatchSample, kMaxBatches> g_batches {};
std::int64_t g_batchCalls { 0 };
std::array<std::int64_t, kMaxEndToEnd> g_endToEnd {};
std::int64_t g_endToEndCalls { 0 };

ProducerData* thisProducer() noexcept {
    if (t_producerIndex < 0) {
        t_producerIndex = g_producerCount.fetch_add(1, std::memory_order_relaxed);
    }
    return t_producerIndex < kMaxProducers ? &g_producers[t_producerIndex] : nullptr;
}

int producerCount() noexcept {
    return std::min(g_producerCount.load(std::memory_order_relaxed), kMaxProducers);
}

// ---------------------------------------------------------------- statistics
// Reporting runs once, after the measured run: heap use here is fine.

struct Stats {
    std::size_t n { 0 };
    double min { 0 }, avg { 0 }, p50 { 0 }, p99 { 0 }, max { 0 };
};

Stats computeStats(std::vector<double> values) {
    Stats s;
    s.n = values.size();
    if (values.empty()) {
        return s;
    }
    std::sort(values.begin(), values.end());
    double sum = 0;
    for (const double v : values) {
        sum += v;
    }
    // nearest-rank percentile
    const auto rank = [&](double p) {
        const auto idx = static_cast<std::size_t>(p * static_cast<double>(s.n) + 0.999999);
        return values[std::clamp<std::size_t>(idx, 1, s.n) - 1];
    };
    s.min = values.front();
    s.avg = sum / static_cast<double>(s.n);
    s.p50 = rank(0.50);
    s.p99 = rank(0.99);
    s.max = values.back();
    return s;
}

void printStatsHeader(std::FILE* out, const char* unit) {
    std::fprintf(out, "  %-18s %-12s %8s %10s %10s %10s %10s %10s\n",
                 "thread", "metric", "samples", "min", "avg", "p50", "p99", "max");
    std::fprintf(out, "  %-18s %-12s %8s %10s %10s %10s %10s %10s\n",
                 "", "", "", unit, unit, unit, unit, unit);
}

void printStatsRow(std::FILE* out, const char* label, const char* metric, const Stats& s) {
    std::fprintf(out, "  %-18s %-12s %8zu %10.2f %10.2f %10.2f %10.2f %10.2f\n",
                 label, metric, s.n, s.min, s.avg, s.p50, s.p99, s.max);
}

constexpr double kNsPerUs { 1e3 };
constexpr double kNsPerMs { 1e6 };

std::vector<double> producerField(const ProducerData& p, std::int64_t ProducerSample::* field) {
    std::vector<double> v;
    v.reserve(static_cast<std::size_t>(p.stored));
    for (int i = 0; i < p.stored; ++i) {
        v.push_back(static_cast<double>(p.samples[i].*field) / kNsPerUs);
    }
    return v;
}

const char* reasonName(WakeReason r) {
    switch (r) {
        case WakeReason::Timeout:   return "timeout";
        case WakeReason::Threshold: return "threshold";
        case WakeReason::Shutdown:  return "shutdown";
    }
    return "?";
}

} // namespace

// ---------------------------------------------------------------- recording

std::int64_t nowNs() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

std::int64_t wallNowNs() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

void nameThisThread(const char* name) noexcept {
    if (ProducerData* p = thisProducer()) {
        p->name = name;
    }
}

void recordProducer(std::int64_t start, std::int64_t formatted, std::int64_t locked,
                    std::int64_t unlocking, std::int64_t end) noexcept {
    ProducerData* p = thisProducer();
    if (p == nullptr) {
        return;
    }
    ++p->calls;
    if (p->stored < kMaxSamplesPerProducer) {
        p->samples[p->stored++] = { formatted - start, locked - formatted, unlocking - locked, end - start };
    }
}

void recordBatch(WakeReason reason, std::size_t count, std::size_t dropped,
                 std::int64_t copyHoldNs, std::int64_t writeNs) noexcept {
    if (g_batchCalls < kMaxBatches) {
        g_batches[g_batchCalls] = { reason, count, dropped, copyHoldNs, writeNs };
    }
    ++g_batchCalls;
}

void recordEndToEnd(std::int64_t latencyNs) noexcept {
    if (g_endToEndCalls < kMaxEndToEnd) {
        g_endToEnd[g_endToEndCalls] = latencyNs;
    }
    ++g_endToEndCalls;
}

// ---------------------------------------------------------------- reporting

void printReport(std::FILE* out) noexcept {
    // ---- producers
    std::fprintf(out, "\n== Producers: operator<< ==\n");
    std::fprintf(out, "  lock wait = mutex'i almak için beklenen süre, hold = mutex'i tutma süresi\n\n");
    printStatsHeader(out, "us");

    std::vector<double> allWait, allHold, allFormat, allTotal;
    bool truncated = false;
    for (int i = 0; i < producerCount(); ++i) {
        const ProducerData& p = g_producers[i];
        char fallback[24];
        std::snprintf(fallback, sizeof fallback, "thread-%d", i);
        const char* label = p.name != nullptr ? p.name : fallback;

        auto wait = producerField(p, &ProducerSample::lockWaitNs);
        auto hold = producerField(p, &ProducerSample::holdNs);
        auto format = producerField(p, &ProducerSample::formatNs);
        auto total = producerField(p, &ProducerSample::totalNs);

        printStatsRow(out, label, "lock wait", computeStats(wait));
        printStatsRow(out, "", "hold", computeStats(hold));
        printStatsRow(out, "", "format", computeStats(format));
        printStatsRow(out, "", "total", computeStats(total));
        std::fprintf(out, "\n");

        allWait.insert(allWait.end(), wait.begin(), wait.end());
        allHold.insert(allHold.end(), hold.begin(), hold.end());
        allFormat.insert(allFormat.end(), format.begin(), format.end());
        allTotal.insert(allTotal.end(), total.begin(), total.end());
        truncated = truncated || p.calls > p.stored;
    }
    if (producerCount() > 1) {
        printStatsRow(out, "ALL", "lock wait", computeStats(allWait));
        printStatsRow(out, "", "hold", computeStats(allHold));
        printStatsRow(out, "", "format", computeStats(allFormat));
        printStatsRow(out, "", "total", computeStats(allTotal));
    }
    if (truncated) {
        std::fprintf(out, "  ! bazı thread'ler %d sample sınırını aştı; istatistikler ilk %d çağrıdan\n",
                     kMaxSamplesPerProducer, kMaxSamplesPerProducer);
    }
    if (g_producerCount.load() > kMaxProducers) {
        std::fprintf(out, "  ! %d producer thread'den sadece ilk %d ölçüldü\n",
                     g_producerCount.load(), kMaxProducers);
    }

    // ---- writer
    const int batches = static_cast<int>(std::min<std::int64_t>(g_batchCalls, kMaxBatches));
    int byReason[3] = { 0, 0, 0 };
    std::int64_t written = 0;
    std::int64_t dropped = 0;
    std::vector<double> sizes, copyHold, write;
    for (int i = 0; i < batches; ++i) {
        const BatchSample& b = g_batches[i];
        ++byReason[static_cast<int>(b.reason)];
        written += static_cast<std::int64_t>(b.count);
        dropped += static_cast<std::int64_t>(b.dropped);
        sizes.push_back(static_cast<double>(b.count));
        copyHold.push_back(static_cast<double>(b.copyHoldNs) / kNsPerUs);
        write.push_back(static_cast<double>(b.writeNs) / kNsPerUs);
    }

    std::fprintf(out, "\n== Writer thread ==\n");
    std::fprintf(out, "  wake-ups: %d  (timeout: %d, threshold: %d, shutdown: %d)\n",
                 batches, byReason[0], byReason[1], byReason[2]);
    std::fprintf(out, "  records written: %" PRId64 ", dropped (overflow): %" PRId64 "\n\n", written, dropped);
    printStatsHeader(out, "");
    printStatsRow(out, "writer", "batch size", computeStats(sizes));
    std::fprintf(out, "  %-18s %-12s %8s %10s\n", "", "", "", "us");
    printStatsRow(out, "", "copy hold", computeStats(copyHold));
    printStatsRow(out, "", "write I/O", computeStats(write));
    std::fprintf(out, "  copy hold = writer'ın kayıtları kopyalarken mutex'i tuttuğu süre (bu sırada producer'lar bekler)\n");
    if (g_batchCalls > kMaxBatches) {
        std::fprintf(out, "  ! %" PRId64 " batch'ten ilk %d ölçüldü\n", g_batchCalls, kMaxBatches);
    }

    // ---- end to end
    const int e2eCount = static_cast<int>(std::min<std::int64_t>(g_endToEndCalls, kMaxEndToEnd));
    std::vector<double> e2e;
    e2e.reserve(static_cast<std::size_t>(e2eCount));
    for (int i = 0; i < e2eCount; ++i) {
        e2e.push_back(static_cast<double>(g_endToEnd[i]) / kNsPerMs);
    }
    std::fprintf(out, "\n== End-to-end latency: kayıt üretildi -> write() döndü ==\n");
    std::fprintf(out, "  (timestamp ms çözünürlüklü: değerler en fazla 1 ms fazla görünebilir)\n\n");
    printStatsHeader(out, "ms");
    printStatsRow(out, "record", "latency", computeStats(e2e));
    if (g_endToEndCalls > kMaxEndToEnd) {
        std::fprintf(out, "  ! %" PRId64 " kayıttan ilk %d ölçüldü\n", g_endToEndCalls, kMaxEndToEnd);
    }
    std::fprintf(out, "\n");
}

bool writeCsv(const char* directory) noexcept {
    const std::string dir(directory);
    bool ok = true;

    if (std::FILE* f = std::fopen((dir + "/producer.csv").c_str(), "w")) {
        std::fprintf(f, "thread,seq,format_ns,lock_wait_ns,hold_ns,total_ns\n");
        for (int t = 0; t < producerCount(); ++t) {
            const ProducerData& p = g_producers[t];
            const std::string name = p.name != nullptr ? p.name : "thread-" + std::to_string(t);
            for (int i = 0; i < p.stored; ++i) {
                const ProducerSample& s = p.samples[i];
                std::fprintf(f, "%s,%d,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 "\n",
                             name.c_str(), i, s.formatNs, s.lockWaitNs, s.holdNs, s.totalNs);
            }
        }
        std::fclose(f);
    } else {
        ok = false;
    }

    if (std::FILE* f = std::fopen((dir + "/writer.csv").c_str(), "w")) {
        std::fprintf(f, "seq,reason,batch_size,dropped,copy_hold_ns,write_ns\n");
        const int batches = static_cast<int>(std::min<std::int64_t>(g_batchCalls, kMaxBatches));
        for (int i = 0; i < batches; ++i) {
            const BatchSample& b = g_batches[i];
            std::fprintf(f, "%d,%s,%zu,%zu,%" PRId64 ",%" PRId64 "\n",
                         i, reasonName(b.reason), b.count, b.dropped, b.copyHoldNs, b.writeNs);
        }
        std::fclose(f);
    } else {
        ok = false;
    }

    if (std::FILE* f = std::fopen((dir + "/e2e.csv").c_str(), "w")) {
        std::fprintf(f, "seq,latency_ns\n");
        const int n = static_cast<int>(std::min<std::int64_t>(g_endToEndCalls, kMaxEndToEnd));
        for (int i = 0; i < n; ++i) {
            std::fprintf(f, "%d,%" PRId64 "\n", i, g_endToEnd[i]);
        }
        std::fclose(f);
    } else {
        ok = false;
    }
    return ok;
}

} // namespace profiling
