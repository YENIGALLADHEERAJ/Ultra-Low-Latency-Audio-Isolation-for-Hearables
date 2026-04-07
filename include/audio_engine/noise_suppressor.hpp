// noise_suppressor.hpp
// Wiener-filter-based spectral noise suppressor for 5 ms audio frames.
//
// Algorithm
// ─────────
//  1. Noise Initialisation  – the first NOISE_INIT_FRAMES frames are assumed
//     to be noise-only; their average power spectrum seeds the noise PSD.
//  2. Voice Activity Detection (VAD) – a simple energy-ratio test compares
//     the frame's total power against the estimated noise floor.
//  3. Noise PSD tracking – during detected silence the noise PSD is updated
//     with exponential smoothing so the suppressor adapts to slowly changing
//     background noise.
//  4. Wiener gain – per-bin gain G[k] = SNR[k] / (SNR[k] + 1), floored at
//     GAIN_FLOOR to prevent "musical noise" artefacts.
//  5. Gain smoothing – first-order IIR across successive frames to reduce
//     rapid gain fluctuations that the ear perceives as distortion.
//  6. Spectrum modification – multiply each complex bin by its gain G[k].
//
// Reference
// ─────────
//  Ephraim & Malah, "Speech Enhancement Using a Minimum Mean-Square Error
//  Short-Time Spectral Amplitude Estimator", IEEE Trans. ASSP, 1984.
//
// This class is NOT thread-safe.  Create one instance per processing thread.

#pragma once

#include "audio_frame.hpp"

#include <array>
#include <complex>
#include <cstddef>

namespace audio_engine {

class NoiseSuppressor {
public:
    // ── Tunable constants ─────────────────────────────────────────────────────

    /// Number of frames used to bootstrap the initial noise PSD estimate.
    static constexpr int NOISE_INIT_FRAMES = 10;

    /// Exponential smoothing factor for noise PSD tracking (0 < α < 1).
    /// Higher → slower adaptation (good for stationary background noise).
    static constexpr float NOISE_SMOOTH_ALPHA = 0.95f;

    /// Minimum Wiener gain applied to any frequency bin.
    /// Prevents complete suppression which causes "musical noise".
    static constexpr float GAIN_FLOOR = 0.08f;

    /// First-order IIR smoothing factor for the per-bin gain across frames.
    static constexpr float GAIN_SMOOTH_BETA = 0.85f;

    /// SNR threshold for voice activity detection (linear, not dB).
    /// A frame is classified as speech when
    ///     total_power / total_noise_power  >  VAD_SNR_THRESHOLD.
    /// A value of 2.0 corresponds to approximately +3 dB.
    static constexpr float VAD_SNR_THRESHOLD = 2.0f;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    NoiseSuppressor() noexcept;

    // ── Processing ────────────────────────────────────────────────────────────

    /// Apply Wiener-filter noise suppression to one spectral frame.
    ///
    /// \param[inout] spectrum  One-sided complex STFT spectrum (FFT_BINS bins).
    ///                         Modified in-place by the computed Wiener gain.
    /// \param[in]    power     Pre-computed power spectrum |spectrum[k]|².
    ///                         Must have FFT_BINS elements; typically obtained
    ///                         from STFTProcessor::power().
    /// \returns true  if speech activity is detected in this frame.
    /// \returns false if the frame is classified as silence/noise.
    bool process(std::array<std::complex<float>, FFT_BINS>& spectrum,
                 const std::array<float, FFT_BINS>& power) noexcept;

    /// Reset to initial state – restarts the noise initialisation phase.
    void reset() noexcept;

    // ── Diagnostics ───────────────────────────────────────────────────────────

    /// Current estimated noise power spectral density (FFT_BINS values).
    [[nodiscard]] const std::array<float, FFT_BINS>& noise_psd() const noexcept {
        return noise_psd_;
    }

    /// Smoothed Wiener gains applied during the last process() call.
    [[nodiscard]] const std::array<float, FFT_BINS>& wiener_gain() const noexcept {
        return gain_;
    }

    /// Whether the most recent process() call detected voice activity.
    [[nodiscard]] bool vad_active() const noexcept { return vad_active_; }

    /// True once the initial NOISE_INIT_FRAMES estimation phase is complete.
    [[nodiscard]] bool initialized() const noexcept {
        return init_frames_remaining_ <= 0;
    }

private:
    std::array<float, FFT_BINS> noise_psd_{};    ///< Estimated noise power per bin.
    std::array<float, FFT_BINS> noise_accum_{};  ///< Accumulator used during init.
    std::array<float, FFT_BINS> gain_{};         ///< Current smoothed Wiener gains.

    int  init_frames_remaining_{NOISE_INIT_FRAMES};
    bool vad_active_{false};
};

} // namespace audio_engine
