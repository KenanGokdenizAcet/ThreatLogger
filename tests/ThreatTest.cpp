// Threat unit testleri
//
// Coverage:
//   Compile-time tasarım kararları (static_assert)
//     - Threat abstract class (pure virtual format())
//     - Threat'in virtual destructor'ı var (base pointer üzerinden delete güvenli)
//     - format() noexcept
//     - Her alt sınıf (Missile, Radar, Laser) format()'ı implement etmiş: abstract değil
//   Construction ve getter / setter
//     - ConstructorStoresValues                   constructor değerleri getter'lardan aynen döner
//     - SettersUpdateValues                       setter'lar değeri günceller
//     - ValuesAreStoredExactlyEvenWhenExtreme     -0, denormal, max, inf, NaN: constructor / setter değeri bozmaz
//   format() çıktısı
//     - MissileThreatFormatMatchesExpectedText       [MISSILE] satırı birebir beklenen metin
//     - RadarThreatFormatMatchesExpectedText      [RADAR] satırı birebir beklenen metin, %g ile 9.4e+09
//     - LaserThreatFormatMatchesExpectedText      [LASER] satırı birebir beklenen metin
//     - FormatKeepsSixSignificantDigits           %g 6 anlamlı basamak yazar: 1234567 -> 1.23457e+06
//     - DoubleFieldsMatchPrintfG                  to_chars çıktısı = snprintf("%g") (yuvarlama sınırı, küçük / büyük, denormal, -0)
//     - FloatFieldsMatchPrintfG                   float alanlar (az / el) için de aynı: double'a çevrilip yazılır
//     - FormatReturnsWrittenLength                dönüş değeri '\0' hariç yazılan karakter sayısı
//     - FormatReflectsCurrentState                setter sonrası format() yeni değeri yazar (snapshot anı)
//     - FormatDispatchesThroughBaseReference      Threat& üzerinden çağrı derived format()'a gider (3 tip)
//     - NonFiniteValueIsFormattedAsInf            inf değeri "inf" olarak yazılır, crash yok
//     - NaNIsFormattedAsNan                       NaN değeri "nan" olarak yazılır, crash yok
//   Uç sayısal değerler
//     - ToTextMatchesPrintfGForExtremeDoubles     toText() = %g: denormal, max, ±0, üs eşikleri, yuvarlama taşması, tie, 2^53 üstü
//     - ToTextMatchesPrintfGForExtremeFloats      float'tan gelen değerler için aynı (float denormal, FLT_MAX, 2^24 + 1)
//     - ToTextWritesLongestValuesCompletely       en uzun çıktılar (13 karakter) toText()'in 16 byte'ına kesilmeden yazılır,
//                                                 -NaN ve signaling NaN da sığar
//     - EveryFieldMatchesPrintfGForExtremeSets    3 tip x 8 uç değer seti: satırın tamamı %g'li eski format()'la birebir aynı
//   format() sınır durumları (snprintf contract)
//     - FormatTruncatesWhenCapIsSmall             küçük cap: metin kesilir, '\0' ile biter, tam uzunluk döner
//     - FormatWithZeroCapOnlyReturnsLength        cap 0 + nullptr: hiçbir şey yazmaz, gereken uzunluğu döner
//     - WorstCaseFormatFitsInBufferSlot           en uzun değerler (max, denormal, -inf, -NaN) + 5 haneli id bile
//                                                 Buffer::kTextSize'a sığar (3 tip)
//   Threat ID
//     - IdsAreAssignedSequentially                farklı tiplerde ardışık oluşturulan threat'ler ardışık id alır
//     - IdsAreUniqueAcrossThreads                 4 thread aynı anda threat oluşturur, id çakışması yok
//     - IdWrapsAroundAfter65536Threats            uint16_t sayaç 65536 threat sonra başa döner (mevcut davranış)
//
// Bilinçli olarak test edilmeyenler:
//   - Setter'ların thread safety'si: Threat nesnesi thread-safe değil, paylaşımı kullananın sorumluluğu.
//   - -NaN ve signaling NaN'ın metni: to_chars "-nan(ind)" / "nan(snan)" yazıyor, %g ikisinde de "nan".
//     Standard library'ye bağlı, metni sabitlemiyoruz; sadece buffer'a ve slota sığdığını test ediyoruz.
//   - format() içinde heap allocation olmaması: gtest ölçemiyor, bench/heapcheck (rehis_heapcheck) ölçüyor.
//
// Not: Test kodunda std::string / std::vector (heap) kullanıyoruz. "Heap yok" kuralı ürün kodu için;
// test binary'si RTOS'ta çalışmıyor.

#include <gtest/gtest.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "Buffer.h"
#include "Threat.h"

namespace {

// ---------------- Compile-time tasarım kararları ----------------
// Bu kontroller derleme sırasında yapılır: biri kararı bozarsa test binary'si derlenmez.
static_assert(std::is_abstract_v<Threat>, "Threat abstract olmalı: format() pure virtual");
static_assert(std::has_virtual_destructor_v<Threat>, "Threat base class: destructor virtual olmalı");
static_assert(noexcept(std::declval<const Threat&>().format(nullptr, 0)), "format() noexcept olmalı");
// Alt sınıf format()'ı override etmeyi unutursa abstract kalır ve burada derleme hatası verir.
static_assert(!std::is_abstract_v<MissileThreat>, "MissileThreat format()'ı implement etmeli");
static_assert(!std::is_abstract_v<RadarThreat>, "RadarThreat format()'ı implement etmeli");
static_assert(!std::is_abstract_v<LaserThreat>, "LaserThreat format()'ı implement etmeli");

// format() çıktısını std::string olarak döndürür; testlerde karşılaştırmayı kolaylaştırır.
std::string formatToString(const Threat& threat) {
    std::array<char, Buffer::kTextSize> buf {};
    const int n = threat.format(buf.data(), buf.size());
    return n < 0 ? std::string {} : std::string(buf.data());
}

std::string idText(const Threat& threat) {
    return "id=" + std::to_string(threat.getThreatId());
}

// Beklenen değer: aynı sayının snprintf("%g") çıktısı. to_chars'a geçmeden önceki davranış buydu.
std::string printfG(double value) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", value);
    return buf;
}

// Fail mesajı için: değerin tam hali (17 basamak), 6 basamağa yuvarlanmadan.
std::string exactText(double value) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", value);
    return buf;
}

using DoubleLimits = std::numeric_limits<double>;
using FloatLimits = std::numeric_limits<float>;

// toText() protected static: test için bir subclass using-declaration ile dışarı açıyor.
// Nesne oluşturulmuyor (sınıf hâlâ abstract), sadece static fonksiyon çağrılıyor.
struct ToTextProbe : Threat {
    using Threat::toText;
};

std::string toTextString(double value) {
    return ToTextProbe::toText(value).data();
}

// Bir threat'in dört sayısal alanı: az / el float, diğer ikisi double.
struct FieldValues {
    const char* name;
    float azimuth;
    float elevation;
    double first;                              // speed / frequency / wavelength
    double second;                             // velocity / signal / prf
};

// Uç değer setleri. nextafter constexpr değil: liste runtime'da kuruluyor.
std::vector<FieldValues> extremeSets() {
    const float fNaN = FloatLimits::quiet_NaN();
    const double dNaN = DoubleLimits::quiet_NaN();
    return {
        { "en büyük",          FLT_MAX, -FLT_MAX, DBL_MAX, -DBL_MAX },
        { "en küçük normal",   FLT_MIN, -FLT_MIN, DBL_MIN, -DBL_MIN },
        { "en küçük denormal", FloatLimits::denorm_min(), -FloatLimits::denorm_min(),
                               DoubleLimits::denorm_min(), -DoubleLimits::denorm_min() },
        { "en büyük denormal", std::nextafter(FLT_MIN, 0.0F), -std::nextafter(FLT_MIN, 0.0F),
                               std::nextafter(DBL_MIN, 0.0), -std::nextafter(DBL_MIN, 0.0) },
        { "sıfır ve -0",       0.0F, -0.0F, 0.0, -0.0 },
        { "sonsuz",            FloatLimits::infinity(), -FloatLimits::infinity(),
                               DoubleLimits::infinity(), -DoubleLimits::infinity() },
        { "NaN",               fNaN, fNaN, dNaN, dNaN },
        { "yuvarlama sınırı",  123.0625F, -323.4375F, 1064.005, -100000.5 },
    };
}

// Satırın beklenen hali: to_chars'tan önceki format() ile aynı, sayılar "%g".
std::string expectedLine(const char* tag, const Threat& threat, const char* firstName, const char* secondName,
                         const FieldValues& v) {
    return std::string(tag) + " " + idText(threat) + " az=" + printfG(v.azimuth) + " el=" + printfG(v.elevation)
           + " " + firstName + "=" + printfG(v.first) + " " + secondName + "=" + printfG(v.second);
}

} // namespace

// ---------------- Construction ve getter / setter ----------------

TEST(ThreatTest, ConstructorStoresValues) {
    const MissileThreat missile {45.0F, 10.5F, 850.0, 900.0};
    const RadarThreat radar {120.0F, 2.0F, 9.4e9, -62.0};
    const LaserThreat laser {270.0F, 3.5F, 1064.0, 20.0};

    EXPECT_FLOAT_EQ(missile.getAzimuth(), 45.0F);
    EXPECT_FLOAT_EQ(missile.getElevation(), 10.5F);
    EXPECT_DOUBLE_EQ(missile.getSpeed(), 850.0);
    EXPECT_DOUBLE_EQ(missile.getVelocity(), 900.0);

    EXPECT_FLOAT_EQ(radar.getAzimuth(), 120.0F);
    EXPECT_FLOAT_EQ(radar.getElevation(), 2.0F);
    EXPECT_DOUBLE_EQ(radar.getFrequency(), 9.4e9);
    EXPECT_DOUBLE_EQ(radar.getSignalStrength(), -62.0);

    EXPECT_FLOAT_EQ(laser.getAzimuth(), 270.0F);
    EXPECT_FLOAT_EQ(laser.getElevation(), 3.5F);
    EXPECT_DOUBLE_EQ(laser.getWavelength(), 1064.0);
    EXPECT_DOUBLE_EQ(laser.getPulseRepetition(), 20.0);
}

TEST(ThreatTest, SettersUpdateValues) {
    MissileThreat missile {0.0F, 0.0F, 0.0, 0.0};
    RadarThreat radar {0.0F, 0.0F, 0.0, 0.0};
    LaserThreat laser {0.0F, 0.0F, 0.0, 0.0};

    missile.setAzimuth(30.0F);
    missile.setElevation(5.0F);
    missile.setSpeed(700.0);
    missile.setVelocity(710.0);
    radar.setFrequency(1.2e9);
    radar.setSignalStrength(-80.0);
    laser.setWavelength(1550.0);
    laser.setPulseRepetition(10.0);

    EXPECT_FLOAT_EQ(missile.getAzimuth(), 30.0F);
    EXPECT_FLOAT_EQ(missile.getElevation(), 5.0F);
    EXPECT_DOUBLE_EQ(missile.getSpeed(), 700.0);
    EXPECT_DOUBLE_EQ(missile.getVelocity(), 710.0);
    EXPECT_DOUBLE_EQ(radar.getFrequency(), 1.2e9);
    EXPECT_DOUBLE_EQ(radar.getSignalStrength(), -80.0);
    EXPECT_DOUBLE_EQ(laser.getWavelength(), 1550.0);
    EXPECT_DOUBLE_EQ(laser.getPulseRepetition(), 10.0);
}

// Getter'lar değeri aynen döner: -0'ın işareti, NaN, sonsuz ve denormal da korunur.
// (EXPECT_DOUBLE_EQ 4 ULP tolerans verir; burada birebir aynılık istiyoruz.)
TEST(ThreatTest, ValuesAreStoredExactlyEvenWhenExtreme) {
    const RadarThreat radar {-0.0F, FloatLimits::denorm_min(), DoubleLimits::denorm_min(), DBL_MAX};
    LaserThreat laser {0.0F, 0.0F, 0.0, 0.0};

    laser.setAzimuth(-FLT_MAX);
    laser.setElevation(FloatLimits::infinity());
    laser.setWavelength(DoubleLimits::quiet_NaN());
    laser.setPulseRepetition(-0.0);

    EXPECT_EQ(radar.getAzimuth(), 0.0F);
    EXPECT_TRUE(std::signbit(radar.getAzimuth())) << "-0'ın işareti kaybolmamalı";
    EXPECT_EQ(radar.getElevation(), FloatLimits::denorm_min());
    EXPECT_EQ(radar.getFrequency(), DoubleLimits::denorm_min());
    EXPECT_EQ(radar.getSignalStrength(), DBL_MAX);

    EXPECT_EQ(laser.getAzimuth(), -FLT_MAX);
    EXPECT_EQ(laser.getElevation(), FloatLimits::infinity());
    EXPECT_TRUE(std::isnan(laser.getWavelength()));
    EXPECT_TRUE(std::signbit(laser.getPulseRepetition())) << "-0'ın işareti kaybolmamalı";
}

// ---------------- format() çıktısı ----------------

// id her çalıştırmada farklı olabilir (static sayaç), bu yüzden beklenen metne getter'dan ekliyoruz.
TEST(ThreatTest, MissileThreatFormatMatchesExpectedText) {
    const MissileThreat missile {45.0F, 10.5F, 850.0, 900.0};

    EXPECT_EQ(formatToString(missile),
              "[MISSILE] " + idText(missile) + " az=45 el=10.5 speed=850 velocity=900");
}

// %g büyük sayıları bilimsel gösterime çevirir: 9.4e9 -> "9.4e+09".
TEST(ThreatTest, RadarThreatFormatMatchesExpectedText) {
    const RadarThreat radar {120.0F, 2.0F, 9.4e9, -62.0};

    EXPECT_EQ(formatToString(radar),
              "[RADAR] " + idText(radar) + " az=120 el=2 freq=9.4e+09 signal=-62");
}

// 1064 nm: Nd:YAG laser rangefinder / designator dalga boyu.
TEST(ThreatTest, LaserThreatFormatMatchesExpectedText) {
    const LaserThreat laser {270.0F, 3.5F, 1064.0, 20.0};

    EXPECT_EQ(formatToString(laser),
              "[LASER] " + idText(laser) + " az=270 el=3.5 wavelength=1064 prf=20");
}

// %g en fazla 6 anlamlı basamak yazar: 1234567 Hz -> "1.23457e+06" (son basamaklar yuvarlanır).
// Slot boyutunu sabit tutan bu karar, log'da hassasiyet kaybı da demek: test bunu belgeliyor.
TEST(ThreatTest, FormatKeepsSixSignificantDigits) {
    const LaserThreat laser {0.0F, 0.0F, 1064.0, 1234567.0};

    EXPECT_NE(formatToString(laser).find("prf=1.23457e+06"), std::string::npos) << formatToString(laser);
}

// toText() snprintf("%g") yerine std::to_chars(general, 6) kullanıyor (heap allocation yok).
// Log satırları değişmemeli: her değer için çıktı %g ile birebir aynı olmalı.
// Değerler heap taramasında %g'nin big-integer yoluna düşenler (6 basamakta yuvarlama sınırı) + uç değerler.
TEST(ThreatTest, DoubleFieldsMatchPrintfG) {
    const double values[] = {
        1064.005, -62.00005, 100000.5, 123456.5, 999999.5,       // yuvarlama sınırı (tie)
        0.0, -0.0, 1e-5, 0.0001, 0.1,                             // sıfır, küçük, %g'nin üs eşiği
        1e15, DBL_MAX, -DBL_MAX, DBL_MIN, 5e-324,                 // büyük, en küçük normal, denormal
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
    };

    for (const double value : values) {
        const RadarThreat radar {0.0F, 0.0F, value, 0.0};

        // " signal=" ile bitir: "freq=1" gibi bir prefix'in yanlışlıkla eşleşmesini önler.
        EXPECT_NE(formatToString(radar).find("freq=" + printfG(value) + " signal="), std::string::npos)
            << "beklenen: " << printfG(value) << ", çıktı: " << formatToString(radar);
    }
}

// az / el float: toText(double)'a float -> double dönüşümüyle gelir. %g'ye de aynı dönüşümle gidiyordu.
TEST(ThreatTest, FloatFieldsMatchPrintfG) {
    const float values[] = { 123.0625F, 323.4375F, 45.6789F, 0.1F, -0.0F, FLT_MAX, -FLT_MAX, FLT_MIN };

    for (const float value : values) {
        const MissileThreat missile {value, 0.0F, 0.0, 0.0};

        EXPECT_NE(formatToString(missile).find("az=" + printfG(value) + " el="), std::string::npos)
            << "beklenen: " << printfG(value) << ", çıktı: " << formatToString(missile);
    }
}

// ThreatLogger bu değeri Slot::len'e yazıyor: yanlış olursa log satırı kesik ya da çöp olur.
TEST(ThreatTest, FormatReturnsWrittenLength) {
    const MissileThreat missile {45.0F, 10.5F, 850.0, 900.0};
    std::array<char, Buffer::kTextSize> buf {};

    const int n = missile.format(buf.data(), buf.size());

    ASSERT_GT(n, 0);
    EXPECT_EQ(static_cast<std::size_t>(n), std::strlen(buf.data()));
}

// Logger'ın snapshot kararı: format() o anki state'i yazar, nesne sonradan değişebilir.
TEST(ThreatTest, FormatReflectsCurrentState) {
    MissileThreat missile {45.0F, 10.5F, 850.0, 900.0};
    const std::string before = formatToString(missile);

    missile.setAzimuth(90.5F);
    const std::string after = formatToString(missile);

    EXPECT_NE(before, after);
    EXPECT_NE(after.find("az=90.5"), std::string::npos) << after;
}

// Logger sadece Threat& görür: virtual dispatch doğru derived format()'ı çağırmalı.
TEST(ThreatTest, FormatDispatchesThroughBaseReference) {
    const MissileThreat missile {1.0F, 1.0F, 1.0, 1.0};
    const RadarThreat radar {1.0F, 1.0F, 1.0, 1.0};
    const LaserThreat laser {1.0F, 1.0F, 1.0, 1.0};
    const Threat& asMissile = missile;
    const Threat& asRadar = radar;
    const Threat& asLaser = laser;

    EXPECT_EQ(formatToString(asMissile).rfind("[MISSILE]", 0), 0u);
    EXPECT_EQ(formatToString(asRadar).rfind("[RADAR]", 0), 0u);
    EXPECT_EQ(formatToString(asLaser).rfind("[LASER]", 0), 0u);
}

// Sensörden bozuk veri gelirse (inf) format() crash etmemeli, okunabilir bir şey yazmalı.
TEST(ThreatTest, NonFiniteValueIsFormattedAsInf) {
    const RadarThreat radar {0.0F, 0.0F, std::numeric_limits<double>::infinity(), 0.0};

    EXPECT_NE(formatToString(radar).find("freq=inf"), std::string::npos);
}

// NaN da aynı: crash yok, "nan" yazılır. Slot'a sığmayan ya da çöp bir metin çıkmamalı.
TEST(ThreatTest, NaNIsFormattedAsNan) {
    const RadarThreat radar {0.0F, 0.0F, std::numeric_limits<double>::quiet_NaN(), 0.0};

    EXPECT_NE(formatToString(radar).find("freq=nan signal="), std::string::npos) << formatToString(radar);
}

// ---------------- Uç sayısal değerler ----------------

// toText()'i doğrudan, format()'tan bağımsız test ediyoruz: her değer için %g ile aynı metin.
TEST(ThreatTest, ToTextMatchesPrintfGForExtremeDoubles) {
    const double values[] = {
        // denormal ve normal sınırı, iki işaretle
        DoubleLimits::denorm_min(), -DoubleLimits::denorm_min(),
        std::nextafter(DBL_MIN, 0.0), -std::nextafter(DBL_MIN, 0.0),     // en büyük denormal
        DBL_MIN, -DBL_MIN, DBL_MAX, -DBL_MAX,
        std::nextafter(DBL_MAX, 0.0),                                     // DBL_MAX'tan bir önceki
        // 3 haneli üs
        1e100, -1e100, 1e-100, -1e-100, 1e308, 1e-308,
        // %g'nin sabit / bilimsel geçişleri: üs < -4 ya da >= 6 ise bilimsel
        0.0001, 0.00009999995,                     // yuvarlanınca 1e-4'e çıkıyor: "0.0001"
        0.000099999,                               // "9.9999e-05"
        999999.0, 999999.4, 999999.5, 1e6,         // 999999.5 yuvarlanınca 1e+06
        // yuvarlama taşması: son basamak bir üst basamağa taşıyor
        9.999995, 9.9999949, 99999.95, -9.999995,
        // 6. basamakta tam tie (binary'de tam gösterilebilen): round-half-even
        1234565.0, 1234575.0, 0.5, 2.5,
        // tie gibi görünen ama binary'de tam olmayan değerler
        1064.005, -1064.005, -100000.5, 4.35, 0.1 + 0.2,
        // double'ın tam sayı sınırı: 2^53 ve üstü
        9007199254740992.0, 9007199254740993.0, 1e21, -1e21,
        // işaretli sıfır
        0.0, -0.0,
    };

    for (const double value : values) {
        EXPECT_EQ(toTextString(value), printfG(value)) << "değer: " << exactText(value);
    }
}

// az / el float alanları: toText(double)'a float -> double dönüşümüyle gelir.
TEST(ThreatTest, ToTextMatchesPrintfGForExtremeFloats) {
    const float values[] = {
        FloatLimits::denorm_min(), -FloatLimits::denorm_min(),
        std::nextafter(FLT_MIN, 0.0F), -std::nextafter(FLT_MIN, 0.0F),
        FLT_MIN, -FLT_MIN, FLT_MAX, -FLT_MAX,
        16777216.0F, 16777217.0F,                  // 2^24 ve 2^24 + 1 (float'ta 2^24'e yuvarlanır)
        0.1F, -0.1F, 1e-38F, 123.0625F, -123.0625F, 323.4375F,
        359.99999F, -0.0F,                         // azimuth sınırı, işaretli sıfır
        FloatLimits::infinity(), -FloatLimits::infinity(),
    };

    for (const float value : values) {
        EXPECT_EQ(toTextString(value), printfG(value)) << "değer: " << printfG(value);
    }
}

// toText() 16 byte'lık array dönüyor, to_chars en fazla 15 karakter yazabiliyor. Sığmasaydı to_chars hata
// döner ve buffer'da yarım metin kalırdı: bu yüzden sadece uzunluğa değil, metnin tamamına bakıyoruz.
// En uzun adaylar: işaretli max ve denormal (13 karakter).
TEST(ThreatTest, ToTextWritesLongestValuesCompletely) {
    const double longest[] = {
        -DBL_MAX, -DBL_MIN, -DoubleLimits::denorm_min(), -std::nextafter(DBL_MIN, 0.0), -1.23456789e-300,
    };
    constexpr std::size_t kMaxChars = sizeof(ToTextProbe::toText(0.0)) - 1;     // '\0' için 1 byte

    for (const double value : longest) {
        const std::string text = toTextString(value);

        EXPECT_EQ(text, printfG(value)) << "metin kesilmemeli, değer: " << exactText(value);
        EXPECT_LE(text.size(), kMaxChars) << text;
    }

    // NaN metni standard library'ye bağlı ("-nan(ind)" / "-nan", "nan(snan)" / "nan"): sadece başını sabitliyoruz.
    EXPECT_EQ(toTextString(-DoubleLimits::quiet_NaN()).rfind("-nan", 0), 0u);
    EXPECT_EQ(toTextString(DoubleLimits::signaling_NaN()).rfind("nan", 0), 0u);
    EXPECT_LE(toTextString(-DoubleLimits::quiet_NaN()).size(), kMaxChars);
    EXPECT_LE(toTextString(DoubleLimits::signaling_NaN()).size(), kMaxChars);
}

// Her tipin dört alanı da aynı yoldan yazılıyor mu? Bir alan farklı formatlanırsa
// (örneğin biri %f'e dönerse) satır %g'li eski format()'tan ayrılır.
TEST(ThreatTest, EveryFieldMatchesPrintfGForExtremeSets) {
    for (const FieldValues& v : extremeSets()) {
        const MissileThreat missile {v.azimuth, v.elevation, v.first, v.second};
        const RadarThreat radar {v.azimuth, v.elevation, v.first, v.second};
        const LaserThreat laser {v.azimuth, v.elevation, v.first, v.second};

        EXPECT_EQ(formatToString(missile), expectedLine("[MISSILE]", missile, "speed", "velocity", v)) << "set: " << v.name;
        EXPECT_EQ(formatToString(radar), expectedLine("[RADAR]", radar, "freq", "signal", v)) << "set: " << v.name;
        EXPECT_EQ(formatToString(laser), expectedLine("[LASER]", laser, "wavelength", "prf", v)) << "set: " << v.name;
    }
}

// ---------------- format() sınır durumları ----------------

// snprintf contract: cap'e sığmayan kısım kesilir, buffer her zaman '\0' ile biter,
// dönüş değeri ise kesilmemiş tam uzunluktur. Logger'daki writtenLength() buna güveniyor.
TEST(ThreatTest, FormatTruncatesWhenCapIsSmall) {
    const MissileThreat missile {45.0F, 10.5F, 850.0, 900.0};
    const std::string full = formatToString(missile);
    constexpr std::size_t kSmallCap = 10;
    std::array<char, kSmallCap> buf {};

    const int n = missile.format(buf.data(), buf.size());

    EXPECT_EQ(static_cast<std::size_t>(n), full.size());
    EXPECT_EQ(buf[kSmallCap - 1], '\0');
    EXPECT_EQ(std::string(buf.data()), full.substr(0, kSmallCap - 1));
}

// cap 0 iken snprintf hiçbir şey yazmaz (buf nullptr olabilir), sadece gereken uzunluğu döner.
TEST(ThreatTest, FormatWithZeroCapOnlyReturnsLength) {
    const RadarThreat radar {120.0F, 2.0F, 9.4e9, -62.0};

    const int n = radar.format(nullptr, 0);

    EXPECT_EQ(static_cast<std::size_t>(n), formatToString(radar).size());
}

// kTextSize kararının testi: en uzun sayı metinleri ve 5 haneli id ile bile metin slota sığmalı.
// Tüm alanlar negatif (işaret 1 karakter ekliyor). Yeni bir threat eklenince bu test de eklenmeli.
TEST(ThreatTest, WorstCaseFormatFitsInBufferSlot) {
    const double dNegNaN = -DoubleLimits::quiet_NaN();
    const float fNegNaN = -FloatLimits::quiet_NaN();
    const FieldValues worstSets[] = {
        { "-max",               -FLT_MAX, -FLT_MAX, -DBL_MAX, -DBL_MAX },
        { "-en küçük normal",   -FLT_MIN, -FLT_MIN, -DBL_MIN, -DBL_MIN },
        { "-en büyük denormal", -std::nextafter(FLT_MIN, 0.0F), -std::nextafter(FLT_MIN, 0.0F),
                                -std::nextafter(DBL_MIN, 0.0), -std::nextafter(DBL_MIN, 0.0) },
        { "-en küçük denormal", -FloatLimits::denorm_min(), -FloatLimits::denorm_min(),
                                -DoubleLimits::denorm_min(), -DoubleLimits::denorm_min() },
        { "-inf",               -FloatLimits::infinity(), -FloatLimits::infinity(),
                                -DoubleLimits::infinity(), -DoubleLimits::infinity() },
        { "-NaN",               fNegNaN, fNegNaN, dNegNaN, dNegNaN },
        { "signaling NaN",      FloatLimits::signaling_NaN(), FloatLimits::signaling_NaN(),
                                DoubleLimits::signaling_NaN(), DoubleLimits::signaling_NaN() },
    };
    constexpr std::size_t kMaxIdDigits = 5;            // uint16_t max: 65535

    for (const FieldValues& v : worstSets) {
        const MissileThreat missile {v.azimuth, v.elevation, v.first, v.second};
        const RadarThreat radar {v.azimuth, v.elevation, v.first, v.second};
        const LaserThreat laser {v.azimuth, v.elevation, v.first, v.second};
        const Threat* const threats[] = { &missile, &radar, &laser };

        for (const Threat* threat : threats) {
            const int n = threat->format(nullptr, 0);
            ASSERT_GT(n, 0) << "set: " << v.name;
            const std::size_t idDigits = std::to_string(threat->getThreatId()).size();
            const std::size_t worst = static_cast<std::size_t>(n) - idDigits + kMaxIdDigits;

            // '\0' için bir byte gerekli: < kTextSize
            EXPECT_LT(worst, Buffer::kTextSize) << "set: " << v.name << ", satır: " << formatToString(*threat);
        }
    }
}

// ---------------- Threat ID ----------------

TEST(ThreatTest, IdsAreAssignedSequentially) {
    const MissileThreat first {0.0F, 0.0F, 0.0, 0.0};
    const RadarThreat second {0.0F, 0.0F, 0.0, 0.0};
    const LaserThreat third {0.0F, 0.0F, 0.0, 0.0};

    // Sayaç base class'ta: tip fark etmeksizin tek bir sıra var.
    EXPECT_EQ(static_cast<std::uint16_t>(first.getThreatId() + 1), second.getThreatId());
    EXPECT_EQ(static_cast<std::uint16_t>(second.getThreatId() + 1), third.getThreatId());
}

// createdThreatNumber atomic: aynı anda oluşturulan threat'ler çakışan id almamalı.
TEST(ThreatTest, IdsAreUniqueAcrossThreads) {
    constexpr std::size_t kThreads = 4;
    constexpr std::size_t kPerThread = 1000;                   // toplam 65536'nın çok altında: wrap olmaz
    std::vector<std::vector<std::uint16_t>> idsPerThread(kThreads);

    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&ids = idsPerThread[t]] {
            ids.reserve(kPerThread);
            for (std::size_t i = 0; i < kPerThread; ++i) {
                const MissileThreat threat {0.0F, 0.0F, 0.0, 0.0};
                ids.push_back(threat.getThreatId());
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    std::vector<std::uint16_t> all;
    for (const auto& ids : idsPerThread) {
        all.insert(all.end(), ids.begin(), ids.end());
    }
    std::sort(all.begin(), all.end());
    EXPECT_EQ(std::adjacent_find(all.begin(), all.end()), all.end()) << "duplicate id bulundu";
    EXPECT_EQ(all.size(), kThreads * kPerThread);
}

// Mevcut davranışı belgeleyen test: sayaç uint16_t, 65536 threat sonra id'ler tekrar eder.
// Bu bir karar mı yoksa limit mi, sunumda konuşulacak bir nokta. Davranış değişirse bu test güncellenmeli.
TEST(ThreatTest, IdWrapsAroundAfter65536Threats) {
    const MissileThreat first {0.0F, 0.0F, 0.0, 0.0};
    constexpr int kCounterRange = std::numeric_limits<std::uint16_t>::max() + 1;   // 65536

    for (int i = 0; i < kCounterRange - 1; ++i) {
        const MissileThreat filler {0.0F, 0.0F, 0.0, 0.0};
    }
    const MissileThreat wrapped {0.0F, 0.0F, 0.0, 0.0};

    EXPECT_EQ(wrapped.getThreatId(), first.getThreatId());
}
