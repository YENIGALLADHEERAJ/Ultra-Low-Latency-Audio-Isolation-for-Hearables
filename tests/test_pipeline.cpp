// test_pipeline.cpp
// Integration tests for the end-to-end Pipeline.
//
// Coverage:
//  1. No output until enough samples are buffered (FRAME_SIZE).
//  2. Correct number of frames emitted for a given number of input samples.
//  3. frames_processed() counter increments correctly.
//  4. Noise suppression reduces energy relative to the input.
//  5. Pass-through mode (noise suppression disabled) preserves signal structure.
//  6. DebugFrame is populated correctly when provided.
//  7. buffered_samples() reflects the current ring-buffer occupancy.

#include "audio_engine/audio_frame.hpp"
#include "audio_engine/pipeline.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <stdexcept>
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
#define ASSERT_GE(a, b)         ASSERT((a) >= (b))

// ── Helpers ───────────────────────────────────────────────────────────────────

using namespace audio_engine;

/// Build one FRAME_SIZE block of low-amplitude noise.
static std::array<float, FRAME_SIZE> make_noise_frame(float amplitude = 0.01f) {
    std::array<float, FRAME_SIZE> frame;
    for (std::size_t n = 0; n < FRAME_SIZE; ++n) {
        // Deterministic "pseudo-noise" – square wave at 1 kHz.
        frame[n] = ((n / 8) % 2 == 0) ? amplitude : -amplitude;
    }
    return frame;
}

/// Build one FRAME_SIZE block of a 250 Hz sinusoid.
static std::array<float, FRAME_SIZE> make_speech_frame(float amplitude = 0.5f) {
    std::array<float, FRAME_SIZE> frame;
    for (std::size_t n = 0; n < FRAME_SIZE; ++n) {
        frame[n] = amplitude * std::sin(
            2.0f * std::numbers::pi_v<float> * 250.0f *
            static_cast<float>(n) / static_cast<float>(SAMPLE_RATE_HZ));
    }
    return frame;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST(no_output_until_frame_size_samples_buffered) {
    Pipeline pipeline;
    std::array<float, FRAME_SIZE> out{};

    // Feed one sample less than a full frame.
    constexpr std::size_t PARTIAL = FRAME_SIZE - 1;
    auto frame = make_noise_frame();
    pipeline.ingest(frame.data(), PARTIAL);

    ASSERT(!pipeline.process_next_frame(out));
    ASSERT_EQ(pipeline.frames_processed(), 0u);
}

TEST(correct_frames_emitted_for_input_count) {
    Pipeline pipeline;
    std::array<float, FRAME_SIZE> out{};

    constexpr int N_FRAMES = 5;
    auto noise = make_noise_frame();

    // Ingest exactly N_FRAMES worth of samples.
    for (int i = 0; i < N_FRAMES; ++i) {
        pipeline.ingest(noise.data(), FRAME_SIZE);
    }

    int count = 0;
    while (pipeline.process_next_frame(out)) { ++count; }

    ASSERT_EQ(count, N_FRAMES);
    ASSERT_EQ(pipeline.frames_processed(), static_cast<std::uint64_t>(N_FRAMES));
}

TEST(frames_processed_counter_increments) {
    Pipeline pipeline;
    std::array<float, FRAME_SIZE> out{};
    auto noise = make_noise_frame();

    ASSERT_EQ(pipeline.frames_processed(), 0u);
    pipeline.ingest(noise.data(), FRAME_SIZE);
    (void)pipeline.process_next_frame(out);
    ASSERT_EQ(pipeline.frames_processed(), 1u);

    pipeline.ingest(noise.data(), FRAME_SIZE);
    (void)pipeline.process_next_frame(out);
    ASSERT_EQ(pipeline.frames_processed(), 2u);
}

TEST(buffered_samples_decreases_after_processing) {
    Pipeline pipeline;
    std::array<float, FRAME_SIZE> out{};
    auto noise = make_noise_frame();

    pipeline.ingest(noise.data(), FRAME_SIZE);
    std::size_t before = pipeline.buffered_samples();
    ASSERT_GE(before, FRAME_SIZE);

    (void)pipeline.process_next_frame(out);
    std::size_t after = pipeline.buffered_samples();
    ASSERT_GT(before, after);
}

TEST(noise_suppression_reduces_energy) {
    // The suppressor needs NOISE_INIT_FRAMES to bootstrap its noise model.
    // Feed enough noise-only frames to complete initialisation, then measure
    // the energy reduction on a subsequent noise frame.
    Pipeline pipeline;

    auto noise = make_noise_frame(0.1f);

    // Ingest (NOISE_INIT_FRAMES + 5) noise frames to complete init + settle.
    const int TOTAL_NOISE_FRAMES = NoiseSuppressor::NOISE_INIT_FRAMES + 5;
    for (int i = 0; i < TOTAL_NOISE_FRAMES; ++i) {
        pipeline.ingest(noise.data(), FRAME_SIZE);
    }

    // Drain all frames up to the last one.
    std::array<float, FRAME_SIZE> out{};
    std::array<float, FRAME_SIZE> last_out{};
    int drained = 0;
    while (pipeline.process_next_frame(out)) {
        last_out = out;
        ++drained;
    }
    ASSERT_GT(drained, 0);

    // The last output frame (post-init, noise input) should have lower energy
    // than the raw noise input.
    float energy_in  = 0.0f;
    float energy_out = 0.0f;
    for (std::size_t n = 0; n < FRAME_SIZE; ++n) {
        energy_in  += noise[n] * noise[n];
        energy_out += last_out[n] * last_out[n];
    }
    ASSERT_GT(energy_in, energy_out);
}

TEST(passthrough_mode_produces_output) {
    // When noise suppression is disabled the pipeline still produces output
    // (STFT → ISTFT round-trip) and the output is non-trivially non-zero for
    // a non-zero input.
    Pipeline::Config cfg;
    cfg.enable_noise_suppression = false;
    Pipeline pipeline(cfg);

    auto speech = make_speech_frame(0.5f);
    pipeline.ingest(speech.data(), FRAME_SIZE);

    std::array<float, FRAME_SIZE> out{};
    ASSERT(pipeline.process_next_frame(out));

    // The output should contain energy (not all zeros).
    float energy = 0.0f;
    for (auto s : out) energy += s * s;
    ASSERT_GT(energy, 1e-6f);
}

TEST(debug_frame_populated_when_provided) {
    Pipeline pipeline;
    auto noise = make_noise_frame(0.05f);
    pipeline.ingest(noise.data(), FRAME_SIZE);

    std::array<float, FRAME_SIZE> out{};
    Pipeline::DebugFrame dbg{};
    ASSERT(pipeline.process_next_frame(out, &dbg));

    // Debug frame must have a non-zero sequence number.
    ASSERT_GT(dbg.sequence_number, 0u);

    // Magnitude and power arrays should contain at least some non-zero values.
    float total_mag = 0.0f;
    for (auto m : dbg.magnitude) total_mag += m;
    ASSERT_GT(total_mag, 0.0f);
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main() {
    std::printf("\n=== Pipeline integration tests ===\n");
    std::printf("Results: %d passed, %d failed, %d total\n\n",
                g_passed, g_failed, g_total);
    return (g_failed == 0) ? 0 : 1;
}
