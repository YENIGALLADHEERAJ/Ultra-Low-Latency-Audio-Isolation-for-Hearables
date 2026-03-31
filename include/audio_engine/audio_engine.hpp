// audio_engine.hpp
// Top-level audio processing engine for the Ultra-Low-Latency pipeline.
//
// Usage (single producer / single consumer)
// ─────────────────────────────────────────
//   AudioEngine engine;
//
//   // --- ISR / DMA callback (producer thread) ---
//   engine.ingest(pcm_block, block_samples);
//
//   // --- Processing thread (consumer thread) ---
//   AudioEngine::SpectrumFrame sf;
//   while (engine.process_next_frame(sf)) {
//       // sf.bins contains FFT_BINS complex coefficients
//       // sf.magnitude / sf.power contain derived real arrays
//       feed_to_model(sf);
//   }

#pragma once

#include "audio_frame.hpp"
#include "circular_buffer.hpp"
#include "stft_processor.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>

namespace audio_engine {

class AudioEngine {
public:
    // ── Types ─────────────────────────────────────────────────────────────────

    /// Output produced for each processed 5 ms frame.
    struct SpectrumFrame {
        /// One-sided complex STFT spectrum (FFT_BINS bins).
        std::array<std::complex<float>, FFT_BINS> bins{};

        /// Linear magnitude spectrum derived from \p bins.
        std::array<float, FFT_BINS> magnitude{};

        /// Power spectrum derived from \p bins.
        std::array<float, FFT_BINS> power{};

        /// Monotonically increasing frame counter (wraps at UINT64_MAX).
        std::uint64_t sequence_number{0};
    };

    // ── Constants ─────────────────────────────────────────────────────────────

    /// Internal circular buffer capacity (must be power-of-two).
    /// 4096 floats ≈ 256 ms of audio headroom at 16 kHz.
    static constexpr std::size_t BUFFER_CAPACITY = 4096;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    AudioEngine() noexcept = default;

    // Non-copyable (contains atomic circular buffer).
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // ── Producer API ──────────────────────────────────────────────────────────

    /// Ingest \p count normalised float samples from the audio source.
    ///
    /// Samples must be in the range [-1.0, 1.0].  When the internal buffer
    /// is full the oldest data is silently overwritten so the producer is
    /// never blocked.
    ///
    /// Thread-safety: must be called from a single producer thread only.
    void ingest(const float* samples, std::size_t count) noexcept;

    // ── Consumer API ──────────────────────────────────────────────────────────

    /// Attempt to dequeue one 5 ms frame and compute its STFT spectrum.
    ///
    /// \param[out] out  Populated with the spectral data if a full frame is
    ///                  available.
    /// \returns true   if a frame was processed and \p out is valid.
    /// \returns false  if fewer than FRAME_SIZE samples are buffered.
    ///
    /// Thread-safety: must be called from a single consumer thread only.
    [[nodiscard]] bool process_next_frame(SpectrumFrame& out) noexcept;

    // ── Diagnostics ───────────────────────────────────────────────────────────

    /// Approximate number of samples currently buffered.
    [[nodiscard]] std::size_t buffered_samples() const noexcept {
        return input_buffer_.available();
    }

    /// Total number of frames successfully processed since construction.
    [[nodiscard]] std::uint64_t frames_processed() const noexcept {
        return frame_counter_;
    }

private:
    /// Raw sample ring-buffer (SPSC, lock-free).
    CircularBuffer<float, BUFFER_CAPACITY> input_buffer_;

    /// STFT engine (not thread-safe – owned by the consumer thread).
    STFTProcessor stft_;

    /// Scratch buffer used to pop one frame at a time.
    std::array<float, FRAME_SIZE> frame_scratch_{};

    /// Monotonic frame counter.
    std::uint64_t frame_counter_{0};
};

} // namespace audio_engine
