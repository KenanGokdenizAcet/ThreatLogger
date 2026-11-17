// Buffer unit testleri
//
// Coverage:
//   Temel push / pop
//     - PopOnEmptyBufferReturnsFalse           boş buffer'dan pop false döner, count 0
//     - PushAndPopASlot                        push edilen slot (timestamp, len, text) aynen geri gelir
//     - CountTracksPushAndPop                  her push count'u artırır, her pop azaltır
//   FIFO sırası
//     - PopReturnsSlotsInFifoOrder             kayıtlar eklendiği sırayla çıkar
//     - FifoOrderSurvivesWrapAround            head array sonundan başa dönünce de sıra bozulmaz
//   Kapasite ve overflow (en eskinin üstüne yaz)
//     - FillingToCapacityDropsNothing          tam kapasite overflow sayılmaz, dropped 0
//     - PushOnFullBufferOverwritesOldest       full buffer'a push en eskiyi overwrite eder, dropped 1
//     - AfterOverflowNewestSlotsSurviveInOrder overflow sonrası en yeni kSlotCount kayıt sırayla gelir
//     - OverwriteAcrossMultipleLapsKeepsNewest 3 tur overwrite: dropped 2 x kSlotCount, son tur kalır
//     - OverflowAfterPartialPopKeepsFifoOrder  kısmi pop + overflow: oldest index formülünün diğer path'i
//   dropped sayacı
//     - TakeDroppedResetsCounter               takeDropped() değeri döndürür ve sıfırlar
//   Yeniden kullanım
//     - BufferIsReusableAfterDraining          boşaltılan buffer tekrar kullanılabilir, eski kayıt gelmez
//
// Bilinçli olarak test edilmeyenler:
//   - Thread safety: Buffer thread-safe değil (sadece container); ThreatLogger testlerinde ele alınır.

#include <gtest/gtest.h>
#include "Buffer.h"

namespace {

// Slotları timestampMs ile ayırt ediyoruz; text içeriği bu testler için önemsiz.
Buffer::Slot makeSlot(std::int64_t timestampMs) {
    Buffer::Slot slot {};
    slot.timestampMs = timestampMs;
    return slot;
}

// Sayaç/indeks (size_t) → timestamp (int64_t). Test verisi olarak indeksi timestamp'e
// yazıyoruz; işaret dönüşümü tek yerde ve açıkça yapılsın.
std::int64_t ts(std::size_t index) {
    return static_cast<std::int64_t>(index);
}

// Buffer'ı kapasitesinin tamamına kadar doldurur: timestamp'ler first, first+1, ...
void fill(Buffer& buffer, std::size_t first = 0) {
    for (std::size_t i = 0; i < Buffer::kSlotCount; ++i) {
        buffer.push(makeSlot(ts(first + i)));
    }
}

} // namespace

TEST(BufferTest, PopOnEmptyBufferReturnsFalse) {
    Buffer buffer;
    Buffer::Slot slot {};

    const bool popped = buffer.pop(slot);

    EXPECT_FALSE(popped);
    EXPECT_EQ(buffer.count(), 0u);
}

TEST(BufferTest, PushAndPopASlot) {
    Buffer buffer;
    const Buffer::Slot pushed {.timestampMs = 320432523, .len = 4, .text = {'t', 'e', 's', 't'}};

    buffer.push(pushed);
    Buffer::Slot popped {};
    const bool ok = buffer.pop(popped);

    // pop() fail olursa popped boş kalır, içine bakmanın anlamı yok: ASSERT ile testi durdur.
    ASSERT_TRUE(ok);
    EXPECT_EQ(popped.timestampMs, pushed.timestampMs);
    EXPECT_EQ(popped.len, pushed.len);
    EXPECT_EQ(popped.text, pushed.text);
    EXPECT_EQ(buffer.count(), 0u);
}

// Her push count'u bir artırır, her pop bir azaltır.
TEST(BufferTest, CountTracksPushAndPop) {
    Buffer buffer;

    buffer.push(makeSlot(1));
    buffer.push(makeSlot(2));
    buffer.push(makeSlot(3));
    EXPECT_EQ(buffer.count(), 3u);

    Buffer::Slot slot {};
    ASSERT_TRUE(buffer.pop(slot));
    EXPECT_EQ(buffer.count(), 2u);
}

// FIFO: kayıtlar eklendiği sırayla çıkar, pop() her zaman en eskiyi verir.
TEST(BufferTest, PopReturnsSlotsInFifoOrder) {
    Buffer buffer;
    for (std::int64_t ts = 1; ts <= 3; ++ts) {
        buffer.push(makeSlot(ts));
    }

    for (std::int64_t expected = 1; expected <= 3; ++expected) {
        Buffer::Slot slot {};
        ASSERT_TRUE(buffer.pop(slot)) << "pop #" << expected << " fail oldu";
        EXPECT_EQ(slot.timestampMs, expected);
    }
    EXPECT_EQ(buffer.count(), 0u);
}

// Tam kapasiteye kadar doldurmak overflow değildir: hiçbir kayıt drop edilmez.
TEST(BufferTest, FillingToCapacityDropsNothing) {
    Buffer buffer;

    fill(buffer);

    EXPECT_EQ(buffer.count(), Buffer::kSlotCount);
    EXPECT_EQ(buffer.takeDropped(), 0u);
}

// Buffer doluyken push: en eski kayıt overwrite edilir, count kapasitede kalır,
// dropped sayacı artar.
TEST(BufferTest, PushOnFullBufferOverwritesOldest) {
    Buffer buffer;
    fill(buffer, 0);                       // 0 .. kSlotCount-1

    buffer.push(makeSlot(1000));           // 0 overwrite edilmeli

    EXPECT_EQ(buffer.count(), Buffer::kSlotCount);
    EXPECT_EQ(buffer.takeDropped(), 1u);

    Buffer::Slot slot {};
    ASSERT_TRUE(buffer.pop(slot));
    EXPECT_EQ(slot.timestampMs, 1);        // en eski artık 1
}

// Overflow sonrası buffer'ı boşaltınca hayatta kalanlar sırayla gelir ve
// en son push edilen kayıt en sonda çıkar.
TEST(BufferTest, AfterOverflowNewestSlotsSurviveInOrder) {
    Buffer buffer;
    constexpr std::size_t kExtra = 5;
    for (std::size_t i = 0; i < Buffer::kSlotCount + kExtra; ++i) {
        buffer.push(makeSlot(ts(i)));
    }

    EXPECT_EQ(buffer.takeDropped(), kExtra);
    for (std::size_t expected = kExtra; expected < Buffer::kSlotCount + kExtra; ++expected) {
        Buffer::Slot slot {};
        ASSERT_TRUE(buffer.pop(slot));
        EXPECT_EQ(slot.timestampMs, ts(expected));
    }
    EXPECT_EQ(buffer.count(), 0u);
}

// Birden fazla tur overwrite: kapasitenin 3 katı push edilince head defalarca
// başa döner, count hep full kalır. Sadece son kSlotCount kayıt hayatta kalmalı.
TEST(BufferTest, OverwriteAcrossMultipleLapsKeepsNewest) {
    Buffer buffer;
    constexpr std::size_t kLaps = 3;
    constexpr std::size_t kTotal = Buffer::kSlotCount * kLaps;
    for (std::size_t i = 0; i < kTotal; ++i) {
        buffer.push(makeSlot(ts(i)));
    }

    EXPECT_EQ(buffer.count(), Buffer::kSlotCount);
    EXPECT_EQ(buffer.takeDropped(), Buffer::kSlotCount * (kLaps - 1));
    for (std::size_t expected = kTotal - Buffer::kSlotCount; expected < kTotal; ++expected) {
        Buffer::Slot slot {};
        ASSERT_TRUE(buffer.pop(slot));
        ASSERT_EQ(slot.timestampMs, ts(expected));
    }
    EXPECT_EQ(buffer.count(), 0u);
}

// Önce kısmi pop, sonra overflow: pop()'taki oldest index formülü
// (m_head + kSlotCount - m_count) % kSlotCount farklı bir head/count
// kombinasyonuyla çalışır. Sıfırdan doldurma testleri bu path'i görmez.
TEST(BufferTest, OverflowAfterPartialPopKeepsFifoOrder) {
    Buffer buffer;
    fill(buffer, 0);                                   // 0 .. 127

    constexpr std::size_t kPopped = 50;
    for (std::size_t expected = 0; expected < kPopped; ++expected) {
        Buffer::Slot slot {};
        ASSERT_TRUE(buffer.pop(slot));
        ASSERT_EQ(slot.timestampMs, ts(expected));
    }                                                  // kalan: 50 .. 127

    // 50 push boş yeri doldurur, sonraki 10 push en eski 10 kaydı (50 .. 59) overwrite eder.
    constexpr std::size_t kOverflow = 10;
    constexpr std::size_t firstNew = Buffer::kSlotCount;  // 128
    constexpr std::size_t lastNew = firstNew + kPopped + kOverflow;
    for (std::size_t i = firstNew; i < lastNew; ++i) {
        buffer.push(makeSlot(ts(i)));
    }

    EXPECT_EQ(buffer.count(), Buffer::kSlotCount);
    EXPECT_EQ(buffer.takeDropped(), kOverflow);
    for (std::size_t expected = kPopped + kOverflow; expected < lastNew; ++expected) {
        Buffer::Slot slot {};
        ASSERT_TRUE(buffer.pop(slot));
        ASSERT_EQ(slot.timestampMs, ts(expected));         // 60 .. 127, sonra 128 .. 187
    }
    EXPECT_EQ(buffer.count(), 0u);
}

// takeDropped() sayacı döndürür ve sıfırlar: ikinci çağrı 0 vermeli.
TEST(BufferTest, TakeDroppedResetsCounter) {
    Buffer buffer;
    fill(buffer);
    buffer.push(makeSlot(1000));
    buffer.push(makeSlot(1001));

    EXPECT_EQ(buffer.takeDropped(), 2u);
    EXPECT_EQ(buffer.takeDropped(), 0u);
}

// Wrap-around: head array'in sonundan başa döndüğünde de FIFO bozulmamalı.
// Push/pop dönüşümlü yapılınca indeksler kapasiteyi defalarca geçer.
TEST(BufferTest, FifoOrderSurvivesWrapAround) {
    Buffer buffer;
    std::int64_t nextPush = 0;
    std::int64_t nextPop = 0;

    // Önce yarıya kadar doldur, sonra her adımda bir push bir pop yap.
    for (std::size_t i = 0; i < Buffer::kSlotCount / 2; ++i) {
        buffer.push(makeSlot(nextPush++));
    }
    for (std::size_t i = 0; i < Buffer::kSlotCount * 3; ++i) {
        buffer.push(makeSlot(nextPush++));
        Buffer::Slot slot {};
        ASSERT_TRUE(buffer.pop(slot));
        ASSERT_EQ(slot.timestampMs, nextPop++) << "adım " << i;
    }

    EXPECT_EQ(buffer.count(), Buffer::kSlotCount / 2);
    EXPECT_EQ(buffer.takeDropped(), 0u);
}

// Boşaltılan buffer tekrar kullanılabilir: eski kayıtlar geri gelmez.
TEST(BufferTest, BufferIsReusableAfterDraining) {
    Buffer buffer;
    fill(buffer, 0);
    Buffer::Slot slot {};
    while (buffer.pop(slot)) {}

    buffer.push(makeSlot(500));

    ASSERT_TRUE(buffer.pop(slot));
    EXPECT_EQ(slot.timestampMs, 500);
    EXPECT_FALSE(buffer.pop(slot));
}
