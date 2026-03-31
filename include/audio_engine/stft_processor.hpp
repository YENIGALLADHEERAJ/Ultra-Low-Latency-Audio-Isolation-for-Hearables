// stft_processor.hpp
// Short-Time Fourier Transform (STFT) processor for 5 ms audio frames.
//
// Algorithm
// ─────────
//  1. Accept a FRAME_SIZE (80-sample) input frame.
//  2. Copy it into a zero-padded FFT_SIZE (256-point) work buffer.
//  3. Apply a periodic Hann window to the windowed region.
//  4. Run an in-place, radix-2, decimation-in-time (DIT) Cooley-Tukey FFT.
//  5. Return the one-sided complex spectrum (FFT_BINS = 129 bins).
//
// The one-sided spectrum covers 0 … SAMPLE_RATE/2 in FFT_BINS linearly
// spaced bins with a frequency resolution of SAMPLE_RATE / FFT_SIZE = 62.5 Hz.
//
// This class is NOT thread-safe.  Create one instance per processing thread.

#pragma once

#include "audio_frame.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <span>

namespace audio_engine {

class STFTProcessor {
public:
    /// Precomputes the Hann window and twiddle factors.
    STFTProcessor() noexcept;

    /// Compute the STFT of \p frame.
    ///
    /// \param frame   Input PCM samples (FRAME_SIZE elements).
    /// \param out     Output complex spectrum (FFT_BINS elements).
    ///
    /// The input is zero-padded to FFT_SIZE before the FFT is applied, so
    /// \p out[k] represents the complex amplitude at frequency
    ///     f_k = k * SAMPLE_RATE_HZ / FFT_SIZE  (Hz).
    void process(std::span<const float, FRAME_SIZE> frame,
                 std::span<std::complex<float>, FFT_BINS> out) noexcept;

    /// Convenience overload that accepts a raw pointer pair.
    void process(const float* frame_data,
                 std::complex<float>* out_data) noexcept {
        process(std::span<const float, FRAME_SIZE>{frame_data, FRAME_SIZE},
                std::span<std::complex<float>, FFT_BINS>{out_data, FFT_BINS});
    }

    // ── Accessors ─────────────────────────────────────────────────────────────

    /// Frequency (Hz) of bin \p k.
    [[nodiscard]] static constexpr float bin_frequency(std::size_t k) noexcept {
        return static_cast<float>(k) *
               static_cast<float>(SAMPLE_RATE_HZ) /
               static_cast<float>(FFT_SIZE);
    }

    /// Magnitude spectrum (linear scale) of the last processed frame.
    /// Valid after a call to process().
    [[nodiscard]] const std::array<float, FFT_BINS>& magnitude() const noexcept {
        return magnitude_;
    }

    /// Power spectrum of the last processed frame.
    [[nodiscard]] const std::array<float, FFT_BINS>& power() const noexcept {
        return power_;
    }

private:
    // ── In-place radix-2 DIT FFT (operates on fft_buf_) ────────────────────

    void compute_fft() noexcept;

    // ── State ─────────────────────────────────────────────────────────────────

    /// Periodic Hann window – length == FFT_SIZE, but only the first
    /// FRAME_SIZE coefficients are non-trivial; the rest are 0.
    std::array<float, FFT_SIZE> window_{};

    /// Precomputed twiddle factors W_N^k = exp(-j 2π k / N) for the FFT.
    std::array<std::complex<float>, FFT_SIZE / 2> twiddle_{};

    /// Working buffer for the in-place FFT (complex-valued).
    std::array<std::complex<float>, FFT_SIZE> fft_buf_{};

    /// One-sided magnitude spectrum (output of last process() call).
    std::array<float, FFT_BINS> magnitude_{};

    /// One-sided power spectrum (output of last process() call).
    std::array<float, FFT_BINS> power_{};
};

} // namespace audio_engine
