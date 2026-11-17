// ThreatLogger davranış testleri: çıktı formatı, shutdown, overflow, flush tetikleyicileri, concurrency
//
// Coverage:
//   Satır formatı ve sırası (ThreatLoggerFormatDeathTest)
//     - LineIsTimestampPrefixPlusFormattedThreat   satır = "[YYYY.MM.DD_HH:MM:SS.MMM] " + format() çıktısı + '\n'
//     - AllThreatTypesAreLoggedWithTheirOwnText    Missile / Radar / Laser kendi metniyle, sırayla yazılır
//     - SingleProducerKeepsFifoOrder               tek producer: satırlar push sırasıyla yazılır
//     - StreamOperatorReturnsSameLoggerForChaining operator<< aynı logger'ı döner, a << b << c çalışır
//     - FormatIsSnapshotAtPushTime                 push sonrası threat değişse de satır push anındaki değeri yazar
//     - TimestampIsRecordTimeNotWriteTime          satırdaki zaman push anı, writer'ın yazdığı an değil
//   Timestamp ve utcOffset (ThreatLoggerTimestampDeathTest)
//     - ZeroUtcOffsetWritesUtcTime                 utcOffset 0: satırdaki saat UTC
//     - HalfHourUtcOffsetIsApplied                 +5:30: offset dakika çözünürlüğünde uygulanır
//     - NegativeUtcOffsetIsApplied                 -5:00: UTC'nin gerisindeki bölgeler (gün geri dönebilir)
//   Slot sınırları (ThreatLoggerSlotDeathTest)
//     - OverlongTextIsTruncatedToSlot              kTextSize'ı aşan format() çıktısı kTextSize - 1'de kesilir, sonraki satır sağlam
//     - FormatErrorIsLoggedAsEmptyText             format() < 0 dönerse: boş metinli satır, crash yok (mevcut davranış)
//   Shutdown (ThreatLoggerShutdownDeathTest)
//     - ShutdownFlushesPendingWithoutWaitingPeriod uzun period: shutdown beklemeden kalan kaydı yazar
//     - ShutdownTwiceIsSafe                        ikinci shutdown() no-op, crash yok
//     - LogAfterShutdownIsIgnored                  shutdown sonrası operator<< sessizce yok sayılır
//     - ExitWithoutShutdownFlushesViaDestructor    shutdown() çağrılmadan std::exit: destructor kalanları yazar
//   Overflow (ThreatLoggerOverflowDeathTest)
//     - FullBufferWithoutOverflowDropsNothing      tam kSlotCount kayıt: dropped satırı yok, hepsi yazılır
//     - OverflowReportsDroppedCountAndKeepsNewest  kSlotCount + 5: "[LOGGER] 5 record(s) dropped" + en yeni kSlotCount sırayla
//     - DroppedCountIsReportedOnceThenReset        dropped satırı sadece overflow olan batch'te, sonraki batch'te tekrar yok
//   Line buffer (ThreatLoggerLineBufferDeathTest)
//     - BatchLargerThanLineBufferIsWrittenIntact   kLineBufferSize'ı aşan batch: aradaki flush ile satır kaybı / bozulması yok
//   Flush tetikleyicileri (ThreatLoggerFlushDeathTest, zamana bağlı)
//     - IdleLoggerWritesNothing                    kayıt yoksa dosyaya hiçbir şey yazılmaz
//     - RecordBelowThresholdIsWrittenAfterPeriod   threshold'a ulaşmayan kayıt period dolunca yazılır, önce değil
//     - ThresholdWakesWriterBeforePeriod           threshold kadar kayıt: period beklemeden yazılır
//     - WriterKeepsFlushingAfterFirstBatch         ilk batch sonrası writer tekrar uyuyup sonraki kaydı da yazar
//   Concurrency (ThreatLoggerConcurrencyDeathTest)
//     - ConcurrentProducersLoseNoRecordSilently    8 thread: her satır bütün, yazılan + dropped = push edilen,
//                                                  her thread'in kayıtları kendi sırasında
//
// Zamana bağlı testler: alt sınırlar sıkı (wait_for period'dan erken dönmez), üst sınırlar geniş
// (CI makinesi yavaş olabilir). Deterministik testlerde kNeverPeriod / kNeverThreshold kullanıyoruz:
// writer sadece shutdown'da yazar, zamanlamaya bağlı sonuç yok.
//
// Bilinçli olarak test edilmeyenler:
//   - write() hatası ve EINTR yolu: fd injection gerekir; macOS'ta /dev/full yok.
//   - msToTimestamp / writtenLength doğrudan: anonymous namespace'te. Satır formatı, utcOffset ve slot sınırı
//     testleri üzerinden dolaylı test ediliyor. Gün / ay / artık yıl geçişi: saat inject edilemiyor (system_clock).
//   - Heap allocation olmaması: bench/heapcheck (rehis_heapcheck) ölçüyor.
//   - Data race: bu testler yakalamaz, ThreadSanitizer build'i ile çalıştırılmalı.

#include "ThreatLoggerTestUtils.h"

#include <regex>

using namespace logger_test;

namespace {

using ThreatLoggerFormatDeathTest = LoggerDeathTest;
using ThreatLoggerTimestampDeathTest = LoggerDeathTest;
using ThreatLoggerSlotDeathTest = LoggerDeathTest;
using ThreatLoggerShutdownDeathTest = LoggerDeathTest;
using ThreatLoggerOverflowDeathTest = LoggerDeathTest;
using ThreatLoggerLineBufferDeathTest = LoggerDeathTest;
using ThreatLoggerFlushDeathTest = LoggerDeathTest;
using ThreatLoggerConcurrencyDeathTest = LoggerDeathTest;

bool hasTimestampPrefix(const std::string& line) {
    static const std::regex prefix(kTimestampPrefixPattern);
    return std::regex_search(line, prefix);
}

// Prefix'i atıp sadece threat metnini döner.
std::string textOf(const std::string& line) {
    return line.size() >= kTimestampPrefixLen ? line.substr(kTimestampPrefixLen) : std::string {};
}

// Her satır: timestamp prefix + beklenen metin, aynı sırayla.
bool linesMatch(const std::vector<std::string>& lines, const std::vector<std::string>& expectedTexts) {
    if (lines.size() != expectedTexts.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!hasTimestampPrefix(lines[i]) || textOf(lines[i]) != expectedTexts[i]) {
            return false;
        }
    }
    return true;
}

// Verilen utcOffset ile bir kayıt log eder. Satırdaki timestamp, push anını iki taraftan sınırlayan
// beklenen değerlerin arasında olmalı. Offset uygulanmasaydı ya da yanlış uygulansaydı saat aralığın dışında kalırdı.
bool timestampFollowsOffset(const char* testName, std::chrono::minutes utcOffset) {
    const fs::path logFile = makeTempDir(testName) / "threat.log";
    initLogger(logFile.c_str(), kNeverPeriod, kNeverThreshold, utcOffset);

    const std::int64_t before = nowMs();
    ThreatLogger::getInstance() << MissileThreat(10.0f, 20.0f, 300.0, 1.5);
    const std::int64_t after = nowMs();
    ThreatLogger::getInstance().shutdown();

    const std::vector<std::string> lines = readLines(logFile);
    if (lines.size() != 1) {
        return false;
    }
    const std::string stamp = lines[0].substr(0, kTimestampPrefixLen);
    return timestampFor(before, utcOffset) <= stamp && stamp <= timestampFor(after, utcOffset);
}

// Test threat'leri: format() çıktısını test belirliyor. Threat'in constructor'ı protected, subclass çağırabilir.

// Verilen metni olduğu gibi yazar: slot'a sığmayan uzun metin üretmek için.
class TextThreat final : public Threat {
public:
    explicit TextThreat(std::string text) : Threat(0.0f, 0.0f), m_text(std::move(text)) {}

    int format(char* buf, std::size_t cap) const noexcept override {
        return std::snprintf(buf, cap, "%s", m_text.c_str());
    }

private:
    std::string m_text;
};

// snprintf'in encoding hatası gibi: hiçbir şey yazmadan negatif döner.
class FailingThreat final : public Threat {
public:
    FailingThreat() : Threat(0.0f, 0.0f) {}

    int format(char*, std::size_t) const noexcept override { return -1; }
};

} // namespace

// =====================================================================  Satır formatı ve sırası

TEST_F(ThreatLoggerFormatDeathTest, LineIsTimestampPrefixPlusFormattedThreat) {
    EXPECT_EXIT(
        {
            // Arrange
            const fs::path logFile = makeTempDir("format_line") / "threat.log";
            initLogger(logFile.c_str(), kNeverPeriod, kNeverThreshold);
            const MissileThreat threat(10.0f, 20.0f, 300.0, 1.5);

            // Act
            ThreatLogger::getInstance() << threat;
            ThreatLogger::getInstance().shutdown();

            // Assert
            const std::string content = readFile(logFile);
            const std::vector<std::string> lines = readLines(logFile);
            childCheck(lines.size() == 1, "tek satır yazılmalı");
            childCheck(hasTimestampPrefix(lines[0]), "satır timestamp prefix ile başlamalı");
            childCheck(textOf(lines[0]) == formatThreat(threat), "prefix'ten sonrası format() çıktısı olmalı");
            childCheck(!content.empty() && content.back() == '\n', "satır '\\n' ile bitmeli");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(ThreatLoggerFormatDeathTest, AllThreatTypesAreLoggedWithTheirOwnText) {
    EXPECT_EXIT(
        {
            // Arrange
            const fs::path logFile = makeTempDir("all_types") / "threat.log";
            initLogger(logFile.c_str(), kNeverPeriod, kNeverThreshold);
            const MissileThreat missile(10.0f, 20.0f, 300.0, 1.5);
            const RadarThreat radar(30.0f, 5.0f, 9.4e9, -60.0);
            const LaserThreat laser(45.0f, 2.0f, 1064.0, 20.0);

            // Act: base reference üzerinden, farklı tipler arka arkaya
            ThreatLogger::getInstance() << missile << radar << laser;
            ThreatLogger::getInstance().shutdown();

            // Assert
            childCheck(linesMatch(readLines(logFile), { formatThreat(missile), formatThreat(radar), formatThreat(laser) }),
                       "3 satır, her biri kendi tipinin metniyle ve sırayla");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(ThreatLoggerFormatDeathTest, SingleProducerKeepsFifoOrder) {
    EXPECT_EXIT(
        {
            // Arrange: azimuth = sıra numarası, satırları birbirinden ayırt etmek için
            const fs::path logFile = makeTempDir("fifo") / "threat.log";
            initLogger(logFile.c_str(), kNeverPeriod, kNeverThreshold);
            constexpr int kCount = 50;                // kSlotCount'tan az: overflow yok

            // Act
            std::vector<std::string> expected;
            for (int i = 0; i < kCount; ++i) {
                const MissileThreat threat(static_cast<float>(i), 0.0f, 1.0, 1.0);
                expected.push_back(formatThreat(threat));
                ThreatLogger::getInstance() << threat;
            }
            ThreatLogger::getInstance().shutdown();

            // Assert
            childCheck(linesMatch(readLines(logFile), expected), "satırlar push sırasıyla yazılmalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(ThreatLoggerFormatDeathTest, StreamOperatorReturnsSameLoggerForChaining) {
    EXPECT_EXIT(
        {
            // Arrange
            const fs::path logFile = makeTempDir("chaining") / "threat.log";
            initLogger(logFile.c_str(), kNeverPeriod, kNeverThreshold);
            ThreatLogger& logger = ThreatLogger::getInstance();
            const MissileThreat a(1.0f, 0.0f, 1.0, 1.0);
            const MissileThreat b(2.0f, 0.0f, 1.0, 1.0);
            const MissileThreat c(3.0f, 0.0f, 1.0, 1.0);

            // Act
            ThreatLogger& returned = (logger << a);
            logger << b << c;
            logger.shutdown();

            // Assert
            childCheck(&returned == &logger, "operator<< aynı logger'ı dönmeli");
            childCheck(linesMatch(readLines(logFile), { formatThreat(a), formatThreat(b), formatThreat(c) }),
                       "zincirdeki tüm threat'ler sırayla yazılmalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(ThreatLoggerFormatDeathTest, FormatIsSnapshotAtPushTime) {
    EXPECT_EXIT(
        {
            // Arrange
            const fs::path logFile = makeTempDir("snapshot") / "threat.log";
            initLogger(logFile.c_str(), kNeverPeriod, kNeverThreshold);
            MissileThreat threat(10.0f, 20.0f, 300.0, 1.5);
            const std::string atPush = formatThreat(threat);

            // Act: push sonrası değeri değiştir; writer henüz yazmadı (kNeverPeriod)
            ThreatLogger::getInstance() << threat;
            threat.setSpeed(999.0);
            ThreatLogger::getInstance().shutdown();

            // Assert: format() producer tarafında, push anında çağrılır
            const std::vector<std::string> lines = readLines(logFile);
            childCheck(lines.size() == 1 && textOf(lines[0]) == atPush, "satır push anındaki değeri yazmalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(ThreatLoggerFormatDeathTest, TimestampIsRecordTimeNotWriteTime) {
    EXPECT_EXIT(
        {
            // Arrange
            const fs::path logFile = makeTempDir("timestamp") / "threat.log";
            initLogger(logFile.c_str(), kNeverPeriod, kNeverThreshold);

            // Act: push anını iki taraftan sınırla, sonra yazmayı 300 ms geciktir
            const std::int64_t before = nowMs();
            ThreatLogger::getInstance() << MissileThreat(10.0f, 20.0f, 300.0, 1.5);
            const std::int64_t after = nowMs();
            std::this_thread::sleep_for(300ms);
            ThreatLogger::getInstance().shutdown();   // yazma burada: after + ~300 ms

            // Assert: sabit genişlikli format, string karşılaştırması = zaman karşılaştırması
            const std::vector<std::string> lines = readLines(logFile);
            childCheck(lines.size() == 1, "tek satır yazılmalı");
            const std::string stamp = lines[0].substr(0, kTimestampPrefixLen);
            childCheck(timestampFor(before) <= stamp && stamp <= timestampFor(after),
                       "timestamp push anı olmalı, yazma anı değil");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

// =====================================================================  Timestamp ve utcOffset
// Test tarafı gmtime_r ile hesaplıyor, ürün kodu <chrono> ile: iki bağımsız yol aynı saati vermeli.

TEST_F(ThreatLoggerTimestampDeathTest, ZeroUtcOffsetWritesUtcTime) {
    EXPECT_EXIT(
        {
            childCheck(timestampFollowsOffset("offset_utc", std::chrono::minutes { 0 }), "saat UTC olmalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(ThreatLoggerTimestampDeathTest, HalfHourUtcOffsetIsApplied) {
    EXPECT_EXIT(
        {
            // Hindistan, UTC+5:30: offset saat değil dakika çözünürlüğünde olmalı
            constexpr std::chrono::minutes kOffset { 5 * 60 + 30 };
            childCheck(timestampFollowsOffset("offset_half_hour", kOffset), "saat UTC+5:30 olmalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(ThreatLoggerTimestampDeathTest, NegativeUtcOffsetIsApplied) {
    EXPECT_EXIT(
        {
            // UTC-5: local_time UTC'den küçük, gün de bir önceki gün olabilir
            constexpr std::chrono::minutes kOffset { -5 * 60 };
            childCheck(timestampFollowsOffset("offset_negative", kOffset), "saat UTC-5 olmalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

// =====================================================================  Slot sınırları

TEST_F(ThreatLoggerSlotDeathTest, OverlongTextIsTruncatedToSlot) {
    EXPECT_EXIT(
        {
            // Arrange: slot'tan uzun metin, arkasından normal bir threat
            const fs::path logFile = makeTempDir("overlong") / "threat.log";
            initLogger(logFile.c_str(), kNeverPeriod, kNeverThreshold);
            const TextThreat overlong(std::string(constants::kTextSize * 2, 'x'));
            const MissileThreat next(10.0f, 20.0f, 300.0, 1.5);

            // Act
            ThreatLogger::getInstance() << overlong << next;
            ThreatLogger::getInstance().shutdown();

            // Assert: format() kTextSize * 2 döner ama slot'ta kTextSize - 1 karakter var. len yanlış olsaydı
            // satıra '\0' ya da slot dışı çöp data girerdi, bir sonraki satır da kayardı.
            childCheck(linesMatch(readLines(logFile), { std::string(constants::kTextSize - 1, 'x'), formatThreat(next) }),
                       "uzun metin kTextSize - 1'de kesilmeli, sonraki satır sağlam olmalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

// Mevcut davranışı belgeleyen test: format() hata dönerse kayıt boş metinle yazılır (sadece timestamp).
// Kayıt sessizce kaybolmuyor, logger crash etmiyor. Davranış değişirse bu test güncellenmeli.
TEST_F(ThreatLoggerSlotDeathTest, FormatErrorIsLoggedAsEmptyText) {
    EXPECT_EXIT(
        {
            // Arrange
            const fs::path logFile = makeTempDir("format_error") / "threat.log";
            initLogger(logFile.c_str(), kNeverPeriod, kNeverThreshold);
            const FailingThreat failing;
            const MissileThreat next(10.0f, 20.0f, 300.0, 1.5);

            // Act
            ThreatLogger::getInstance() << failing << next;
            ThreatLogger::getInstance().shutdown();

            // Assert
            childCheck(linesMatch(readLines(logFile), { "", formatThreat(next) }),
                       "hatalı kayıt boş metinle yazılmalı, sonraki satır sağlam olmalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

// =====================================================================  Shutdown

TEST_F(ThreatLoggerShutdownDeathTest, ShutdownFlushesPendingWithoutWaitingPeriod) {
    EXPECT_EXIT(
        {
            // Arrange: period 10 s, threshold hiç dolmaz: sadece shutdown yazdırabilir
            const fs::path logFile = makeTempDir("shutdown_flush") / "threat.log";
            initLogger(logFile.c_str(), kNeverPeriod, kNeverThreshold);
            ThreatLogger::getInstance() << MissileThreat(10.0f, 20.0f, 300.0, 1.5);

            // Act
            const auto start = std::chrono::steady_clock::now();
            ThreatLogger::getInstance().shutdown();
            const auto elapsed = std::chrono::steady_clock::now() - start;

            // Assert
            childCheck(elapsed < 2s, "shutdown period'u beklememeli");
            childCheck(countCompleteLines(logFile) == 1, "bekleyen kayıt yazılmalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(ThreatLoggerShutdownDeathTest, ShutdownTwiceIsSafe) {
    EXPECT_EXIT(
        {
            // Arrange
            const fs::path logFile = makeTempDir("shutdown_twice") / "threat.log";
            initLogger(logFile.c_str(), kNeverPeriod, kNeverThreshold);
            ThreatLogger::getInstance() << MissileThreat(10.0f, 20.0f, 300.0, 1.5);

            // Act: ikinci çağrı no-op olmalı (fd iki kez close edilmemeli, join iki kez yapılmamalı)
            ThreatLogger::getInstance().shutdown();
            ThreatLogger::getInstance().shutdown();

            // Assert
            childCheck(countCompleteLines(logFile) == 1, "kayıt bir kez yazılmalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(ThreatLoggerShutdownDeathTest, LogAfterShutdownIsIgnored) {
    EXPECT_EXIT(
        {
            // Arrange
            const fs::path logFile = makeTempDir("log_after_shutdown") / "threat.log";
            initLogger(logFile.c_str(), kNeverPeriod, kNeverThreshold);
            const MissileThreat beforeShutdown(1.0f, 0.0f, 1.0, 1.0);
            const MissileThreat afterShutdown(2.0f, 0.0f, 1.0, 1.0);

            // Act
            ThreatLogger::getInstance() << beforeShutdown;
            ThreatLogger::getInstance().shutdown();
            ThreatLogger::getInstance() << afterShutdown;   // crash yok, yazılmaz

            // Assert
            childCheck(linesMatch(readLines(logFile), { formatThreat(beforeShutdown) }),
                       "sadece shutdown öncesi kayıt yazılmalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(ThreatLoggerShutdownDeathTest, ExitWithoutShutdownFlushesViaDestructor) {
    // Arrange: dosyayı parent okuyacak. makeTempDir child'da tekrar çalışır, aynı klasörü bulur.
    const fs::path logFile = makeTempDir("destructor") / "threat.log";

    // Act: shutdown() yok; std::exit static destructor'ları çalıştırır -> ~ThreatLogger -> shutdown()
    EXPECT_EXIT(
        {
            initLogger(logFile.c_str(), kNeverPeriod, kNeverThreshold);
            ThreatLogger::getInstance() << MissileThreat(10.0f, 20.0f, 300.0, 1.5);
            std::exit(0);
        },
        ::testing::ExitedWithCode(0), "");

    // Assert (parent'ta)
    EXPECT_EQ(countCompleteLines(logFile), 1u);
}

// =====================================================================  Overflow
// Writer sadece shutdown'da uyanır (kNeverPeriod / kNeverThreshold): push edilenlerin hepsi
// ring buffer'da birikir, overflow deterministik.

TEST_F(ThreatLoggerOverflowDeathTest, FullBufferWithoutOverflowDropsNothing) {
    EXPECT_EXIT(
        {
            // Arrange
            const fs::path logFile = makeTempDir("full_no_overflow") / "threat.log";
            initLogger(logFile.c_str(), kNeverPeriod, kNeverThreshold);

            // Act: tam kapasite
            std::vector<std::string> expected;
            for (std::size_t i = 0; i < constants::kSlotCount; ++i) {
                const MissileThreat threat(static_cast<float>(i), 0.0f, 1.0, 1.0);
                expected.push_back(formatThreat(threat));
                ThreatLogger::getInstance() << threat;
            }
            ThreatLogger::getInstance().shutdown();

            // Assert
            const std::vector<std::string> lines = readLines(logFile);
            childCheck(!contains(readFile(logFile), "[LOGGER]"), "dropped satırı olmamalı");
            childCheck(linesMatch(lines, expected), "kSlotCount kaydın hepsi sırayla yazılmalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(ThreatLoggerOverflowDeathTest, OverflowReportsDroppedCountAndKeepsNewest) {
    EXPECT_EXIT(
        {
            // Arrange
            const fs::path logFile = makeTempDir("overflow") / "threat.log";
            initLogger(logFile.c_str(), kNeverPeriod, kNeverThreshold);
            constexpr std::size_t kExtra = 5;

            // Act: kapasiteden 5 fazla: en eski 5 kayıt overwrite edilir
            std::vector<std::string> pushed;
            for (std::size_t i = 0; i < constants::kSlotCount + kExtra; ++i) {
                const MissileThreat threat(static_cast<float>(i), 0.0f, 1.0, 1.0);
                pushed.push_back(formatThreat(threat));
                ThreatLogger::getInstance() << threat;
            }
            ThreatLogger::getInstance().shutdown();

            // Assert: önce dropped satırı, sonra en yeni kSlotCount kayıt sırayla
            std::vector<std::string> expected { "[LOGGER] 5 record(s) dropped: buffer overflow" };
            expected.insert(expected.end(), pushed.begin() + kExtra, pushed.end());
            childCheck(linesMatch(readLines(logFile), expected),
                       "dropped satırı + en yeni kSlotCount kayıt sırayla yazılmalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

// Zamana bağlı (period 100 ms): writer ilk kayıtla uyanır, period dolana kadar bekler. kSlotCount + 3 push
// mikro saniyeler sürer, yani hepsi period dolmadan biter ve ilk batch'te 3 kayıt drop edilmiş olur.
TEST_F(ThreatLoggerOverflowDeathTest, DroppedCountIsReportedOnceThenReset) {
    EXPECT_EXIT(
        {
            // Arrange
            constexpr std::size_t kExtra = 3;
            const fs::path logFile = makeTempDir("dropped_reset") / "threat.log";
            initLogger(logFile.c_str(), 100ms, kNeverThreshold);

            // Act: 1. batch overflow, 2. batch tek kayıt
            for (std::size_t i = 0; i < constants::kSlotCount + kExtra; ++i) {
                ThreatLogger::getInstance() << MissileThreat(static_cast<float>(i), 0.0f, 1.0, 1.0);
            }
            const bool firstBatch = waitUntil([&] { return countCompleteLines(logFile) == constants::kSlotCount + 1; }, 3s);
            const MissileThreat last(999.0f, 0.0f, 1.0, 1.0);
            ThreatLogger::getInstance() << last;
            const bool secondBatch = waitUntil([&] { return countCompleteLines(logFile) == constants::kSlotCount + 2; }, 3s);
            ThreatLogger::getInstance().shutdown();

            // Assert: dropped satırı bir kez ve doğru sayıyla; ikinci batch sadece kendi kaydını yazmış
            const std::vector<std::string> lines = readLines(logFile);
            const auto droppedLines = std::count_if(lines.begin(), lines.end(),
                                                    [](const std::string& line) { return contains(line, "[LOGGER]"); });
            childCheck(firstBatch, "ilk batch period sonunda yazılmalı");
            childCheck(secondBatch, "ikinci batch period sonunda yazılmalı");
            childCheck(droppedLines == 1, "dropped satırı sadece bir kez yazılmalı");
            childCheck(textOf(lines.front()) == "[LOGGER] 3 record(s) dropped: buffer overflow", "ilk satır dropped satırı olmalı");
            childCheck(textOf(lines.back()) == formatThreat(last), "son satır ikinci batch'in kaydı olmalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

// =====================================================================  Line buffer

TEST_F(ThreatLoggerLineBufferDeathTest, BatchLargerThanLineBufferIsWrittenIntact) {
    EXPECT_EXIT(
        {
            // Arrange: uzun satırlar (~95 byte) x kSlotCount, tek batch'te > kLineBufferSize
            const fs::path logFile = makeTempDir("line_buffer") / "threat.log";
            initLogger(logFile.c_str(), kNeverPeriod, kNeverThreshold);

            // Act
            std::vector<std::string> expected;
            for (std::size_t i = 0; i < constants::kSlotCount; ++i) {
                const MissileThreat threat(123.456f + static_cast<float>(i), -45.6789f, 123456.789, 1.23456789);
                expected.push_back(formatThreat(threat));
                ThreatLogger::getInstance() << threat;
            }
            ThreatLogger::getInstance().shutdown();

            // Assert
            childCheck(fs::file_size(logFile) > constants::kLineBufferSize,
                       "test anlamlı olmalı: batch line buffer'dan büyük olmalı");
            childCheck(linesMatch(readLines(logFile), expected), "satırlar kayıpsız, bozulmadan, sırayla yazılmalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

// =====================================================================  Flush tetikleyicileri (zamana bağlı)

TEST_F(ThreatLoggerFlushDeathTest, IdleLoggerWritesNothing) {
    EXPECT_EXIT(
        {
            // Arrange: kısa period, ama hiç kayıt yok
            const fs::path logFile = makeTempDir("idle") / "threat.log";
            initLogger(logFile.c_str(), 20ms);

            // Act
            std::this_thread::sleep_for(200ms);       // 10 period boyunca bekle
            const auto sizeWhileIdle = fs::file_size(logFile);
            ThreatLogger::getInstance().shutdown();

            // Assert
            childCheck(sizeWhileIdle == 0, "idle iken bir şey yazılmamalı");
            childCheck(fs::file_size(logFile) == 0, "boş shutdown bir şey yazmamalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(ThreatLoggerFlushDeathTest, RecordBelowThresholdIsWrittenAfterPeriod) {
    EXPECT_EXIT(
        {
            // Arrange: 1 kayıt, threshold'a uzak: sadece period yazdırabilir
            constexpr auto kPeriod = 300ms;
            const fs::path logFile = makeTempDir("period_flush") / "threat.log";
            initLogger(logFile.c_str(), kPeriod, kNeverThreshold);

            // Act
            const auto start = std::chrono::steady_clock::now();
            ThreatLogger::getInstance() << MissileThreat(10.0f, 20.0f, 300.0, 1.5);
            const bool written = waitUntil([&] { return countCompleteLines(logFile) == 1; }, 3s);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            ThreatLogger::getInstance().shutdown();

            // Assert: wait_for period'dan erken dönmez (alt sınır sıkı), üst sınır geniş
            childCheck(written, "kayıt period sonunda yazılmalı");
            childCheck(elapsed >= kPeriod - 10ms, "kayıt period dolmadan yazılmamalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(ThreatLoggerFlushDeathTest, ThresholdWakesWriterBeforePeriod) {
    EXPECT_EXIT(
        {
            // Arrange: period 10 s, threshold 10
            constexpr std::size_t kThreshold = 10;
            const fs::path logFile = makeTempDir("threshold_flush") / "threat.log";
            initLogger(logFile.c_str(), kNeverPeriod, kThreshold);

            // Act
            for (std::size_t i = 0; i < kThreshold; ++i) {
                ThreatLogger::getInstance() << MissileThreat(static_cast<float>(i), 0.0f, 1.0, 1.0);
            }
            const bool written = waitUntil([&] { return countCompleteLines(logFile) == kThreshold; }, 2s);
            ThreatLogger::getInstance().shutdown();

            // Assert: 10 s'lik period'dan çok önce yazılmış olmalı
            childCheck(written, "threshold'a ulaşınca writer period beklemeden yazmalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

TEST_F(ThreatLoggerFlushDeathTest, WriterKeepsFlushingAfterFirstBatch) {
    EXPECT_EXIT(
        {
            // Arrange
            const fs::path logFile = makeTempDir("second_batch") / "threat.log";
            initLogger(logFile.c_str(), 50ms, kNeverThreshold);

            // Act + Assert: her kayıt ayrı batch; writer her seferinde uyuyup tekrar uyanmalı
            ThreatLogger::getInstance() << MissileThreat(1.0f, 0.0f, 1.0, 1.0);
            const bool first = waitUntil([&] { return countCompleteLines(logFile) == 1; }, 2s);
            ThreatLogger::getInstance() << MissileThreat(2.0f, 0.0f, 1.0, 1.0);
            const bool second = waitUntil([&] { return countCompleteLines(logFile) == 2; }, 2s);
            ThreatLogger::getInstance().shutdown();

            childCheck(first, "ilk kayıt period sonunda yazılmalı");
            childCheck(second, "ikinci kayıt da shutdown beklemeden yazılmalı");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}

// =====================================================================  Concurrency

TEST_F(ThreatLoggerConcurrencyDeathTest, ConcurrentProducersLoseNoRecordSilently) {
    EXPECT_EXIT(
        {
            // Arrange: azimuth = thread içindeki sıra, elevation = thread no. Default config.
            constexpr int kThreads = 8;
            constexpr int kPerThread = 1000;
            const fs::path logFile = makeTempDir("concurrency") / "threat.log";
            initLogger(logFile.c_str());

            // Act
            std::vector<std::thread> producers;
            for (int t = 0; t < kThreads; ++t) {
                producers.emplace_back([t] {
                    for (int seq = 0; seq < kPerThread; ++seq) {
                        ThreatLogger::getInstance()
                            << MissileThreat(static_cast<float>(seq), static_cast<float>(t), 1.0, 1.0);
                    }
                });
            }
            for (std::thread& p : producers) {
                p.join();
            }
            ThreatLogger::getInstance().shutdown();

            // Assert: her satır ya bir threat ya da dropped satırı; ikisi de bütün olmalı
            static const std::regex threatLine(R"(^\[MISSILE\] id=\d+ az=(\d+) el=(\d+) speed=1 velocity=1$)");
            static const std::regex droppedLine(R"(^\[LOGGER\] (\d+) record\(s\) dropped: buffer overflow$)");

            std::size_t written = 0;
            std::size_t dropped = 0;
            std::vector<int> lastSeq(kThreads, -1);
            for (const std::string& line : readLines(logFile)) {
                childCheck(hasTimestampPrefix(line), "her satır timestamp ile başlamalı");
                const std::string text = textOf(line);
                std::smatch m;
                if (std::regex_match(text, m, droppedLine)) {
                    dropped += std::stoul(m[1].str());
                    continue;
                }
                childCheck(std::regex_match(text, m, threatLine), "satır bütün olmalı (karışma / kesilme yok)");
                const int seq = std::stoi(m[1].str());
                const int t = std::stoi(m[2].str());
                childCheck(t >= 0 && t < kThreads, "thread no geçerli olmalı");
                childCheck(seq > lastSeq[t], "her thread'in kayıtları kendi sırasında olmalı");
                lastSeq[t] = seq;
                ++written;
            }
            childCheck(written + dropped == static_cast<std::size_t>(kThreads * kPerThread),
                       "yazılan + dropped = push edilen (sessiz kayıp yok)");
            childPass();
        },
        ::testing::ExitedWithCode(0), "");
}
