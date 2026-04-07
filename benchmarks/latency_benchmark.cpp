// latency_benchmark.cpp
// Per-frame end-to-end processing latency benchmark for the audio pipeline.
//
// Measures the wall-clock time consumed by a single
//   pipeline.process_next_frame()
// call over many iterations and reports min / max / mean / P50 / P95 / P99
// latency in microseconds.
//
// Build with BUILD_BENCHMARKS=ON (Release mode for meaningful numbers):
//   cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON -B build .
//   cmake --build build
//   ./build/latency_benchmark
//
// The target budget is < 5000 µs (5 ms) per frame at 16 kHz with 5 ms frames,
// since the pipeline must complete before the next frame arrives.

#include "audio_engine/audio_frame.hpp"
#include "audio_engine/pipeline.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <numeric>
#include <vector>

using namespace audio_engine;
using Clock     = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;
using Micros    = std::chrono::duration<double, std::micro>;

// ── Configuration ─────────────────────────────────────────────────────────────

static constexpr int  WARMUP_FRAMES     = 50;
static constexpr int  BENCHMARK_FRAMES  = 10'000;
static constexpr int  TOTAL_FRAMES      = WARMUP_FRAMES + BENCHMARK_FRAMES;
static constexpr float SIGNAL_AMPLITUDE  = 0.1f;
static constexpr float SIGNAL_FREQ_HZ    = 250.0f;

// ── Helpers ───────────────────────────────────────────────────────────────────

/// Fill one FRAME_SIZE block with a 250 Hz sinusoid + 5% white-ish noise.
static void fill_test_frame(std::array<float, FRAME_SIZE>& frame,
                             std::uint64_t frame_index) {
    for (std::size_t n = 0; n < FRAME_SIZE; ++n) {
        const float t = static_cast<float>(frame_index * FRAME_SIZE + n) /
                        static_cast<float>(SAMPLE_RATE_HZ);
        // Deterministic "noise" component using a simple LFSR-like hash.
        const std::uint32_t rnd =
            static_cast<std::uint32_t>(frame_index * 1664525u + n * 1013904223u);
        const float noise = (static_cast<float>(rnd & 0xFFFFu) / 32768.0f - 1.0f)
                            * SIGNAL_AMPLITUDE * 0.05f;
        frame[n] = SIGNAL_AMPLITUDE *
                   std::sin(2.0f * std::numbers::pi_v<float> *
                             SIGNAL_FREQ_HZ * t) + noise;
    }
}

/// Print a statistical summary of a latency sample vector (in µs).
static void print_stats(const std::vector<double>& samples) {
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());

    const double n    = static_cast<double>(sorted.size());
    const double mean = std::accumulate(sorted.begin(), sorted.end(), 0.0) / n;

    double variance = 0.0;
    for (double v : sorted) variance += (v - mean) * (v - mean);
    const double stddev = std::sqrt(variance / n);

    auto percentile = [&](double pct) -> double {
        const std::size_t idx =
            std::min(static_cast<std::size_t>(pct / 100.0 * n),
                     sorted.size() - 1);
        return sorted[idx];
    };

    std::printf("  Samples : %d\n",   static_cast<int>(sorted.size()));
    std::printf("  Min     : %8.2f µs\n", sorted.front());
    std::printf("  Max     : %8.2f µs\n", sorted.back());
    std::printf("  Mean    : %8.2f µs\n", mean);
    std::printf("  Std dev : %8.2f µs\n", stddev);
    std::printf("  P50     : %8.2f µs\n", percentile(50.0));
    std::printf("  P95     : %8.2f µs\n", percentile(95.0));
    std::printf("  P99     : %8.2f µs\n", percentile(99.0));

    const double budget_us = static_cast<double>(FRAME_SIZE) /
                             static_cast<double>(SAMPLE_RATE_HZ) * 1e6;
    std::printf("  Budget  : %8.2f µs  (one frame duration at %d Hz)\n",
                budget_us, SAMPLE_RATE_HZ);
    const int within_budget =
        static_cast<int>(std::count_if(sorted.begin(), sorted.end(),
            [budget_us](double v) { return v <= budget_us; }));
    std::printf("  Within  : %d / %d frames (%.1f%%)\n",
                within_budget, static_cast<int>(sorted.size()),
                100.0 * within_budget / static_cast<double>(sorted.size()));
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::printf("═══════════════════════════════════════════════════════\n");
    std::printf("  Ultra-Low-Latency Audio Isolation – Latency Benchmark\n");
    std::printf("═══════════════════════════════════════════════════════\n\n");
    std::printf("  Frame size  : %zu samples @ %d Hz (%d ms)\n",
                FRAME_SIZE, SAMPLE_RATE_HZ,
                static_cast<int>(FRAME_SIZE * 1000 / SAMPLE_RATE_HZ));
    std::printf("  FFT size    : %zu points  (%zu bins)\n",
                FFT_SIZE, FFT_BINS);
    std::printf("  Warmup      : %d frames\n", WARMUP_FRAMES);
    std::printf("  Benchmark   : %d frames\n\n", BENCHMARK_FRAMES);

    Pipeline pipeline;

    std::array<float, FRAME_SIZE> frame_buf{};
    std::array<float, FRAME_SIZE> out_pcm{};
    std::vector<double> latency_us;
    latency_us.reserve(BENCHMARK_FRAMES);

    // ── Pre-fill ring buffer with one frame of silence so the first call
    //    to process_next_frame() has data immediately. ──────────────────────
    frame_buf.fill(0.0f);
    pipeline.ingest(frame_buf.data(), FRAME_SIZE);

    for (int f = 0; f < TOTAL_FRAMES; ++f) {
        // Prepare next input frame.
        fill_test_frame(frame_buf, static_cast<std::uint64_t>(f));

        // Ensure one frame is available in the buffer.
        pipeline.ingest(frame_buf.data(), FRAME_SIZE);

        // Time the process_next_frame() call.
        const TimePoint t0 = Clock::now();
        const bool ok = pipeline.process_next_frame(out_pcm);
        const TimePoint t1 = Clock::now();

        if (!ok) {
            std::fprintf(stderr, "WARNING: process_next_frame() returned false "
                                 "at iteration %d\n", f);
            continue;
        }

        if (f >= WARMUP_FRAMES) {
            latency_us.push_back(Micros(t1 - t0).count());
        }
    }

    // ── Full pipeline ─────────────────────────────────────────────────────────
    std::printf("Full pipeline (STFT + Wiener filter + ISTFT)\n");
    std::printf("────────────────────────────────────────────\n");
    print_stats(latency_us);

    // ── STFT-only ─────────────────────────────────────────────────────────────
    std::printf("\nSTFT only (analysis stage)\n");
    std::printf("──────────────────────────\n");

    {
        audio_engine::STFTProcessor stft;
        std::array<std::complex<float>, FFT_BINS> spec{};
        std::vector<double> stft_lat;
        stft_lat.reserve(BENCHMARK_FRAMES);

        for (int f = 0; f < BENCHMARK_FRAMES; ++f) {
            fill_test_frame(frame_buf, static_cast<std::uint64_t>(f));
            const TimePoint t0 = Clock::now();
            stft.process(frame_buf.data(), spec.data());
            const TimePoint t1 = Clock::now();
            stft_lat.push_back(Micros(t1 - t0).count());
        }
        print_stats(stft_lat);
    }

    // ── ISTFT-only ────────────────────────────────────────────────────────────
    std::printf("\nISTFT only (synthesis stage)\n");
    std::printf("────────────────────────────\n");

    {
        audio_engine::ISTFTProcessor istft;
        std::array<std::complex<float>, FFT_BINS> spec;
        for (std::size_t k = 0; k < FFT_BINS; ++k) {
            spec[k] = {0.1f * std::cos(static_cast<float>(k)), 0.0f};
        }
        std::vector<double> istft_lat;
        istft_lat.reserve(BENCHMARK_FRAMES);

        for (int f = 0; f < BENCHMARK_FRAMES; ++f) {
            const TimePoint t0 = Clock::now();
            istft.process(spec.data(), frame_buf.data());
            const TimePoint t1 = Clock::now();
            istft_lat.push_back(Micros(t1 - t0).count());
        }
        print_stats(istft_lat);
    }

    std::printf("\nDone.\n");
    return 0;
}
