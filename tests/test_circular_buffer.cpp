// test_circular_buffer.cpp
// Unit tests for CircularBuffer<float, N>.
//
// Uses a minimal, zero-dependency test harness (no external library required)
// so that the tests can run in resource-constrained CI and embedded toolchains.

#include "audio_engine/circular_buffer.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

// ── Minimal test harness ──────────────────────────────────────────────────────

static int g_total  = 0;
static int g_passed = 0;
static int g_failed = 0;

#define TEST(name)                                                             \
    static void name();                                                        \
    struct _Reg_##name {                                                       \
        _Reg_##name() { run_test(#name, name); }                              \
    } _reg_##name;                                                             \
    static void name()

static void run_test(const char* name, void (*fn)()) {
    ++g_total;
    bool ok = true;
    try {
        fn();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "  EXCEPTION: %s\n", ex.what());
        ok = false;
    } catch (...) {
        std::fprintf(stderr, "  UNKNOWN EXCEPTION\n");
        ok = false;
    }
    if (ok) {
        ++g_passed;
        std::printf("[PASS] %s\n", name);
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", name);
    }
}

#define ASSERT(expr)                                                           \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::fprintf(stderr, "  Assertion failed: %s  (%s:%d)\n",        \
                         #expr, __FILE__, __LINE__);                          \
            throw std::runtime_error("assertion failed");                     \
        }                                                                      \
    } while (false)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NEAR(a, b, eps) ASSERT(std::fabs((a) - (b)) < (eps))

// ── Tests ─────────────────────────────────────────────────────────────────────

using namespace audio_engine;

TEST(empty_buffer_initially) {
    CircularBuffer<float, 64> buf;
    ASSERT(buf.empty());
    ASSERT_EQ(buf.available(), 0u);
    ASSERT(!buf.full());
}

TEST(push_and_pop_single_element) {
    CircularBuffer<float, 64> buf;
    float in = 1.5f, out = 0.0f;
    ASSERT(buf.push(&in, 1));
    ASSERT_EQ(buf.available(), 1u);
    ASSERT(buf.pop(&out, 1));
    ASSERT_NEAR(out, in, 1e-6f);
    ASSERT(buf.empty());
}

TEST(push_many_pop_many) {
    constexpr std::size_t N = 64;
    CircularBuffer<float, 128> buf;
    std::vector<float> src(N), dst(N, 0.0f);
    for (std::size_t i = 0; i < N; ++i) src[i] = static_cast<float>(i);

    ASSERT(buf.push(src.data(), N));
    ASSERT_EQ(buf.available(), N);
    ASSERT(buf.pop(dst.data(), N));
    for (std::size_t i = 0; i < N; ++i) {
        ASSERT_NEAR(dst[i], src[i], 1e-6f);
    }
    ASSERT(buf.empty());
}

TEST(pop_fails_when_insufficient_data) {
    CircularBuffer<float, 64> buf;
    float tmp[10];
    ASSERT(!buf.pop(tmp, 1));

    float src[5] = {1, 2, 3, 4, 5};
    ASSERT(buf.push(src, 5));
    ASSERT(!buf.pop(tmp, 10));
    ASSERT_EQ(buf.available(), 5u); // data still intact after failed pop
}

TEST(push_fails_when_full) {
    CircularBuffer<float, 8> buf;
    float src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT(buf.push(src, 8));
    ASSERT(buf.full());

    float extra = 9.0f;
    ASSERT(!buf.push(&extra, 1)); // no room
    ASSERT_EQ(buf.available(), 8u);
}

TEST(push_overwrite_discards_oldest) {
    CircularBuffer<float, 4> buf;
    float src[4] = {1, 2, 3, 4};
    buf.push_overwrite(src, 4); // fills buffer

    float newer[2] = {5, 6};
    buf.push_overwrite(newer, 2); // must drop oldest 2

    float out[4];
    ASSERT(buf.pop(out, 4));
    ASSERT_NEAR(out[0], 3.0f, 1e-6f);
    ASSERT_NEAR(out[1], 4.0f, 1e-6f);
    ASSERT_NEAR(out[2], 5.0f, 1e-6f);
    ASSERT_NEAR(out[3], 6.0f, 1e-6f);
}

TEST(wrap_around_multiple_times) {
    CircularBuffer<float, 8> buf;
    for (int pass = 0; pass < 4; ++pass) {
        float src[6] = {
            static_cast<float>(pass * 6 + 0),
            static_cast<float>(pass * 6 + 1),
            static_cast<float>(pass * 6 + 2),
            static_cast<float>(pass * 6 + 3),
            static_cast<float>(pass * 6 + 4),
            static_cast<float>(pass * 6 + 5),
        };
        ASSERT(buf.push(src, 6));
        float dst[6];
        ASSERT(buf.pop(dst, 6));
        for (int i = 0; i < 6; ++i) {
            ASSERT_NEAR(dst[i], src[i], 1e-6f);
        }
    }
}

TEST(peek_does_not_consume) {
    CircularBuffer<float, 16> buf;
    float src[3] = {7.0f, 8.0f, 9.0f};
    ASSERT(buf.push(src, 3));

    float peeked[3];
    ASSERT(buf.peek(peeked, 3));
    ASSERT_EQ(buf.available(), 3u); // still there

    float popped[3];
    ASSERT(buf.pop(popped, 3));
    for (int i = 0; i < 3; ++i) {
        ASSERT_NEAR(peeked[i], popped[i], 1e-6f);
    }
    ASSERT(buf.empty());
}

TEST(reset_clears_buffer) {
    CircularBuffer<float, 16> buf;
    float src[8] = {1,2,3,4,5,6,7,8};
    ASSERT(buf.push(src, 8));
    ASSERT_EQ(buf.available(), 8u);
    buf.reset();
    ASSERT(buf.empty());
    ASSERT_EQ(buf.available(), 0u);
}

TEST(spsc_concurrent_push_pop) {
    // Basic SPSC correctness: producer fills the buffer in blocks of 80
    // (= FRAME_SIZE), consumer drains it in matching blocks.
    constexpr std::size_t CAP = 4096;
    constexpr std::size_t BLOCK = 80;
    constexpr std::size_t ITERATIONS = 1000;

    CircularBuffer<float, CAP> buf;
    std::atomic<bool> done{false};
    std::vector<float> consumed;
    consumed.reserve(BLOCK * ITERATIONS);

    std::thread producer([&] {
        for (std::size_t i = 0; i < ITERATIONS; ++i) {
            std::vector<float> blk(BLOCK, static_cast<float>(i));
            while (!buf.push(blk.data(), BLOCK)) {
                std::this_thread::yield();
            }
        }
        done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        std::vector<float> blk(BLOCK);
        std::size_t received = 0;
        while (received < ITERATIONS || !buf.empty()) {
            if (buf.pop(blk.data(), BLOCK)) {
                for (auto v : blk) consumed.push_back(v);
                ++received;
                if (received >= ITERATIONS &&
                    done.load(std::memory_order_acquire)) break;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(consumed.size(), BLOCK * ITERATIONS);
    // Spot-check: first BLOCK values must all equal 0.0f (iteration 0).
    for (std::size_t i = 0; i < BLOCK; ++i) {
        ASSERT_NEAR(consumed[i], 0.0f, 1e-6f);
    }
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main() {
    std::printf("\n=== CircularBuffer tests ===\n");
    std::printf("Results: %d passed, %d failed, %d total\n\n",
                g_passed, g_failed, g_total);
    return (g_failed == 0) ? 0 : 1;
}
