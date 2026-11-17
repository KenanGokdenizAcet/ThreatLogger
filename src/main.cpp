#include "ThreatLogger.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

ThreatLoggerConfig parseArgs(int argc, char** argv) {
    ThreatLoggerConfig cfg;
    static constexpr char kFilePrefix[] = "--log-file=";
    static constexpr std::size_t kFilePrefixLen = sizeof kFilePrefix - 1;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--log-console") == 0) {
            cfg.filePath = nullptr;
        } else if (std::strncmp(arg, kFilePrefix, kFilePrefixLen) == 0 && arg[kFilePrefixLen] != '\0') {
            cfg.filePath = arg + kFilePrefixLen;
        } else {
            std::fprintf(stderr, "[main] unknown or invalid argument '%s', logging to console\n", arg);
            cfg.filePath = nullptr;
        }
    }
    return cfg;
}

// ---------------------------------------------------------------- demo scenario
// Each thread owns a group of threats and logs the whole group every round
// with one chained << expression. Threats move between rounds, so the log
// shows the snapshot taken at the moment each record was produced.

constexpr int kRounds { 10 };
constexpr std::chrono::milliseconds kRoundPeriod { 20 };

// Thread 1: a missile group (MissileThreat only)
void missileTracker() {
    ThreatLogger& logger = ThreatLogger::getInstance();
    std::array<MissileThreat, 3> missiles { {
        { 30.0F, 5.0F, 850.0, 900.0 },
        { 32.5F, 6.0F, 870.0, 910.0 },
        { 35.0F, 4.5F, 820.0, 880.0 },
    } };

    for (int round = 0; round < kRounds; ++round) {
        logger << missiles[0] << missiles[1] << missiles[2];

        for (MissileThreat& m : missiles) {          // missiles approach: azimuth drifts, speed rises
            m.setAzimuth(m.getAzimuth() + 0.5F);
            m.setSpeed(m.getSpeed() + 15.0);
        }
        std::this_thread::sleep_for(kRoundPeriod);
    }
}

// Thread 2: a radar group (RadarThreat only)
void radarWarner() {
    ThreatLogger& logger = ThreatLogger::getInstance();
    std::array<RadarThreat, 3> radars { {
        { 120.0F, 2.0F, 9.4e9, -62.0 },
        { 135.0F, 1.5F, 3.1e9, -70.0 },
        { 210.0F, 0.5F, 1.2e10, -55.0 },
    } };

    for (int round = 0; round < kRounds; ++round) {
        logger << radars[0] << radars[1] << radars[2];

        for (RadarThreat& r : radars) {           // emitters get closer: signal gets stronger
            r.setSignalStrength(r.getSignalStrength() + 1.0);
        }
        std::this_thread::sleep_for(kRoundPeriod);
    }
}

// Thread 3: a laser group (LaserThreat only)
void laserWarner() {
    ThreatLogger& logger = ThreatLogger::getInstance();
    std::array<LaserThreat, 2> lasers { {
        { 60.0F, 3.0F, 1064.0, 20.0 },             // Nd:YAG rangefinder
        { 75.0F, 1.0F, 1064.0, 10.0 },             // designator
    } };

    for (int round = 0; round < kRounds; ++round) {
        logger << lasers[0] << lasers[1];

        for (LaserThreat& l : lasers) {            // platform turns: bearing drifts
            l.setAzimuth(l.getAzimuth() - 0.25F);
        }
        std::this_thread::sleep_for(kRoundPeriod);
    }
}

// Thread 4: a mixed group (logger sees all three types only as Threat&)
void mixedSector() {
    ThreatLogger& logger = ThreatLogger::getInstance();
    MissileThreat missile { 300.0F, 12.0F, 650.0, 700.0 };
    RadarThreat radar { 305.0F, 10.0F, 5.6e9, -68.0 };
    LaserThreat laser { 310.0F, 8.0F, 1550.0, 1.0 };   // eye-safe rangefinder

    for (int round = 0; round < kRounds; ++round) {
        logger << missile << radar << laser;

        missile.setElevation(missile.getElevation() - 1.0F);
        radar.setFrequency(radar.getFrequency() + 1.0e8);
        laser.setPulseRepetition(laser.getPulseRepetition() + 1.0);
        std::this_thread::sleep_for(kRoundPeriod);
    }
}

} // namespace

int main(int argc, char** argv) {
    ThreatLogger::init(parseArgs(argc, argv));          // before any logging thread starts

    // Threads are created at startup (init phase), not while logging.
    std::thread t1(missileTracker);
    std::thread t2(radarWarner);
    std::thread t3(laserWarner);
    std::thread t4(mixedSector);

    t1.join();
    t2.join();
    t3.join();
    t4.join();

    ThreatLogger::getInstance().shutdown();             // flush the remaining records
    return 0;
}
