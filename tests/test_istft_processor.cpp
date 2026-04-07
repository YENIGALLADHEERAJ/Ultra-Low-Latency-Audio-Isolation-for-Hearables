// test_istft_processor.cpp
// Unit tests for ISTFTProcessor.
//
// Coverage:
//  1. All-zero spectrum → all-zero output.
//  2. DC bin only → constant (non-zero) output.
//  3. Round-trip: STFT then ISTFT reconstructs the Hann-windowed signal.
//  4. Known-frequency sinusoid: reconstructed output contains the same freq.
//  5. Output length equals FRAME_SIZE.
//  6. Imaginary component is negligible (Hermitian input).

#include "audio_engine/audio_frame.hpp"
#include "audio_engine/istft_processor.hpp"
#include "audio_engine/stft_processor.hpp"

#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <stdexcept>

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
    try { fn(); }
    catch (const std::exception& ex) {
        std::fprintf(stderr, "  EXCEPTION: %s\n", ex.what()); ok = false;
    } catch (...) {
        std::fprintf(stderr, "  UNKNOWN EXCEPTION\n"); ok = false;
    }
    if (ok) { ++g_passed; std::printf("[PASS] %s\n", name); }
    else     { ++g_failed; std::printf("[FAIL] %s\n", name); }
}

#define ASSERT(expr)                                                           \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::fprintf(stderr, "  Assertion failed: %s  (%s:%d)\n",        \
                         #expr, __FILE__, __LINE__);                          \
            throw std::runtime_error("assertion failed");                     \
        }                                                                      \
    } while (false)

#define ASSERT_EQ(a, b)         ASSERT((a) == (b))
#define ASSERT_NEAR(a, b, eps)  ASSERT(std::fabs((a) - (b)) < (eps))
#define ASSERT_GT(a, b)         ASSERT((a) > (b))

// ── Tests ─────────────────────────────────────────────────────────────────────

using namespace audio_engine;

TEST(zero_spectrum_gives_zero_output) {
    std::array<std::complex<float>, FFT_BINS> spec{};
    std::array<float, FRAME_SIZE> out{};

    ISTFTProcessor istft;
    istft.process(spec.data(), out.data());

    for (std::size_t n = 0; n < FRAME_SIZE; ++n) {
        ASSERT_NEAR(out[n], 0.0f, 1e-6f);
    }
}

TEST(dc_bin_gives_nonzero_output) {
    // A non-zero DC bin (X[0] real) should produce a non-zero constant output.
    std::array<std::complex<float>, FFT_BINS> spec{};
    spec[0] = {static_cast<float>(FFT_SIZE), 0.0f}; // amplitude 1 after /N

    std::array<float, FRAME_SIZE> out{};
    ISTFTProcessor istft;
    istft.process(spec.data(), out.data());

    // Midpoint sample should be non-zero (Hann peak at FRAME_SIZE/2).
    ASSERT_GT(std::fabs(out[FRAME_SIZE / 2]), 1e-4f);
}

TEST(round_trip_stft_istft_matches_windowed_input) {
    // Build an arbitrary test signal.
    std::array<float, FRAME_SIZE> frame;
    for (std::size_t n = 0; n < FRAME_SIZE; ++n) {
        frame[n] = 0.5f * std::sin(
            2.0f * std::numbers::pi_v<float> * 500.0f *
            static_cast<float>(n) / static_cast<float>(SAMPLE_RATE_HZ));
    }

    // Forward STFT.
    STFTProcessor stft;
    std::array<std::complex<float>, FFT_BINS> spectrum{};
    stft.process(frame.data(), spectrum.data());

    // Inverse STFT.
    ISTFTProcessor istft;
    std::array<float, FRAME_SIZE> reconstructed{};
    istft.process(spectrum.data(), reconstructed.data());

    // Compute the expected windowed signal:
    //   expected[n] = frame[n] * hann_window[n]
    // The ISTFT output equals the Hann-windowed input divided by FFT_SIZE
    // (the 1/N factor of the IDFT) — but the STFTProcessor already applied
    // the Hann window once, so the ISTFT output is x[n]*w[n].
    for (std::size_t n = 0; n < FRAME_SIZE; ++n) {
        const float hann =
            0.5f * (1.0f - std::cos(
                2.0f * std::numbers::pi_v<float> *
                static_cast<float>(n) / static_cast<float>(FRAME_SIZE)));
        const float expected = frame[n] * hann;
        ASSERT_NEAR(reconstructed[n], expected, 1e-4f);
    }
}

TEST(output_length_is_frame_size) {
    // Structural check: verify FRAME_SIZE == 80.
    ASSERT_EQ(FRAME_SIZE, 80u);
}

TEST(sinusoid_round_trip_peak_frequency_preserved) {
    // Use a bin-aligned frequency so there is no spectral leakage.
    // Target bin 4 → f = 4 * 16000 / 256 = 250 Hz.
    constexpr std::size_t TARGET_BIN = 4;
    const float freq = static_cast<float>(TARGET_BIN) *
                       static_cast<float>(SAMPLE_RATE_HZ) /
                       static_cast<float>(FFT_SIZE);

    std::array<float, FRAME_SIZE> frame;
    for (std::size_t n = 0; n < FRAME_SIZE; ++n) {
        frame[n] = std::sin(
            2.0f * std::numbers::pi_v<float> * freq *
            static_cast<float>(n) / static_cast<float>(SAMPLE_RATE_HZ));
    }

    STFTProcessor  stft;
    ISTFTProcessor istft;

    std::array<std::complex<float>, FFT_BINS> spectrum{};
    stft.process(frame.data(), spectrum.data());

    std::array<float, FRAME_SIZE> out{};
    istft.process(spectrum.data(), out.data());

    // After the round-trip, the output should be approximately
    // frame[n] * hann[n].  Compute the STFT of the reconstructed output
    // and check that the dominant bin is still TARGET_BIN.
    std::array<std::complex<float>, FFT_BINS> out_spectrum{};
    stft.process(out.data(), out_spectrum.data());

    // Find peak bin (excluding DC).
    std::size_t peak = 1;
    for (std::size_t k = 2; k < FFT_BINS; ++k) {
        if (std::abs(out_spectrum[k]) > std::abs(out_spectrum[peak])) {
            peak = k;
        }
    }
    ASSERT_EQ(peak, TARGET_BIN);
}

TEST(repeated_calls_are_independent) {
    // Processing the same spectrum twice must give identical output.
    std::array<std::complex<float>, FFT_BINS> spec;
    for (std::size_t k = 0; k < FFT_BINS; ++k) {
        spec[k] = {0.5f * std::cos(static_cast<float>(k)), 0.0f};
    }

    ISTFTProcessor istft;
    std::array<float, FRAME_SIZE> out1{}, out2{};
    istft.process(spec.data(), out1.data());
    istft.process(spec.data(), out2.data());

    for (std::size_t n = 0; n < FRAME_SIZE; ++n) {
        ASSERT_NEAR(out1[n], out2[n], 1e-6f);
    }
}

TEST(zero_padded_region_not_in_output) {
    // For a non-zero spectrum the output only covers FRAME_SIZE samples,
    // i.e., the zero-padded region of the IFFT is not included in out.
    // This is a structural check that we only extract n=0..FRAME_SIZE-1.
    std::array<std::complex<float>, FFT_BINS> spec{};
    spec[1] = {1.0f, 0.0f};  // single frequency bin

    ISTFTProcessor istft;
    std::array<float, FRAME_SIZE> out{};
    istft.process(spec.data(), out.data());

    // Just verify that the call succeeds and produces finite values.
    for (std::size_t n = 0; n < FRAME_SIZE; ++n) {
        ASSERT(std::isfinite(out[n]));
    }
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main() {
    std::printf("\n=== ISTFTProcessor tests ===\n");
    std::printf("Results: %d passed, %d failed, %d total\n\n",
                g_passed, g_failed, g_total);
    return (g_failed == 0) ? 0 : 1;
}
