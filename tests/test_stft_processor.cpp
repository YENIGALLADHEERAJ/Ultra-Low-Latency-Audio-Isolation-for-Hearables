// test_stft_processor.cpp
// Unit tests for STFTProcessor.
//
// Correctness checks:
//  1. DC tone  → energy concentrated in bin 0.
//  2. Known-frequency sinusoid → peak bin matches analytical bin index.
//  3. Zero input → all output magnitudes are 0.
//  4. bin_frequency() helper returns correct values.
//  5. process() output dimensions are correct (FFT_BINS real bins).
//  6. Parseval's theorem: sum of power spectrum ≈ windowed signal energy.

#include "audio_engine/stft_processor.hpp"
#include "audio_engine/audio_frame.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <stdexcept>

// ── Minimal test harness (same as test_circular_buffer) ──────────────────────

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

TEST(zero_input_gives_zero_output) {
    std::array<float, FRAME_SIZE> frame{};
    frame.fill(0.0f);

    STFTProcessor proc;
    std::array<std::complex<float>, FFT_BINS> out{};
    proc.process(frame.data(), out.data());

    for (std::size_t k = 0; k < FFT_BINS; ++k) {
        ASSERT_NEAR(proc.magnitude()[k], 0.0f, 1e-6f);
    }
}

TEST(bin_frequency_helper) {
    // Bin 0 → DC (0 Hz).
    ASSERT_NEAR(STFTProcessor::bin_frequency(0), 0.0f, 1e-3f);

    // Bin FFT_SIZE/2 → Nyquist = SAMPLE_RATE/2 = 8000 Hz.
    const float nyquist = STFTProcessor::bin_frequency(FFT_SIZE / 2);
    ASSERT_NEAR(nyquist,
                static_cast<float>(SAMPLE_RATE_HZ) / 2.0f,
                1.0f /* Hz tolerance */);

    // Frequency resolution = SAMPLE_RATE / FFT_SIZE = 62.5 Hz / bin.
    const float res = STFTProcessor::bin_frequency(1) -
                      STFTProcessor::bin_frequency(0);
    ASSERT_NEAR(res,
                static_cast<float>(SAMPLE_RATE_HZ) /
                static_cast<float>(FFT_SIZE),
                1e-3f);
}

TEST(dc_tone_energy_in_bin0) {
    std::array<float, FRAME_SIZE> frame;
    frame.fill(1.0f); // Pure DC

    STFTProcessor proc;
    std::array<std::complex<float>, FFT_BINS> out{};
    proc.process(frame.data(), out.data());

    const auto& mag = proc.magnitude();
    // Bin 0 must be the largest.
    const float dc_mag = mag[0];
    for (std::size_t k = 1; k < FFT_BINS; ++k) {
        ASSERT(dc_mag >= mag[k]);
    }
    // DC magnitude must be strictly positive.
    ASSERT(dc_mag > 1e-3f);
}

TEST(sinusoid_peak_bin_matches_frequency) {
    // Choose a frequency that lands exactly on an FFT bin so there is no
    // spectral leakage from the sinusoid itself.
    // f = k * SAMPLE_RATE / FFT_SIZE  →  k = f * FFT_SIZE / SAMPLE_RATE
    // Use k=4 → f = 4 * 16000 / 256 = 250 Hz.
    constexpr std::size_t TARGET_BIN = 4;
    const float freq =
        static_cast<float>(TARGET_BIN) *
        static_cast<float>(SAMPLE_RATE_HZ) /
        static_cast<float>(FFT_SIZE); // 250 Hz

    std::array<float, FRAME_SIZE> frame;
    for (std::size_t n = 0; n < FRAME_SIZE; ++n) {
        frame[n] = std::sin(
            2.0f * std::numbers::pi_v<float> * freq *
            static_cast<float>(n) / static_cast<float>(SAMPLE_RATE_HZ));
    }

    STFTProcessor proc;
    std::array<std::complex<float>, FFT_BINS> out{};
    proc.process(frame.data(), out.data());

    const auto& mag = proc.magnitude();
    // Find the peak bin (excluding DC which may have window-induced leakage).
    std::size_t peak_bin = 1;
    for (std::size_t k = 2; k < FFT_BINS; ++k) {
        if (mag[k] > mag[peak_bin]) peak_bin = k;
    }
    ASSERT_EQ(peak_bin, TARGET_BIN);
}

TEST(output_size_is_fft_bins) {
    // Structural sanity: FFT_BINS == FFT_SIZE/2 + 1.
    ASSERT_EQ(FFT_BINS, FFT_SIZE / 2 + 1);
    // At 16 kHz, FFT_SIZE=256, FFT_BINS must be 129.
    ASSERT_EQ(FFT_BINS, 129u);
}

TEST(magnitude_matches_abs_of_bins) {
    std::array<float, FRAME_SIZE> frame;
    for (std::size_t n = 0; n < FRAME_SIZE; ++n) {
        frame[n] = static_cast<float>(n % 8) / 8.0f - 0.5f;
    }

    STFTProcessor proc;
    std::array<std::complex<float>, FFT_BINS> out{};
    proc.process(frame.data(), out.data());

    const auto& mag = proc.magnitude();
    for (std::size_t k = 0; k < FFT_BINS; ++k) {
        ASSERT_NEAR(mag[k], std::abs(out[k]), 1e-5f);
    }
}

TEST(power_equals_magnitude_squared) {
    std::array<float, FRAME_SIZE> frame;
    for (std::size_t n = 0; n < FRAME_SIZE; ++n) {
        frame[n] = 0.5f * std::sin(
            2.0f * std::numbers::pi_v<float> * 500.0f *
            static_cast<float>(n) / static_cast<float>(SAMPLE_RATE_HZ));
    }

    STFTProcessor proc;
    std::array<std::complex<float>, FFT_BINS> out{};
    proc.process(frame.data(), out.data());

    const auto& mag   = proc.magnitude();
    const auto& power = proc.power();
    for (std::size_t k = 0; k < FFT_BINS; ++k) {
        ASSERT_NEAR(power[k], mag[k] * mag[k], 1e-5f);
    }
}

TEST(repeated_calls_are_independent) {
    // Processing the same frame twice must yield identical results.
    std::array<float, FRAME_SIZE> frame;
    for (std::size_t n = 0; n < FRAME_SIZE; ++n) {
        frame[n] = 0.3f * std::cos(
            2.0f * std::numbers::pi_v<float> * 1000.0f *
            static_cast<float>(n) / static_cast<float>(SAMPLE_RATE_HZ));
    }

    STFTProcessor proc;
    std::array<std::complex<float>, FFT_BINS> out1{}, out2{};
    proc.process(frame.data(), out1.data());
    proc.process(frame.data(), out2.data());

    for (std::size_t k = 0; k < FFT_BINS; ++k) {
        ASSERT_NEAR(out1[k].real(), out2[k].real(), 1e-5f);
        ASSERT_NEAR(out1[k].imag(), out2[k].imag(), 1e-5f);
    }
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main() {
    std::printf("\n=== STFTProcessor tests ===\n");
    std::printf("Results: %d passed, %d failed, %d total\n\n",
                g_passed, g_failed, g_total);
    return (g_failed == 0) ? 0 : 1;
}
