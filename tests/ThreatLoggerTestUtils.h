// ThreatLogger testlerinin ortak helper'ları.
//
// ThreatLogger singleton: process başına bir kez init edilebilir. Bu yüzden init() çağıran her test
// EXPECT_EXIT içinde ayrı bir child process'te çalışır (bkz. LoggerDeathTest). Child'daki check'ler
// childCheck() ile yapılır: fail olursa sebebi stderr'e yazar ve exit code 1 ile çıkar; gtest child'ın
// stderr'ini fail mesajında gösterir.

#ifndef ASELSAN_REHIS_THREAT_LOGGER_TEST_UTILS_H
#define ASELSAN_REHIS_THREAT_LOGGER_TEST_UTILS_H

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "Constants.h"
#include "Threat.h"
#include "ThreatLogger.h"

namespace logger_test {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ---------------- Child process sonucu ----------------

// Child'da bir check fail olursa: sebebi stderr'e yaz, exit code 1 ile çık.
inline void childCheck(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "CHILD CHECK FAILED: %s\n", what);
        std::_Exit(1);
    }
}

// Tüm check'ler geçti. _Exit: static destructor'lar çalışmaz, logger zaten shutdown edildi.
[[noreturn]] inline void childPass() {
    std::_Exit(0);
}

// ---------------- Config ----------------

// "Hiç tetiklenmesin" değerleri: writer sadece shutdown'da uyanıp yazar. Deterministik testler için.
inline constexpr std::chrono::milliseconds kNeverPeriod { 10s };
inline constexpr std::size_t kNeverThreshold { constants::kSlotCount * 10 };   // count hiçbir zaman ulaşamaz

inline void initLogger(const char* filePath,
                       std::chrono::milliseconds period = constants::kDefaultFlushPeriod,
                       std::size_t threshold = constants::kDefaultFlushThreshold,
                       std::chrono::minutes utcOffset = constants::kDefaultUtcOffset) {
    ThreatLoggerConfig cfg {};
    cfg.filePath = filePath;
    cfg.flushPeriod = period;
    cfg.flushThreshold = threshold;
    cfg.utcOffset = utcOffset;
    ThreatLogger::init(cfg);
}

// ---------------- Dosya helper'ları ----------------

// Test başına sabit isimli klasör. Pid kullanmıyoruz: threadsafe death test'te child binary'yi baştan
// çalıştırır ve aynı klasörü bulmalı (parent'ın child bittikten sonra dosyayı okuduğu testler için).
inline fs::path makeTempDir(const std::string& testName) {
    const fs::path dir = fs::temp_directory_path() / ("rehis_logger_" + testName);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

inline std::string readFile(const fs::path& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// '\n' ile biten satırlar, '\n' hariç.
inline std::vector<std::string> readLines(const fs::path& path) {
    std::ifstream in(path);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

// Writer yazarken okunuyorsa son satır yarım olabilir: sadece tamamlanmış satırları say.
inline std::size_t countCompleteLines(const fs::path& path) {
    const std::string content = readFile(path);
    return static_cast<std::size_t>(std::count(content.begin(), content.end(), '\n'));
}

inline bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Child içinde stdout / stderr'i bir dosyaya yönlendirir: logger'ın STDOUT_FILENO'ya write() ettiği
// satırları ve uyarılarını okuyabilmek için.
inline void redirectFd(int targetFd, const fs::path& path) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ::dup2(fd, targetFd);
    ::close(fd);
}

// pred true olana kadar ya da timeout dolana kadar 5 ms aralıkla bekler.
template <typename Pred>
bool waitUntil(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

// ---------------- Satır formatı ----------------

// "[YYYY.MM.DD_HH:MM:SS.MMM] " = 26 karakter
inline constexpr std::size_t kTimestampPrefixLen { 26 };
inline constexpr const char* kTimestampPrefixPattern { R"(^\[\d{4}\.\d{2}\.\d{2}_\d{2}:\d{2}:\d{2}\.\d{3}\] )" };

// Threat'in logger'a vereceği metin: beklenen değeri ürün koduyla aynı yoldan üretiriz.
inline std::string formatThreat(const Threat& threat) {
    char buf[constants::kTextSize];
    const int n = threat.format(buf, sizeof buf);
    return std::string(buf, static_cast<std::size_t>(std::clamp(n, 0, static_cast<int>(sizeof buf) - 1)));
}

inline std::int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Logger'ın timestamp formatının test tarafı kopyası: UTC + utcOffset (varsayılan: Türkiye saati, UTC+3).
// Ürün kodu <chrono> kullanıyor, burada gmtime_r: iki farklı yoldan aynı sonuca varmalı.
// Sabit genişlikli olduğu için string karşılaştırması zaman karşılaştırmasıyla aynı sonucu verir.
inline std::string timestampFor(std::int64_t ms, std::chrono::minutes utcOffset = constants::kDefaultUtcOffset) {
    const auto offsetSeconds = std::chrono::seconds { utcOffset }.count();
    const auto seconds = static_cast<std::time_t>(ms / 1000 + offsetSeconds);
    std::tm tm {};
    gmtime_r(&seconds, &tm);
    char buf[40];
    std::snprintf(buf, sizeof buf, "[%04d.%02d.%02d_%02d:%02d:%02d.%03d] ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms % 1000));
    return buf;
}

// ---------------- Fixture ----------------

class LoggerDeathTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Writer thread'i olan kodda fork-only ("fast") style güvenli değil: "threadsafe" child'da
        // binary'yi baştan çalıştırır, parent'ın state'ini ve thread'lerini miras almaz.
        GTEST_FLAG_SET(death_test_style, "threadsafe");
    }
};

} // namespace logger_test

#endif //ASELSAN_REHIS_THREAT_LOGGER_TEST_UTILS_H
