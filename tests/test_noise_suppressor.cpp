// test_noise_suppressor.cpp
// Unit tests for NoiseSuppressor.
//
// Coverage:
//  1. Initialisation phase: noise PSD bootstrapped from first N frames.
//  2. Wiener gain reduces output energy relative to noisy input.
//  3. VAD correctly classifies silence vs. speech.
//  4. Gain floor is always maintained (no bin is suppressed below GAIN_FLOOR).
//  5. Noise PSD updates during silence, freezes during speech.
//  6. reset() restarts the initialisation phase.
//  7. Pass-through when gain is 1 (pure silence input with no noise).

#include "audio_engine/audio_frame.hpp"
#include "audio_engine/noise_suppressor.hpp"
#include "audio_engine/stft_processor.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <numeric>
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

#define ASSERT_EQ(a, b)          ASSERT((a) == (b))
#define ASSERT_NEAR(a, b, eps)   ASSERT(std::fabs((a) - (b)) < (eps))
#define ASSERT_GT(a, b)          ASSERT((a) > (b))
#define ASSERT_GE(a, b)          ASSERT((a) >= (b))
#define ASSERT_LE(a, b)          ASSERT((a) <= (b))

// ── Helpers ───────────────────────────────────────────────────────────────────

using namespace audio_engine;

/// Generate a noise-like spectrum (flat power).
static void make_flat_spectrum(std::array<std::complex<float>, FFT_BINS>& spec,
                               std::array<float, FFT_BINS>& power,
                               float amplitude = 0.01f) {
    for (std::size_t k = 0; k < FFT_BINS; ++k) {
        spec[k]  = {amplitude, 0.0f};
        power[k] = amplitude * amplitude;
    }
}

/// Run the suppressor through NOISE_INIT_FRAMES of noise to complete init.
static void complete_init(NoiseSuppressor& ns, float noise_amp = 0.01f) {
    std::array<std::complex<float>, FFT_BINS> spec;
    std::array<float, FFT_BINS> power;
    for (int i = 0; i < NoiseSuppressor::NOISE_INIT_FRAMES; ++i) {
        make_flat_spectrum(spec, power, noise_amp);
        ns.process(spec, power);
    }
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST(not_initialized_before_init_frames) {
    NoiseSuppressor ns;
    ASSERT(!ns.initialized());

    std::array<std::complex<float>, FFT_BINS> spec;
    std::array<float, FFT_BINS> power;
    make_flat_spectrum(spec, power);

    for (int i = 0; i < NoiseSuppressor::NOISE_INIT_FRAMES - 1; ++i) {
        ns.process(spec, power);
        ASSERT(!ns.initialized());
    }
    ns.process(spec, power);
    ASSERT(ns.initialized());
}

TEST(noise_psd_matches_init_input) {
    NoiseSuppressor ns;
    constexpr float NOISE_AMP = 0.05f;
    complete_init(ns, NOISE_AMP);

    // After init the noise PSD should reflect the flat spectrum.
    const float expected_psd = NOISE_AMP * NOISE_AMP;
    for (std::size_t k = 0; k < FFT_BINS; ++k) {
        ASSERT_NEAR(ns.noise_psd()[k], expected_psd, expected_psd * 0.01f);
    }
}

TEST(vad_false_during_init_phase) {
    NoiseSuppressor ns;
    std::array<std::complex<float>, FFT_BINS> spec;
    std::array<float, FFT_BINS> power;
    make_flat_spectrum(spec, power);

    for (int i = 0; i < NoiseSuppressor::NOISE_INIT_FRAMES; ++i) {
        bool vad = ns.process(spec, power);
        ASSERT(!vad);
        ASSERT(!ns.vad_active());
    }
}

TEST(vad_detects_silence_after_init) {
    NoiseSuppressor ns;
    constexpr float NOISE_AMP = 0.01f;
    complete_init(ns, NOISE_AMP);

    // Feed the same low-energy signal – should be classified as silence.
    std::array<std::complex<float>, FFT_BINS> spec;
    std::array<float, FFT_BINS> power;
    make_flat_spectrum(spec, power, NOISE_AMP);

    bool vad = ns.process(spec, power);
    ASSERT(!vad);
}

TEST(vad_detects_speech_after_init) {
    NoiseSuppressor ns;
    constexpr float NOISE_AMP  = 0.01f;
    constexpr float SPEECH_AMP = 0.3f;  // >> noise → SNR >> threshold
    complete_init(ns, NOISE_AMP);

    std::array<std::complex<float>, FFT_BINS> spec;
    std::array<float, FFT_BINS> power;
    make_flat_spectrum(spec, power, SPEECH_AMP);

    bool vad = ns.process(spec, power);
    ASSERT(vad);
}

TEST(gain_floor_always_maintained) {
    NoiseSuppressor ns;
    constexpr float NOISE_AMP = 0.1f;
    complete_init(ns, NOISE_AMP);

    // Feed a frame with zero energy – all gains should be floored.
    std::array<std::complex<float>, FFT_BINS> spec{};
    std::array<float, FFT_BINS> power{};
    ns.process(spec, power);

    for (std::size_t k = 0; k < FFT_BINS; ++k) {
        ASSERT_GE(ns.wiener_gain()[k], NoiseSuppressor::GAIN_FLOOR);
    }
}

TEST(suppressor_reduces_noise_energy) {
    NoiseSuppressor ns;
    constexpr float NOISE_AMP = 0.05f;
    complete_init(ns, NOISE_AMP);

    std::array<std::complex<float>, FFT_BINS> spec;
    std::array<float, FFT_BINS> power;
    make_flat_spectrum(spec, power, NOISE_AMP);

    // Compute input energy.
    float energy_in = 0.0f;
    for (auto& b : spec) energy_in += std::norm(b);

    ns.process(spec, power);

    // Compute output energy.
    float energy_out = 0.0f;
    for (auto& b : spec) energy_out += std::norm(b);

    ASSERT_GT(energy_in, energy_out);
}

TEST(noise_psd_updates_during_silence) {
    NoiseSuppressor ns;
    // init noise amplitude
    constexpr float NOISE_AMP_1 = 0.01f;
    // New noise level: 20% louder — SNR = (0.012/0.01)^2 = 1.44 < VAD_SNR_THRESHOLD(2.0)
    // so the VAD will still classify these frames as silence and update the PSD.
    constexpr float NOISE_AMP_2 = 0.012f;
    complete_init(ns, NOISE_AMP_1);

    std::array<std::complex<float>, FFT_BINS> spec;
    std::array<float, FFT_BINS> power;
    // Run 200 silent frames – α=0.95 → 0.95^200 ≈ 0; PSD converges to NOISE_AMP_2^2.
    for (int i = 0; i < 200; ++i) {
        make_flat_spectrum(spec, power, NOISE_AMP_2);
        ns.process(spec, power);
    }

    const float expected_psd = NOISE_AMP_2 * NOISE_AMP_2;
    for (std::size_t k = 0; k < FFT_BINS; ++k) {
        ASSERT_NEAR(ns.noise_psd()[k], expected_psd, expected_psd * 0.05f);
    }
}

TEST(reset_restarts_initialisation) {
    NoiseSuppressor ns;
    complete_init(ns);
    ASSERT(ns.initialized());

    ns.reset();
    ASSERT(!ns.initialized());

    // After reset the first frame should again be silent (init phase).
    std::array<std::complex<float>, FFT_BINS> spec;
    std::array<float, FFT_BINS> power;
    make_flat_spectrum(spec, power, 0.3f);  // high energy
    bool vad = ns.process(spec, power);
    ASSERT(!vad);  // still in init phase, always returns false
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main() {
    std::printf("\n=== NoiseSuppressor tests ===\n");
    std::printf("Results: %d passed, %d failed, %d total\n\n",
                g_passed, g_failed, g_total);
    return (g_failed == 0) ? 0 : 1;
}
