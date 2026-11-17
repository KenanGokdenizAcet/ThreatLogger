// ThreatLogger benchmark: sistemi uçtan uca çalıştırır ve zamanlamaları raporlar.
//
// Kullanım:
//   rehis_bench --scenario=normal|stress|burst [--log-console | --log-file=PATH] [--out=DIR]
//
//   --scenario   hangi yük senaryosu çalışacak (default: normal)
//   --log-*      sink seçimi; default: DIR/threats.log
//   --out        CSV ve log'un yazılacağı klasör; default: bench_results/<scenario>
//
// Senaryolar:
//   normal  main.cpp ile aynı: 4 thread (missile, radar, laser, mixed), 10 round, round arası 20 ms.
//           Düşük contention.
//   stress  4 thread, her biri 5000 kaydı arka arkaya basar. Yüksek contention, overflow.
//           stress ve burst'te thread'ler tipleri sırayla paylaşır: missile, radar, laser, missile.
//   burst   4 thread barrier'da buluşup aynı anda 100'er kayıt basar (5 kez, 50 ms arayla).
//           400 kayıt > 128 slot: her burst'te drop beklenir.
//
// Çıktı: stdout'a özet tablo (min / avg / p50 / p99 / max) + DIR altına ham CSV'ler
// (producer.csv, writer.csv, e2e.csv) grafik çizmek için.
//
// Not: Bu binary THREAT_LOGGER_PROFILING ile derlenen rehis_core_profiled'a link'lenir.
// Ölçüm noktalarının kendisi (steady_clock::now) çağrı başına birkaç on ns ekler.

#include <array>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "LoggerProfiler.h"
#include "ThreatLogger.h"

namespace {

using Clock = std::chrono::steady_clock;
// ---------------------------------------------------------------- argümanlar

struct BenchArgs {
    std::string scenario { "normal" };
    bool console { false };
    std::string logFile;                       // boşsa out/threats.log
    std::string outDir;                        // boşsa bench_results/<scenario>
};

bool startsWith(const char* s, const char* prefix) {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

bool parseArgs(int argc, char** argv, BenchArgs& args) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (startsWith(a, "--scenario=")) {
            args.scenario = a + std::strlen("--scenario=");
        } else if (std::strcmp(a, "--log-console") == 0) {
            args.console = true;
        } else if (startsWith(a, "--log-file=")) {
            args.logFile = a + std::strlen("--log-file=");
        } else if (startsWith(a, "--out=")) {
            args.outDir = a + std::strlen("--out=");
        } else {
            std::fprintf(stderr, "[bench] bilinmeyen argüman: %s\n", a);
            return false;
        }
    }
    if (args.outDir.empty()) {
        args.outDir = "bench_results/" + args.scenario;
    }
    if (!args.console && args.logFile.empty()) {
        args.logFile = args.outDir + "/threats.log";
    }
    return true;
}

// mkdir -p
bool makeDirectories(const std::string& path) {
    for (std::size_t pos = 0; pos != std::string::npos;) {
        pos = path.find('/', pos + 1);
        const std::string part = path.substr(0, pos);
        if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- senaryolar

// --- normal: main.cpp'deki senaryonun aynısı
constexpr int kNormalRounds { 10 };
constexpr std::chrono::milliseconds kNormalPeriod { 20 };

void missileTracker() {
    profiling::nameThisThread("missileTracker");
    ThreatLogger& logger = ThreatLogger::getInstance();
    std::array<MissileThreat, 3> missiles { {
        { 30.0F, 5.0F, 850.0, 900.0 },
        { 32.5F, 6.0F, 870.0, 910.0 },
        { 35.0F, 4.5F, 820.0, 880.0 },
    } };
    for (int round = 0; round < kNormalRounds; ++round) {
        logger << missiles[0] << missiles[1] << missiles[2];
        for (MissileThreat& m : missiles) {
            m.setAzimuth(m.getAzimuth() + 0.5F);
            m.setSpeed(m.getSpeed() + 15.0);
        }
        std::this_thread::sleep_for(kNormalPeriod);
    }
}

void radarWarner() {
    profiling::nameThisThread("radarWarner");
    ThreatLogger& logger = ThreatLogger::getInstance();
    std::array<RadarThreat, 3> radars { {
        { 120.0F, 2.0F, 9.4e9, -62.0 },
        { 135.0F, 1.5F, 3.1e9, -70.0 },
        { 210.0F, 0.5F, 1.2e10, -55.0 },
    } };
    for (int round = 0; round < kNormalRounds; ++round) {
        logger << radars[0] << radars[1] << radars[2];
        for (RadarThreat& r : radars) {
            r.setSignalStrength(r.getSignalStrength() + 1.0);
        }
        std::this_thread::sleep_for(kNormalPeriod);
    }
}

void laserWarner() {
    profiling::nameThisThread("laserWarner");
    ThreatLogger& logger = ThreatLogger::getInstance();
    std::array<LaserThreat, 2> lasers { {
        { 60.0F, 3.0F, 1064.0, 20.0 },
        { 75.0F, 1.0F, 1064.0, 10.0 },
    } };
    for (int round = 0; round < kNormalRounds; ++round) {
        logger << lasers[0] << lasers[1];
        for (LaserThreat& l : lasers) {
            l.setAzimuth(l.getAzimuth() - 0.25F);
        }
        std::this_thread::sleep_for(kNormalPeriod);
    }
}

void mixedSector() {
    profiling::nameThisThread("mixedSector");
    ThreatLogger& logger = ThreatLogger::getInstance();
    MissileThreat missile { 300.0F, 12.0F, 650.0, 700.0 };
    RadarThreat radar { 305.0F, 10.0F, 5.6e9, -68.0 };
    LaserThreat laser { 310.0F, 8.0F, 1550.0, 1.0 };
    for (int round = 0; round < kNormalRounds; ++round) {
        logger << missile << radar << laser;
        missile.setElevation(missile.getElevation() - 1.0F);
        radar.setFrequency(radar.getFrequency() + 1.0e8);
        laser.setPulseRepetition(laser.getPulseRepetition() + 1.0);
        std::this_thread::sleep_for(kNormalPeriod);
    }
}

void runNormal() {
    std::thread t1(missileTracker);
    std::thread t2(radarWarner);
    std::thread t3(laserWarner);
    std::thread t4(mixedSector);
    t1.join();
    t2.join();
    t3.join();
    t4.join();
}

// stress / burst için: index'e göre sırayla missile, radar, laser tiplerinden birini seçer.
// Logger sadece Threat& görür; değişiklik base setter (setAzimuth) ile yapılır.
struct ProducerThreats {
    MissileThreat missile;
    RadarThreat radar;
    LaserThreat laser;

    explicit ProducerThreats(int index)
        : missile { 10.0F * static_cast<float>(index), 5.0F, 800.0, 900.0 },
          radar { 100.0F + static_cast<float>(index), 1.0F, 9.0e9, -60.0 },
          laser { 200.0F + static_cast<float>(index), 2.0F, 1064.0, 20.0 } {}

    Threat& pick(int index) {
        switch (index % 3) {
            case 0:  return missile;
            case 1:  return radar;
            default: return laser;
        }
    }
};

// --- stress: bekleme yok, mutex için sürekli yarış
constexpr int kStressThreads { 4 };
constexpr int kStressRecordsPerThread { 5000 };

const char* const kProducerNames[] = { "producer-0", "producer-1", "producer-2", "producer-3" };

void stressProducer(int index) {
    profiling::nameThisThread(kProducerNames[index]);
    ThreatLogger& logger = ThreatLogger::getInstance();
    ProducerThreats threats(index);
    Threat& threat = threats.pick(index);
    for (int i = 0; i < kStressRecordsPerThread; ++i) {
        threat.setAzimuth(static_cast<float>(i % 360));
        logger << threat;
    }
}

void runStress() {
    std::vector<std::thread> threads;
    for (int i = 0; i < kStressThreads; ++i) {
        threads.emplace_back(stressProducer, i);
    }
    for (auto& t : threads) {
        t.join();
    }
}

// --- burst: hepsi aynı anda başlar, buffer kapasitesinin üstünde kayıt
constexpr int kBurstThreads { 4 };
constexpr int kBursts { 5 };
constexpr int kBurstRecordsPerThread { 100 };
constexpr std::chrono::milliseconds kBurstGap { 50 };

void runBurst() {
    std::barrier sync(kBurstThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kBurstThreads; ++t) {
        threads.emplace_back([t, &sync] {
            profiling::nameThisThread(kProducerNames[t]);
            ThreatLogger& logger = ThreatLogger::getInstance();
            ProducerThreats threats(t);
            Threat& threat = threats.pick(t);
            for (int burst = 0; burst < kBursts; ++burst) {
                sync.arrive_and_wait();        // 4 thread aynı anda başlasın
                for (int i = 0; i < kBurstRecordsPerThread; ++i) {
                    threat.setAzimuth(static_cast<float>(i));
                    logger << threat;
                }
                std::this_thread::sleep_for(kBurstGap);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
}

} // namespace

int main(int argc, char** argv) {
    BenchArgs args;
    if (!parseArgs(argc, argv, args)) {
        return 2;
    }
    void (*scenario)() = nullptr;
    if (args.scenario == "normal") {
        scenario = runNormal;
    } else if (args.scenario == "stress") {
        scenario = runStress;
    } else if (args.scenario == "burst") {
        scenario = runBurst;
    } else {
        std::fprintf(stderr, "[bench] bilinmeyen senaryo: %s (normal|stress|burst)\n", args.scenario.c_str());
        return 2;
    }
    if (!makeDirectories(args.outDir)) {
        std::fprintf(stderr, "[bench] klasör oluşturulamadı: %s\n", args.outDir.c_str());
        return 1;
    }

    // Logger dosyayı O_APPEND ile açar (ürün için doğru davranış). Bench'te her çalıştırma
    // temiz başlasın: önceki run'ın satırları bu run'ın log'una karışmasın.
    if (!args.console) {
        ::unlink(args.logFile.c_str());
    }

    ThreatLoggerConfig cfg;
    cfg.filePath = args.console ? nullptr : args.logFile.c_str();
    ThreatLogger::init(cfg);

    const auto start = Clock::now();
    scenario();
    ThreatLogger::getInstance().shutdown();    // son flush dahil
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();

    std::printf("\n==================== ThreatLogger benchmark ====================\n");
    std::printf("  scenario       : %s\n", args.scenario.c_str());
    std::printf("  sink           : %s\n", args.console ? "console (stdout)" : args.logFile.c_str());
    std::printf("  flushPeriod    : %lld ms\n", static_cast<long long>(cfg.flushPeriod.count()));
    std::printf("  flushThreshold : %zu / %zu slot\n", cfg.flushThreshold, Buffer::kSlotCount);
    std::printf("  mutex          : std::mutex\n");
    std::printf("  süre           : %.1f ms (shutdown dahil)\n", elapsed);
    profiling::printReport(stdout);

    if (profiling::writeCsv(args.outDir.c_str())) {
        std::printf("  CSV: %s/{producer,writer,e2e}.csv\n\n", args.outDir.c_str());
    } else {
        std::fprintf(stderr, "[bench] CSV yazılamadı: %s\n", args.outDir.c_str());
    }
    return 0;
}
