// audio_frame.hpp
// Defines compile-time audio pipeline constants and the AudioFrame POD type.
//
// At 16 kHz a 5 ms frame contains exactly 80 PCM samples.
// The STFT uses FFT_SIZE = 256 (next power-of-2 ≥ 80) yielding
// FFT_BINS = 129 complex coefficients per frame.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace audio_engine {

// ── Pipeline constants ──────────────────────────────────────────────────────

/// PCM sample rate used throughout the pipeline (Hz).
inline constexpr int SAMPLE_RATE_HZ = 16'000;

/// Target processing frame duration (ms).
inline constexpr int FRAME_DURATION_MS = 5;

/// Number of float samples per processing frame  (= 16000 * 0.005 = 80).
inline constexpr std::size_t FRAME_SIZE =
    static_cast<std::size_t>(SAMPLE_RATE_HZ * FRAME_DURATION_MS / 1000);

/// FFT size used by the STFT processor (next power-of-2 ≥ FRAME_SIZE).
inline constexpr std::size_t FFT_SIZE = 256;

/// Number of unique (real-valued) frequency bins output by the STFT
/// (= FFT_SIZE / 2 + 1).
inline constexpr std::size_t FFT_BINS = FFT_SIZE / 2 + 1;

// ── AudioFrame ───────────────────────────────────────────────────────────────

/// A single 5 ms PCM frame of normalised float samples in [-1, 1].
/// This is the unit of data exchanged between the circular buffer and the
/// STFT processor.
struct AudioFrame {
    std::array<float, FRAME_SIZE> samples{};

    /// Sequence counter set by AudioEngine – wraps at UINT64_MAX.
    std::uint64_t sequence_number{0};
};

} // namespace audio_engine
