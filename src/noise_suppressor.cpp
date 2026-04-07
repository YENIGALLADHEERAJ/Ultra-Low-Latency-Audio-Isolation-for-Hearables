// noise_suppressor.cpp
// Implementation of the Wiener-filter noise suppressor.

#include "audio_engine/noise_suppressor.hpp"

#include <algorithm>
#include <cmath>

namespace audio_engine {

// ── Construction ─────────────────────────────────────────────────────────────

NoiseSuppressor::NoiseSuppressor() noexcept {
    // Start gains at the floor so the first (initialisation) frames are
    // gently attenuated while the noise PSD is being bootstrapped.
    gain_.fill(GAIN_FLOOR);

    // Seed noise_psd_ with a tiny non-zero value to prevent division-by-zero
    // before the initialisation phase completes.
    noise_psd_.fill(1e-10f);
    noise_accum_.fill(0.0f);
}

// ── reset() ──────────────────────────────────────────────────────────────────

void NoiseSuppressor::reset() noexcept {
    gain_.fill(GAIN_FLOOR);
    noise_psd_.fill(1e-10f);
    noise_accum_.fill(0.0f);
    init_frames_remaining_ = NOISE_INIT_FRAMES;
    vad_active_ = false;
}

// ── process() ────────────────────────────────────────────────────────────────

bool NoiseSuppressor::process(
    std::array<std::complex<float>, FFT_BINS>& spectrum,
    const std::array<float, FFT_BINS>& power) noexcept {

    // ── Phase 1: Initial noise estimation ────────────────────────────────────
    if (init_frames_remaining_ > 0) {
        for (std::size_t k = 0; k < FFT_BINS; ++k) {
            noise_accum_[k] += power[k];
        }
        --init_frames_remaining_;

        if (init_frames_remaining_ == 0) {
            // Finalise initialisation: average accumulated power spectra.
            const float inv_n =
                1.0f / static_cast<float>(NOISE_INIT_FRAMES);
            for (std::size_t k = 0; k < FFT_BINS; ++k) {
                noise_psd_[k] = std::max(noise_accum_[k] * inv_n, 1e-10f);
            }
        }

        // During initialisation, apply the floor gain and report silence so
        // the caller discards or mutes this frame.
        for (auto& bin : spectrum) { bin *= GAIN_FLOOR; }
        vad_active_ = false;
        return false;
    }

    // ── Phase 2: Voice Activity Detection ────────────────────────────────────
    // Compare total frame power against the estimated noise floor.
    float total_power = 0.0f;
    float total_noise = 0.0f;
    for (std::size_t k = 0; k < FFT_BINS; ++k) {
        total_power += power[k];
        total_noise += noise_psd_[k];
    }

    // Guard against both quantities being near zero (silent room).
    const float frame_snr =
        (total_noise > 1e-30f) ? (total_power / total_noise) : 1.0f;

    vad_active_ = (frame_snr > VAD_SNR_THRESHOLD);

    // ── Phase 3: Noise PSD update (only during silence) ──────────────────────
    // We do NOT update the noise model during speech to preserve an accurate
    // estimate of the background noise level.
    if (!vad_active_) {
        const float alpha           = NOISE_SMOOTH_ALPHA;
        const float one_minus_alpha = 1.0f - alpha;
        for (std::size_t k = 0; k < FFT_BINS; ++k) {
            noise_psd_[k] =
                alpha * noise_psd_[k] + one_minus_alpha * power[k];
            noise_psd_[k] = std::max(noise_psd_[k], 1e-10f);
        }
    }

    // ── Phase 4: Compute and smooth per-bin Wiener gain ──────────────────────
    const float beta           = GAIN_SMOOTH_BETA;
    const float one_minus_beta = 1.0f - beta;

    for (std::size_t k = 0; k < FFT_BINS; ++k) {
        // A-posteriori SNR estimate (clamped to [0, ∞)).
        // snr_k = max( P[k]/N[k] - 1 , 0 )
        const float snr_k =
            std::max(power[k] / noise_psd_[k] - 1.0f, 0.0f);

        // Wiener gain: G = SNR / (SNR + 1).  Floored to avoid musical noise.
        const float g_k = std::max(snr_k / (snr_k + 1.0f), GAIN_FLOOR);

        // First-order IIR smoothing across frames.
        gain_[k] = beta * gain_[k] + one_minus_beta * g_k;
    }

    // ── Phase 5: Apply gain to spectrum ──────────────────────────────────────
    for (std::size_t k = 0; k < FFT_BINS; ++k) {
        spectrum[k] *= gain_[k];
    }

    return vad_active_;
}

} // namespace audio_engine
