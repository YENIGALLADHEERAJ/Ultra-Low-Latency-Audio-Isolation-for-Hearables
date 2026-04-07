// circular_buffer.hpp
// Lock-free, single-producer / single-consumer (SPSC) circular buffer
// optimised for real-time audio I/O.
//
// Design goals
// ─────────────
//  • Zero dynamic memory allocation after construction.
//  • Wait-free for both the producer (ingest) and consumer (pop) paths.
//  • Cache-line-separated head/tail counters to eliminate false sharing.
//  • Power-of-two Capacity so the modulo reduction compiles to a bitmask.
//  • The buffer stores *samples*, not bytes; the element type T is a
//    template parameter (typically float for normalised PCM).
//
// Thread-safety contract
// ──────────────────────
//  Exactly ONE thread may call push() / push_overwrite() at any time.
//  Exactly ONE (different) thread may call pop() / peek() at any time.
//  All other combinations require external synchronisation.

#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace audio_engine {

/// \tparam T        Element type (must be trivially copyable, e.g. float).
/// \tparam Capacity Storage capacity in number of elements.
///                  MUST be a power of two.
template <typename T, std::size_t Capacity>
class CircularBuffer {
    static_assert(std::is_trivially_copyable_v<T>,
                  "CircularBuffer requires a trivially copyable element type.");
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "CircularBuffer capacity must be a power of two.");

public:
    CircularBuffer() noexcept : head_(0), tail_(0) {}

    // Non-copyable, non-movable (contains atomics).
    CircularBuffer(const CircularBuffer&) = delete;
    CircularBuffer& operator=(const CircularBuffer&) = delete;

    // ── Producer API (call from the single producer thread) ──────────────────

    /// Try to push \p count elements from \p data.
    /// \returns true on success; false if there is not enough free space
    ///          (no partial writes are performed).
    [[nodiscard]] bool push(const T* data, std::size_t count) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t free_slots = Capacity - (head - tail);
        if (count > free_slots) {
            return false;
        }
        write_elements(data, count, head);
        head_.store(head + count, std::memory_order_release);
        return true;
    }

    /// Push \p count elements, silently dropping the oldest data if the buffer
    /// is full (overwrite mode).  Safe to call from the producer thread only.
    void push_overwrite(const T* data, std::size_t count) noexcept {
        assert(count <= Capacity && "count must not exceed buffer capacity");
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t used = head - tail;
        const std::size_t free_slots = Capacity - used;

        if (count > free_slots) {
            // Advance tail to make room – drop oldest samples.
            const std::size_t drop = count - free_slots;
            tail_.store(tail + drop, std::memory_order_release);
        }
        write_elements(data, count, head_.load(std::memory_order_relaxed));
        head_.store(head + count, std::memory_order_release);
    }

    // ── Consumer API (call from the single consumer thread) ──────────────────

    /// Try to read \p count elements into \p out.
    /// \returns true on success; false if fewer than \p count elements are
    ///          available (no partial reads are performed).
    [[nodiscard]] bool pop(T* out, std::size_t count) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (head - tail < count) {
            return false;
        }
        read_elements(out, count, tail);
        tail_.store(tail + count, std::memory_order_release);
        return true;
    }

    /// Non-destructive read of the next \p count elements into \p out.
    /// \returns true on success; false if fewer than \p count elements are
    ///          available.
    [[nodiscard]] bool peek(T* out, std::size_t count) const noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (head - tail < count) {
            return false;
        }
        read_elements(out, count, tail);
        return true;
    }

    // ── Queries (may be called from either thread) ────────────────────────────

    /// Approximate number of elements available for reading.
    [[nodiscard]] std::size_t available() const noexcept {
        return head_.load(std::memory_order_acquire) -
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool empty() const noexcept { return available() == 0; }

    /// Returns true when the buffer holds exactly Capacity elements.
    [[nodiscard]] bool full() const noexcept {
        return available() == Capacity;
    }

    /// Total element capacity (compile-time constant).
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

    /// Discard all buffered data.  Must only be called when no concurrent
    /// push/pop is in progress.
    void reset() noexcept {
        tail_.store(0, std::memory_order_relaxed);
        head_.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t MASK = Capacity - 1;

    // Write helpers – handle wrap-around with at most two memcpy calls.
    void write_elements(const T* src, std::size_t count,
                        std::size_t head) noexcept {
        const std::size_t offset = head & MASK;
        const std::size_t first_chunk =
            (offset + count <= Capacity) ? count : (Capacity - offset);
        std::memcpy(storage_.data() + offset, src,
                    first_chunk * sizeof(T));
        if (first_chunk < count) {
            std::memcpy(storage_.data(), src + first_chunk,
                        (count - first_chunk) * sizeof(T));
        }
    }

    void read_elements(T* dst, std::size_t count,
                       std::size_t tail) const noexcept {
        const std::size_t offset = tail & MASK;
        const std::size_t first_chunk =
            (offset + count <= Capacity) ? count : (Capacity - offset);
        std::memcpy(dst, storage_.data() + offset,
                    first_chunk * sizeof(T));
        if (first_chunk < count) {
            std::memcpy(dst + first_chunk, storage_.data(),
                        (count - first_chunk) * sizeof(T));
        }
    }

    // Pad head_ and tail_ to separate cache lines (64 bytes) to prevent
    // false sharing between the producer and consumer cores.
    alignas(64) std::atomic<std::size_t> head_;
    alignas(64) std::atomic<std::size_t> tail_;

    std::array<T, Capacity> storage_{};
};

} // namespace audio_engine
