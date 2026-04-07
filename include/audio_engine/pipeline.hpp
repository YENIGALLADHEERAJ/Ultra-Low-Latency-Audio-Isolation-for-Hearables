// pipeline.hpp
// End-to-end audio isolation pipeline:
//   raw PCM input → STFT → noise suppression → ISTFT → isolated PCM output.
//
// Usage (single producer / single consumer)
// ─────────────────────────────────────────
//   Pipeline pipeline;
//
//   // --- ISR / DMA callback (producer thread) ---
//   pipeline.ingest(dma_samples, num_samples);
//
//   // --- DSP thread (consumer thread) ---
//   std::array<float, FRAME_SIZE> out_pcm;
//   Pipeline::DebugFrame dbg;
//   while (pipeline.process_next_frame(out_pcm, &dbg)) {
//       // out_pcm holds FRAME_SIZE Hann-windowed isolated speech samples.
//       // dbg.spectrum / .magnitude / .power contain raw spectral data.
//       // dbg.vad_active indicates whether speech was detected.
//       send_to_dac(out_pcm.data(), FRAME_SIZE);
//   }
//
// The pipeline owns its AudioEngine, NoiseSuppressor, and ISTFTProcessor
// internally; no external setup is required beyond constructing the object.

#pragma once

#include "audio_engine.hpp"
#include "audio_frame.hpp"
#include "istft_processor.hpp"
#include "noise_suppressor.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>

namespace audio_engine {

class Pipeline {
public:
    // ── Types ─────────────────────────────────────────────────────────────────

    /// Optional per-frame debug / diagnostic snapshot.
    struct DebugFrame {
        /// One-sided complex STFT spectrum before noise suppression.
        std::array<std::complex<float>, FFT_BINS> spectrum{};

        /// Magnitude spectrum (pre-suppression).
        std::array<float, FFT_BINS> magnitude{};

        /// Power spectrum (pre-suppression).
        std::array<float, FFT_BINS> power{};

        /// Per-bin Wiener gain applied by the noise suppressor.
        std::array<float, FFT_BINS> wiener_gain{};

        /// True if the noise suppressor detected voice activity.
        bool vad_active{false};

        /// Monotonic frame counter.
        std::uint64_t sequence_number{0};
    };

    /// Pipeline configuration.
    struct Config {
        /// When false, the noise suppressor is bypassed (pass-through mode).
        bool enable_noise_suppression{true};
    };

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /// Construct with default configuration (noise suppression enabled).
    Pipeline() noexcept;

    /// Construct with explicit configuration.
    explicit Pipeline(Config cfg) noexcept;

    // Non-copyable.
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // ── Producer API ──────────────────────────────────────────────────────────

    /// Ingest \p count normalised float samples from the audio source.
    ///
    /// Samples must be in the range [-1.0, 1.0].  When the internal ring
    /// buffer is full the oldest samples are silently discarded so the
    /// producer is never blocked.
    ///
    /// Thread-safety: must be called from a single producer thread only.
    void ingest(const float* samples, std::size_t count) noexcept;

    // ── Consumer API ──────────────────────────────────────────────────────────

    /// Attempt to process one 5 ms frame through the full pipeline.
    ///
    /// \param[out] out_pcm  Populated with FRAME_SIZE reconstructed samples
    ///                      if a full frame is available.  Values are in the
    ///                      range [-1.0, 1.0] (Hann-windowed).
    /// \param[out] debug    Optional pointer to a DebugFrame; if non-null it
    ///                      is populated with pre-suppression spectral data
    ///                      and the VAD result.
    /// \returns true  if a frame was processed and \p out_pcm is valid.
    /// \returns false if fewer than FRAME_SIZE samples are buffered.
    ///
    /// Thread-safety: must be called from a single consumer thread only.
    [[nodiscard]] bool process_next_frame(
        std::array<float, FRAME_SIZE>& out_pcm,
        DebugFrame* debug = nullptr) noexcept;

    // ── Diagnostics ───────────────────────────────────────────────────────────

    /// Approximate number of PCM samples currently buffered.
    [[nodiscard]] std::size_t buffered_samples() const noexcept;

    /// Total frames successfully processed since construction.
    [[nodiscard]] std::uint64_t frames_processed() const noexcept {
        return frame_counter_;
    }

    /// Whether the last processed frame contained detected speech.
    [[nodiscard]] bool last_vad_result() const noexcept { return last_vad_; }

    /// Read-only access to the noise suppressor for diagnostics.
    [[nodiscard]] const NoiseSuppressor& suppressor() const noexcept {
        return suppressor_;
    }

private:
    Config        cfg_;
    AudioEngine   engine_;
    NoiseSuppressor suppressor_;
    ISTFTProcessor  istft_;

    std::uint64_t frame_counter_{0};
    bool          last_vad_{false};
};

} // namespace audio_engine
