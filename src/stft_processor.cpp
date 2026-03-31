// stft_processor.cpp
// Implementation of STFTProcessor – Hann-windowed, zero-padded, radix-2
// Cooley-Tukey DIT FFT for 5 ms audio frames at 16 kHz.

#include "audio_engine/stft_processor.hpp"

#include <cassert>
#include <cmath>
#include <numbers>

namespace audio_engine {

// ── Construction ─────────────────────────────────────────────────────────────

STFTProcessor::STFTProcessor() noexcept {
    // Periodic Hann window applied to the first FRAME_SIZE samples.
    // w[n] = 0.5 * (1 - cos(2π n / FRAME_SIZE))
    // The remaining FFT_SIZE - FRAME_SIZE coefficients are 0 (zero-padding).
    for (std::size_t n = 0; n < FRAME_SIZE; ++n) {
        window_[n] = 0.5f * (1.0f - std::cos(
            2.0f * std::numbers::pi_v<float> *
            static_cast<float>(n) / static_cast<float>(FRAME_SIZE)));
    }
    // Twiddle factors W_N^k = exp(-j 2π k / N), k = 0 … N/2-1.
    for (std::size_t k = 0; k < FFT_SIZE / 2; ++k) {
        const float angle =
            -2.0f * std::numbers::pi_v<float> *
            static_cast<float>(k) / static_cast<float>(FFT_SIZE);
        twiddle_[k] = {std::cos(angle), std::sin(angle)};
    }
}

// ── process() ────────────────────────────────────────────────────────────────

void STFTProcessor::process(std::span<const float, FRAME_SIZE> frame,
                            std::span<std::complex<float>, FFT_BINS> out) noexcept {
    // 1. Load windowed frame into the FFT work buffer, zero-pad the rest.
    for (std::size_t n = 0; n < FRAME_SIZE; ++n) {
        fft_buf_[n] = {frame[n] * window_[n], 0.0f};
    }
    for (std::size_t n = FRAME_SIZE; n < FFT_SIZE; ++n) {
        fft_buf_[n] = {0.0f, 0.0f};
    }

    // 2. In-place radix-2 DIT FFT.
    compute_fft();

    // 3. Extract one-sided spectrum, magnitude, and power.
    for (std::size_t k = 0; k < FFT_BINS; ++k) {
        out[k]        = fft_buf_[k];
        magnitude_[k] = std::abs(fft_buf_[k]);
        power_[k]     = magnitude_[k] * magnitude_[k];
    }
}

// ── compute_fft() – radix-2 DIT Cooley-Tukey ────────────────────────────────

void STFTProcessor::compute_fft() noexcept {
    const std::size_t N = FFT_SIZE;

    // ── Bit-reversal permutation ──────────────────────────────────────────────
    // log2(256) = 8 bits
    const unsigned int LOG2_N = 8u;
    for (std::size_t i = 0; i < N; ++i) {
        // Bit-reverse index i into r.
        std::size_t r = 0;
        std::size_t tmp = i;
        for (unsigned int b = 0; b < LOG2_N; ++b) {
            r = (r << 1u) | (tmp & 1u);
            tmp >>= 1u;
        }
        if (r > i) {
            std::swap(fft_buf_[i], fft_buf_[r]);
        }
    }

    // ── Butterfly passes ─────────────────────────────────────────────────────
    // Outer loop: stage s = 1 … LOG2_N, butterfly span = 2^s.
    for (unsigned int s = 1; s <= LOG2_N; ++s) {
        const std::size_t span   = std::size_t{1} << s;       // 2^s
        const std::size_t half   = span >> 1u;                 // 2^(s-1)
        const std::size_t stride = N >> s;                     // twiddle stride

        for (std::size_t k = 0; k < N; k += span) {
            for (std::size_t j = 0; j < half; ++j) {
                const auto& tw = twiddle_[j * stride];
                const auto  u  = fft_buf_[k + j];
                const auto  t  = tw * fft_buf_[k + j + half];
                fft_buf_[k + j]        = u + t;
                fft_buf_[k + j + half] = u - t;
            }
        }
    }
}

} // namespace audio_engine
