// HeapCheck: ThreatLogger'ın hangi adımlarda heap allocation yaptığını ölçer (sadece macOS).
//
// Kullanım:
//   rehis_heapcheck [--log-file=PATH]
//
//   Log'lar varsayılan olarak /dev/null'a yazılıyor: writer'ın write() yolu yine çalışıyor,
//   ama log satırları ölçüm sonuçlarının arasına karışmıyor.
//
// Her satır bir ölçüm penceresi: "adım adı  mallocs=N". Beklenen sonuç:
//   - init: allocation var (thread oluşturma, log dosyası). Init'te allocation serbest.
//   - init sonrası her şey (format, operator<<, writer'ın yazması, konsol mesajları): 0.
//
// Malloc sayacı MallocCounter dylib'inde (bkz. MallocCounter.cpp).

#include "Threat.h"
#include "ThreatLogger.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

extern "C" void mc_start() noexcept;
extern "C" int mc_stop() noexcept;

namespace {

int g_failures = 0;

// Ölçüm penceresi: fn() çalışırken yapılan allocation'ları sayar ve yazdırır.
// expectZero: init sonrası adımlar için 0 bekliyoruz; değilse FAIL yazılır.
template <typename Fn>
void measure(const char* name, bool expectZero, Fn fn) {
    mc_start();
    fn();
    const int n = mc_stop();
    const bool fail = expectZero && n != 0;
    g_failures += fail ? 1 : 0;
    std::printf("%-46s mallocs=%-4d %s\n", name, n, fail ? "FAIL" : "");
}

} // namespace

int main(int argc, char** argv) {
    ThreatLoggerConfig cfg;
    cfg.filePath = "/dev/null";
    static constexpr char kFilePrefix[] = "--log-file=";
    if (argc > 1 && std::strncmp(argv[1], kFilePrefix, sizeof kFilePrefix - 1) == 0) {
        cfg.filePath = argv[1] + sizeof kFilePrefix - 1;
    }

    // Threat nesneleri init aşamasında oluşturuluyor (normal programdaki gibi).
    // 123.0625, -62.00005 ve 1064.005 "%g" yuvarlama sınırında: eski snprintf "%g" çözümünde
    // bunlar warm-up'tan sonra bile malloc yapıyordu (2-4 malloc). 850 ve 9.4e9 normal değerler.
    static MissileThreat missile(123.0625f, 5.0f, 850.0, 900.0);
    static RadarThreat radar(10.0f, 2.0f, 9.4e9, -62.00005);
    static LaserThreat laser(5.0f, 1.0f, 1064.005, 20.0);

    // 1) Init: burada allocation olması normal.
    measure("ThreatLogger::init (thread, file)", false, [&] { ThreatLogger::init(cfg); });

    ThreatLogger& logger = ThreatLogger::getInstance();

    // 2) Init sonrası: hepsi 0 olmalı.
    // Gerçek programda format() task'ların kendi thread'lerinde çalışıyor, init'i yapan
    // thread'de değil. Thread başına bir ilk çağrı maliyeti varsa görünsün diye ölçüm
    // hiç log atmamış yeni bir thread'de yapılıyor; thread oluşturma ölçüm penceresinin dışında.
    std::thread task([&] {
        char taskBuf[256];
        measure("Threat::format, first call on a new task", true, [&] {
            missile.format(taskBuf, sizeof taskBuf);
            radar.format(taskBuf, sizeof taskBuf);
            laser.format(taskBuf, sizeof taskBuf);
        });
    });
    task.join();
    // usleep: writer thread'in batch'i yazmasını bekliyoruz (flushPeriod 200 ms).
    measure("logger <<, first batch + writer (timestamp)", true, [&] {
        logger << missile << radar << laser;
        usleep(400'000);
    });
    measure("logger <<, 3000 records + writer", true, [&] {
        for (int i = 0; i < 1000; ++i) {
            logger << missile << radar << laser;
        }
        usleep(400'000);
    });
    measure("fprintf(stderr) + strerror (console message)", true, [] {
        std::fprintf(stderr, "[HeapCheck] sample error: %s\n", std::strerror(ENOENT));
    });
    measure("shutdown", true, [&] { logger.shutdown(); });

    std::printf("%s\n", g_failures == 0 ? "OK: no heap allocation after init" : "FAIL: heap allocation after init");
    return g_failures == 0 ? 0 : 1;
}
