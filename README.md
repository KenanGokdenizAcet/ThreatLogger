# ThreatLogger

Birden fazla thread'den gelen threat kayıtlarını (missile, radar, laser) bir dosyaya ya da konsola yazan asenkron bir logger. Hedef bir gömülü / RTOS sistemi olduğu için tasarımdaki kararların çoğu şu dört kurala dayanıyor:

1. `operator<<` çağıran thread I/O beklemiyor. Threat, mutex'in dışında metne çevriliyor; mutex'in içinde sadece sabit boyutlu bir slot ring buffer'a kopyalanıyor. Buffer doluysa producer yer açılmasını beklemiyor, en eski kaydın üzerine yazıyor. Mutex'i alırken başka producer'larla çekişme olursa kısa bir bekleme yine olabiliyor.
2. `init()`'ten sonra heap allocation yapılmıyor. Buffer'ların hepsi `std::array`; sayılar `std::to_chars` ile, timestamp `<chrono>` ile yazılıyor. Bu, `rehis_heapcheck` ile ölçülüyor ([HeapCheck](#heapcheck-sadece-macos)).
3. Ürün kodunda exception yok. `rehis_core` ve demo uygulaması `-fno-exceptions` ile derleniyor; `throw` / `try` yazılırsa derlenmiyor.
4. I/O ayrı bir writer thread'inde yapılıyor. Writer, `flushPeriod` dolunca ya da buffer'daki kayıt sayısı `flushThreshold`'a ulaşınca birikenleri tek `write()` ile yazıyor. Üzerine yazılan kayıtların sayısı log'a ayrı bir satır olarak düşüyor.

Sunum `docs/ThreatLogger_Sunum.pptx`'te.

## Gereksinimler

- C++20 derleyici
- CMake 3.20+
- POSIX sistem (macOS ya da Linux): çıktı için `open` / `write` kullanılıyor
- GoogleTest, sadece testler için. Kuruluysa o kullanılıyor, değilse configure sırasında v1.17.0 indiriliyor.

Geliştirme ve bütün ölçümler macOS 27 / Apple clang 21 / Apple M4 Pro üzerinde yapıldı. Linux'ta denenmedi. `<chrono>` takvim tipleri ve `double` için `std::to_chars` gerektiği için GCC 11 ve üstüyle derlenmesi bekleniyor.

## Derleme

```bash
cmake -S . -B build
cmake --build build -j
```

Build type verilmezse `Release` derleniyor; Debug için `-DCMAKE_BUILD_TYPE=Debug`.

| CMake seçeneği | Varsayılan | |
|---|---|---|
| `REHIS_BUILD_TESTS` | `ON` | `rehis_tests` |
| `REHIS_BUILD_BENCH` | `ON` | `rehis_bench`, macOS'ta ayrıca `rehis_heapcheck` |
| `REHIS_WARNINGS_AS_ERRORS` | `OFF` | `-Werror`. Proje `-Wall -Wextra -Wpedantic -Wshadow` ile warning vermeden derleniyor |

İnternet yoksa testleri kapatın (`-DREHIS_BUILD_TESTS=OFF`) ya da GoogleTest'in zip'ini başka bir yerde açıp klasörünü verin: `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest-1.17.0`.

## Demo

`aselsan_rehis`, 4 thread'in 10 tur boyunca threat log'ladığı bir demo. Her turda 11 kayıt, toplam 110 satır.

```bash
./build/aselsan_rehis                          # konsol
./build/aselsan_rehis --log-console            # konsol
./build/aselsan_rehis --log-file=threats.log   # dosya, varsa sonuna ekler
```

Dosya açılamazsa (klasör yok, yetki yok, yol bir klasör) program devam ediyor: sebep stderr'e yazılıyor ve log'lar konsola gidiyor. `--log-file=` boş verilirse ya da argüman tanınmazsa da aynısı oluyor.

```
[2026.11.16_15:51:10.023] [MISSILE] id=0 az=30 el=5 speed=850 velocity=900
[2026.11.16_15:51:10.023] [RADAR] id=3 az=120 el=2 freq=9.4e+09 signal=-62
[2026.11.16_15:51:10.023] [LASER] id=6 az=60 el=3 wavelength=1064 prf=20
```

Buffer taştığında yazılan satır şöyle (benchmark'ın `burst` senaryosundan):

```
[2026.11.16_15:51:10.385] [LOGGER] 41 record(s) dropped: buffer overflow
```

Threat satırlarındaki zaman, kaydın log'landığı an; writer'ın yazdığı an değil. "dropped" satırı ise writer'ın yazdığı anın zamanını taşıyor ve batch'in başına geliyor, o yüzden log'da bir satırlığına zaman sırası bozulabiliyor. Zaman UTC + `utcOffset` olarak yazılıyor; varsayılan Türkiye saati (UTC+3).

## Kullanım

```cpp
#include "ThreatLogger.h"

int main() {
    ThreatLoggerConfig cfg;              // bütün alanların varsayılanı var
    cfg.filePath = "threats.log";        // nullptr: konsol
    ThreatLogger::init(cfg);             // program başında, tek thread'den, bir kez

    ThreatLogger& logger = ThreatLogger::getInstance();
    const MissileThreat missile { 30.0F, 5.0F, 850.0, 900.0 };
    const RadarThreat radar { 120.0F, 2.0F, 9.4e9, -62.0 };
    logger << missile << radar;          // herhangi bir thread'den

    logger.shutdown();                   // bekleyen kayıtları yazar, dosyayı kapatır
}
```

- `init()` iki kez çağrılırsa ya da `getInstance()` `init()`'ten önce çağrılırsa program stderr'e sebebini yazıp `std::abort()` ile kapanıyor. `assert` kullanılmadığı için Release build'de de böyle.
- `shutdown()` çağrılmazsa static destructor'da çağrılıyor. `shutdown()`'dan sonra gelen kayıtlar yok sayılıyor.
- Yeni bir threat tipi için `Threat`'ten türetip `format()`'ı yazmak yeterli. `format()` `snprintf` sözleşmesine uymalı: `cap`'ten fazla yazmamalı, yazmak istediği uzunluğu döndürmeli. `kTextSize`'a sığmayan metin kesiliyor.

### Konfigürasyon

`ThreatLoggerConfig` (`src/ThreatLogger.h`):

| Alan | Varsayılan | |
|---|---|---|
| `filePath` | `nullptr` | Log dosyası; `nullptr` ise konsol |
| `flushPeriod` | 200 ms | Bir kaydın yazılmadan önce en fazla bekleyeceği süre |
| `flushThreshold` | 96 | Buffer'da bu kadar kayıt birikince writer period'u beklemeden yazıyor. `kSlotCount`'tan büyük verilirse hiç tetiklenmiyor, sadece period kalıyor |
| `utcOffset` | +3 saat | Log zamanı = UTC + offset. Sabit; yaz saati uygulanmıyor |

Derleme zamanı sabitleri `src/Constants.h`'ta:

| Sabit | Değer | |
|---|---|---|
| `kSlotCount` | 128 | Ring buffer'daki slot sayısı |
| `kTextSize` | 128 | Bir kaydın metni için ayrılan byte, `'\0'` dahil |
| `kLineBufferSize` | 8192 | Writer'ın tek `write()` ile yazdığı en fazla byte |

## Testler

```bash
ctest --test-dir build --output-on-failure
./build/rehis_tests                            # ya da doğrudan
```

71 test, dört dosyada:

| Dosya | Ne test ediliyor |
|---|---|
| `tests/BufferTest.cpp` | FIFO sırası, wrap-around, taşmada en eskinin üzerine yazılması, dropped sayacı |
| `tests/ThreatTest.cpp` | `format()` çıktısı; uç değerlerde (denormal, max, ±0, inf, NaN, 6. basamakta yuvarlama) `to_chars`'ın `%g` ile aynı metni üretmesi; en uzun metnin slot'a sığması; threat ID |
| `tests/ThreatLoggerTest.cpp` | `init` / `getInstance` sırası, config varsayılanları, dosya / konsol seçimi |
| `tests/ThreatLoggerBehaviorTest.cpp` | Satır formatı, timestamp ve `utcOffset`, shutdown, taşma, flush tetikleri, 8 thread ile eşzamanlı log'lama |

Logger singleton ve process başına bir kez init edilebiliyor. Bu yüzden `init()` çağıran testler death test olarak ayrı child process'lerde çalışıyor. Zamana bağlı testlerde alt sınırlar sıkı, üst sınırlar bilerek geniş tutuldu ki yavaş bir makinede de geçsinler.

Testler ASan + UBSan ve ThreadSanitizer altında da hatasız geçiyor. TSan için:

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build-tsan --target rehis_tests -j
TSAN_OPTIONS=halt_on_error=1 ./build-tsan/rehis_tests
```

## Benchmark

`rehis_bench` logger'ı uçtan uca çalıştırıp her adımın süresini ölçüyor. Ölçüm kodu `THREAT_LOGGER_PROFILING` ile derlenen ayrı bir kütüphanede (`rehis_core_profiled`); ürün kütüphanesi bu kodu içermiyor. Benchmark, build type ne olursa olsun `-O2` ile derleniyor.

```bash
./build/rehis_bench --scenario=normal          # normal | stress | burst
./build/rehis_bench --scenario=stress --log-console
./build/rehis_bench --scenario=burst --out=sonuclar/burst
```

| Senaryo | Yük |
|---|---|
| `normal` | Demo ile aynı: 4 thread, 10 tur, turlar arası 20 ms, 110 kayıt |
| `stress` | 4 thread × 5000 kayıt, arada bekleme yok. Contention'ı ve taşma yolunu zorlamak için |
| `burst` | 4 thread aynı anda 100'er kayıt basıyor; 5 kez, 50 ms arayla. Her burst 400 kayıt, buffer 128 slot |

Program stdout'a bir özet tablo basıyor (`operator<<`, `format()`, lock wait / hold, writer copy hold, `write()`, kayıp, uçtan uca gecikme; min / avg / p50 / p99 / max). Ham veriler `--out` klasörüne gidiyor, varsayılanı `bench_results/<senaryo>/`: `producer.csv`, `writer.csv`, `e2e.csv`.

## HeapCheck (sadece macOS)

`rehis_heapcheck`, init'ten sonraki hiçbir adımın heap allocation yapmadığını kontrol ediyor. `malloc`, `calloc` ve `realloc` çağrıları bir dylib üzerinden (dyld interposing) yakalanıp her adım için ayrı sayılıyor. Log'lar varsayılan olarak `/dev/null`'a yazılıyor; `--log-file=PATH` ile dosyaya yönlendirilebiliyor.

```bash
./build/rehis_heapcheck
```

```
[HeapCheck] sample error: No such file or directory
ThreatLogger::init (thread, file)              mallocs=3
Threat::format, first call on a new task       mallocs=0
logger <<, first batch + writer (timestamp)    mallocs=0
logger <<, 3000 records + writer               mallocs=0
fprintf(stderr) + strerror (console message)   mallocs=0
shutdown                                       mallocs=0
OK: no heap allocation after init
```

İlk satır bir hata değil: `strerror` + `fprintf(stderr)` yolunun allocation yapmadığını görmek için bilerek basılan örnek mesaj. Init'ten sonraki bir adımda allocation sayılırsa program `FAIL` yazıp 1 ile çıkıyor.

Sayaç `posix_memalign`, `aligned_alloc` ve `malloc_zone_*` çağrılarını görmüyor. Bu yüzden init satırındaki sayı gerçek sayıdan düşük olabilir. Init sonrası adımlar bu fonksiyonlar da sayılarak ayrıca kontrol edildi ve yine 0 çıktı, ama o sayaç projede yok.

## Klasör yapısı

```
src/                      ürün kodu (rehis_core)
  Constants.h             buffer boyutları ve config varsayılanları
  Buffer.h                sabit boyutlu ring buffer; thread-safe değil, kilidi ThreatLogger tutuyor
  Threat.h / .cpp         Threat ve MissileThreat, RadarThreat, LaserThreat
  ThreatLogger.h / .cpp   producer tarafı, writer thread, dosya / konsol çıktısı
  main.cpp                demo (rehis_core'a girmiyor)
tests/                    GoogleTest testleri
bench/LoggerBench.cpp     benchmark
bench/LoggerProfiler.*    ölçüm noktaları (sadece rehis_core_profiled'da)
bench/heapcheck/          heap allocation sayacı (macOS)
docs/                     sunum
```
