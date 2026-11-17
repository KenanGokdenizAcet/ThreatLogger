#ifndef ASELSAN_REHIS_BUFFER_H
#define ASELSAN_REHIS_BUFFER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "Constants.h"

//NOTE: not thread safe, only a custom container
class Buffer {
public:
    static constexpr std::size_t kTextSize  { constants::kTextSize };
    static constexpr std::size_t kSlotCount { constants::kSlotCount };

    static_assert(kSlotCount > 0, "ring buffer needs at least one slot");
    // Slot::len is uint16_t: the longest text (kTextSize - 1) must fit in it.
    static_assert(kTextSize - 1 <= std::numeric_limits<std::uint16_t>::max(), "kTextSize too large for Slot::len");

    struct Slot {
        std::int64_t timestampMs;              // wall-clock ms since epoch, taken at log time
        std::uint16_t len;                     // text length without '\0'
        std::array<char, kTextSize> text;
    };

    void push(const Slot& slot) noexcept {
        m_slots[m_head] = slot;
        m_head = (m_head + 1) % kSlotCount;

        if (m_count < kSlotCount) {
            ++m_count;
        } else {
            ++m_dropped;                       // when buffer is full, oldest record is overwritten
        }
    }

    bool pop(Slot& slot) noexcept {
        if (m_count == 0) {
            return false;
        }
        const std::size_t oldest = (m_head + kSlotCount - m_count) % kSlotCount;
        slot = m_slots[oldest];
        --m_count;
        return true;
    }

    [[nodiscard]] std::size_t count() const noexcept { return m_count; }

    // Returns the number of overwritten records since the last call and resets it.
    std::size_t takeDropped() noexcept {
        const std::size_t dropped = m_dropped;
        m_dropped = 0;
        return dropped;
    }

private:
    std::array<Slot, kSlotCount> m_slots {};
    std::size_t m_head { 0 };
    std::size_t m_count { 0 };
    std::size_t m_dropped { 0 };
};

#endif //ASELSAN_REHIS_BUFFER_H
