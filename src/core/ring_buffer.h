#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>

namespace litewp {

// Lock-free single-producer single-consumer ring buffer for compressed video packets
// Default: 4 slots, 256 KB per slot (compressed H.264/HEVC frames are ~30-150 KB)
template<size_t SlotSize = 262144, size_t SlotCount = 4>
class RingBuffer {
public:
    struct Packet {
        uint8_t data[SlotSize];
        size_t  size = 0;
        int64_t pts = 0;   // presentation timestamp
        bool    is_key = false;
    };

    // Producer: write packet to buffer. Returns false if full.
    bool Push(const uint8_t* data, size_t size, int64_t pts, bool is_key) {
        size_t write = m_write.load(std::memory_order_relaxed);
        size_t next = (write + 1) % SlotCount;
        if (next == m_read.load(std::memory_order_acquire)) return false; // full
        
        auto& slot = m_slots[write];
        size_t copy_size = (size < SlotSize) ? size : SlotSize;
        if (data && copy_size > 0) {
            std::memcpy(slot.data, data, copy_size);
        }
        slot.size = copy_size;
        slot.pts = pts;
        slot.is_key = is_key;
        
        m_write.store(next, std::memory_order_release);
        return true;
    }

    // Consumer: read packet from buffer. Returns nullptr if empty.
    const Packet* Peek() const {
        size_t read = m_read.load(std::memory_order_relaxed);
        if (read == m_write.load(std::memory_order_acquire)) return nullptr; // empty
        return &m_slots[read];
    }

    // Consumer: drop consumed packet
    void Pop() {
        size_t read = m_read.load(std::memory_order_relaxed);
        m_read.store((read + 1) % SlotCount, std::memory_order_release);
    }

    bool IsEmpty() const {
        return m_read.load(std::memory_order_acquire) == m_write.load(std::memory_order_acquire);
    }

    bool IsFull() const {
        size_t next = (m_write.load(std::memory_order_acquire) + 1) % SlotCount;
        return next == m_read.load(std::memory_order_acquire);
    }

    void Clear() {
        m_read.store(0, std::memory_order_release);
        m_write.store(0, std::memory_order_release);
    }

private:
    std::array<Packet, SlotCount> m_slots;
    std::atomic<size_t> m_read{0};
    std::atomic<size_t> m_write{0};
};

} // namespace litewp
