// MallocCounter: malloc / calloc / realloc çağrılarını sayan küçük bir dylib (sadece macOS).
//
// Nasıl çalışır?
//   macOS'un dynamic linker'ı (dyld), bir dylib'in "__DATA,__interpose" section'ındaki
//   {yeni fonksiyon, orijinal fonksiyon} çiftlerine bakar ve programdaki tüm malloc
//   çağrılarını bizim fonksiyonumuza yönlendirir (interposing). Bizim fonksiyonumuz sayacı
//   artırıp gerçek malloc'u çağırır.
//
//   Önemli: interpose sadece bir dylib içinden çalışır. Aynı kodu doğrudan programın
//   içine koyarsak dyld onu dikkate almaz ve sayaç hep 0 kalır (ilk denemede böyle oldu).
//
// Kullanım: ölçmek istediğin kodu mc_start() / mc_stop() arasına al.
//   mc_start();  ...ölçülen kod...  int n = mc_stop();   // n: aradaki allocation sayısı
//
// Sayaç global: ölçüm penceresi açıkken başka bir thread'in (örneğin writer'ın)
// yaptığı allocation'lar da sayılır. Writer'ı da ölçmek istediğimiz için bu isteniyor.

#include <atomic>
#include <cstdlib>

namespace {
std::atomic<bool> g_on { false };
std::atomic<int> g_count { 0 };

void count() noexcept {
    if (g_on.load(std::memory_order_relaxed)) {
        g_count.fetch_add(1, std::memory_order_relaxed);
    }
}

void* countingMalloc(std::size_t n) { count(); return std::malloc(n); }
void* countingCalloc(std::size_t a, std::size_t b) { count(); return std::calloc(a, b); }
void* countingRealloc(void* p, std::size_t n) { count(); return std::realloc(p, n); }

struct Interpose {
    const void* replacement;
    const void* original;
};

// dyld bu tabloyu okuyup malloc -> countingMalloc yönlendirmesini yapıyor.
// countingMalloc'un içindeki std::malloc çağrısı yönlendirilmez (dylib'in kendi çağrıları
// hariç tutulur), bu yüzden sonsuz döngü olmaz.
__attribute__((used, section("__DATA,__interpose")))
const Interpose kInterposeTable[] = {
    { reinterpret_cast<const void*>(&countingMalloc), reinterpret_cast<const void*>(&std::malloc) },
    { reinterpret_cast<const void*>(&countingCalloc), reinterpret_cast<const void*>(&std::calloc) },
    { reinterpret_cast<const void*>(&countingRealloc), reinterpret_cast<const void*>(&std::realloc) },
};
} // namespace

extern "C" void mc_start() noexcept {
    g_count.store(0);
    g_on.store(true);
}

extern "C" int mc_stop() noexcept {
    g_on.store(false);
    return g_count.load();
}
