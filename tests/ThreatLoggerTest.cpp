// ThreatLogger unit testleri: init / getInstance ve default davranışlar
//
// Coverage:
//   Compile-time tasarım kararları (static_assert)
//     - Copy / move constructor ve assignment delete edilmiş (singleton kopyalanamaz)
//     - init(), getInstance(), operator<<, shutdown() noexcept
//   Config default'ları
//     - ConfigDefaultsMatchConstants               ThreatLoggerConfig{}: filePath nullptr, period / threshold / utcOffset Constants.h'tan
//     - DefaultUtcOffsetIsTurkeyTime               kDefaultUtcOffset = UTC+3 (Türkiye, 2016'dan beri yaz saati yok)
//   Yanlış init sırası (fatal -> abort)
//     - GetInstanceBeforeInitAborts                init() olmadan getInstance(): sebep stderr'e yazılır, abort
//     - InitTwiceAborts                            ikinci init() çağrısı: sebep stderr'e yazılır, abort
//   Doğru init
//     - InitThenGetInstanceReturnsSameInstance     init sonrası getInstance() her seferinde aynı nesneyi döner
//   Destination seçimi ve console fallback
//     - NullFilePathFallsBackToConsole             filePath nullptr: uyarı stderr'e, log satırı stdout'a
//     - EmptyFilePathFallsBackToConsole            filePath "": open fail, uyarı + stdout
//     - MissingDirectoryFallsBackToConsole         olmayan klasör: open fail, uyarı + stdout
//     - DirectoryAsFilePathFallsBackToConsole      path bir klasör: open fail, uyarı + stdout
//     - ValidFilePathWritesToFile                  geçerli path: dosya oluşur, satır dosyaya yazılır, uyarı yok, stdout boş
//     - ExistingFileIsAppendedNotTruncated         var olan dosya: O_APPEND, eski içerik korunur
//
// Neden death test: init() bir static local oluşturur, process başına bir kez çağrılabilir. init() çağıran
// her test EXPECT_EXIT / EXPECT_DEATH içinde ayrı child process'te çalışır (bkz. ThreatLoggerTestUtils.h).
//
// Bilinçli olarak test edilmeyenler (bu dosyada):
//   - Format, flush, overflow, shutdown, concurrency: ThreatLoggerBehaviorTest.cpp
//   - flushPeriod / flushThreshold için 0 gibi değerler: init şu an bunları validate etmiyor.
//
// Not: Test kodunda std::string / std::filesystem (heap) kullanıyoruz. "Heap yok" kuralı ürün kodu için;
// test binary'si RTOS'ta çalışmıyor.

#include "ThreatLoggerTestUtils.h"

#include <type_traits>

using namespace logger_test;

namespace {

// ---------------- Compile-time tasarım kararları ----------------
static_assert(!std::is_copy_constructible_v<ThreatLogger>, "ThreatLogger kopyalanamaz olmalı");
static_assert(!std::is_copy_assignable_v<ThreatLogger>, "ThreatLogger copy assign edilemez olmalı");
static_assert(!std::is_move_constructible_v<ThreatLogger>, "ThreatLogger move edilemez olmalı");
static_assert(!std::is_move_assignable_v<ThreatLogger>, "ThreatLogger move assign edilemez olmalı");
static_assert(noexcept(ThreatLogger::init(std::declval<const ThreatLoggerConfig&>())), "init() noexcept olmalı");
static_assert(noexcept(ThreatLogger::getInstance()), "getInstance() noexcept olmalı");
static_assert(noexcept(std::declval<ThreatLogger&>() << std::declval<const Threat&>()), "operator<< noexcept olmalı");
static_assert(noexcept(std::declval<ThreatLogger&>().shutdown()), "shutdown() noexcept olmalı");

// Log edilen satırda aranan işaret. id her threat'te değiştiği için sadece tipe bakıyoruz.
constexpr const char* kExpectedLine = "[MISSILE]";

// Child'ın ortak akışı: stdout'u yakala, init, bir threat log et, shutdown (writer kalanları yazar).
// Dönen değer: yakalanan stdout içeriği.
std::string initLogAndShutdown(const fs::path& dir, const char* filePath) {
    const fs::path stdoutFile = dir / "stdout.txt";
    redirectFd(STDOUT_FILENO, stdoutFile);

    initLogger(filePath);
    ThreatLogger::getInstance() << MissileThreat(10.0f, 20.0f, 300.0, 1.5);
    ThreatLogger::getInstance().shutdown();       // writer thread join: satır yazılmış olur

    return readFile(stdoutFile);
}

// Console fallback beklenen testlerin ortak check'i: satır stdout'a gitmiş mi.
[[noreturn]] void exitWithStdoutCheck(const std::string& captured) {
    childCheck(contains(captured, kExpectedLine), "log satırı stdout'a yazılmalı");
    childPass();
}

using ThreatLoggerDeathTest = LoggerDeathTest;

} // namespace

// ---------------- Config default'ları ----------------

TEST(ThreatLoggerConfigTest, ConfigDefaultsMatchConstants) {
    // Arrange & Act
    const ThreatLoggerConfig cfg {};

    // Assert: hiçbir alan verilmezse console + Constants.h default'ları
    EXPECT_EQ(cfg.filePath, nullptr);
    EXPECT_EQ(cfg.flushPeriod, constants::kDefaultFlushPeriod);
    EXPECT_EQ(cfg.flushThreshold, constants::kDefaultFlushThreshold);
    EXPECT_EQ(cfg.utcOffset, constants::kDefaultUtcOffset);
}

// Tasarım kararı: log saati Türkiye saati. Biri sabiti değiştirirse bu test bunu görünür yapar.
TEST(ThreatLoggerConfigTest, DefaultUtcOffsetIsTurkeyTime) {
    EXPECT_EQ(constants::kDefaultUtcOffset, std::chrono::hours { 3 });
}

// ---------------- Yanlış init sırası ----------------

TEST_F(ThreatLoggerDeathTest, GetInstanceBeforeInitAborts) {
    // init() yok: getInstance() fatal() ile abort etmeli, sebebi stderr'e yazmalı.
    // EXPECT_DEATH: child sinyal ile (abort -> SIGABRT) ya da sıfırdan farklı code ile ölmeli.
    EXPECT_DEATH(ThreatLogger::getInstance(), "getInstance\\(\\) called before init\\(\\)");
}

TEST_F(ThreatLoggerDeathTest, InitTwiceAborts) {
    EXPECT_DEATH(
        {
            initLogger(nullptr);
            initLogger(nullptr);                  // ikinci çağrı: abort beklenir
        },
        "init\\(\\) called twice");
}

// ---------------- Doğru init ----------------

TEST_F(ThreatLoggerDeathTest, InitThenGetInstanceReturnsSameInstance) {
    EXPECT_EXIT(
        {
            initLogger(nullptr);

            ThreatLogger& first = ThreatLogger::getInstance();
            ThreatLogger& second = ThreatLogger::getInstance();
            first.shutdown();

            childCheck(&first == &second, "getInstance() aynı nesneyi dönmeli");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

// ---------------- Destination seçimi ve console fallback ----------------
// EXPECT_EXIT'in son parametresi child'ın stderr'i üzerinde regex: uyarı mesajının basıldığını doğrular.
// Exit code ise satırın gerçekten stdout'a gittiğini doğrular.

TEST_F(ThreatLoggerDeathTest, NullFilePathFallsBackToConsole) {
    EXPECT_EXIT(
        {
            const fs::path dir = makeTempDir("null_path");
            exitWithStdoutCheck(initLogAndShutdown(dir, nullptr));
        },
        ::testing::ExitedWithCode(0), "no log file given, logging to console");
}

TEST_F(ThreatLoggerDeathTest, EmptyFilePathFallsBackToConsole) {
    EXPECT_EXIT(
        {
            const fs::path dir = makeTempDir("empty_path");
            exitWithStdoutCheck(initLogAndShutdown(dir, ""));
        },
        ::testing::ExitedWithCode(0), "cannot open log file '' for writing.*logging to console");
}

TEST_F(ThreatLoggerDeathTest, MissingDirectoryFallsBackToConsole) {
    EXPECT_EXIT(
        {
            const fs::path dir = makeTempDir("missing_dir");
            const std::string badPath = (dir / "yok" / "threat.log").string();   // "yok" klasörü oluşturulmadı
            exitWithStdoutCheck(initLogAndShutdown(dir, badPath.c_str()));
        },
        ::testing::ExitedWithCode(0), "cannot open log file .*threat\\.log.*logging to console");
}

TEST_F(ThreatLoggerDeathTest, DirectoryAsFilePathFallsBackToConsole) {
    EXPECT_EXIT(
        {
            const fs::path dir = makeTempDir("dir_as_path");
            const std::string dirPath = dir.string();   // dosya değil klasör: O_WRONLY ile açılamaz (EISDIR)
            exitWithStdoutCheck(initLogAndShutdown(dir, dirPath.c_str()));
        },
        ::testing::ExitedWithCode(0), "cannot open log file .*logging to console");
}

TEST_F(ThreatLoggerDeathTest, ValidFilePathWritesToFile) {
    EXPECT_EXIT(
        {
            // Arrange: stderr'i de yakalıyoruz ki "uyarı basılmadı" check'i yapılabilsin.
            // Dikkat: bu yüzden bu testte childCheck mesajları da stderr.txt'ye gider, gtest çıktısında görünmez.
            const fs::path dir = makeTempDir("valid_path");
            const fs::path logFile = dir / "threat.log";
            const fs::path stderrFile = dir / "stderr.txt";
            redirectFd(STDERR_FILENO, stderrFile);

            // Act
            const std::string captured = initLogAndShutdown(dir, logFile.c_str());

            // Assert
            childCheck(contains(readFile(logFile), kExpectedLine), "satır dosyaya yazılmalı");
            childCheck(captured.empty(), "console'a bir şey gitmemeli");
            childCheck(!contains(readFile(stderrFile), "[ThreatLogger]"), "uyarı basılmamalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(ThreatLoggerDeathTest, ExistingFileIsAppendedNotTruncated) {
    EXPECT_EXIT(
        {
            // Arrange: önceki çalıştırmadan kalmış gibi bir satır
            const fs::path dir = makeTempDir("append");
            const fs::path logFile = dir / "threat.log";
            const std::string oldLine = "onceki calistirma satiri\n";
            std::ofstream(logFile) << oldLine;

            // Act
            initLogAndShutdown(dir, logFile.c_str());

            // Assert: eski satır başta duruyor, yeni satır arkasına eklenmiş
            const std::string content = readFile(logFile);
            childCheck(content.rfind(oldLine, 0) == 0, "eski içerik korunmalı");
            childCheck(contains(content.substr(oldLine.size()), kExpectedLine), "yeni satır sona eklenmeli");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}
