// istft_processor.hpp
// Inverse Short-Time Fourier Transform (ISTFT) processor.
//
// Converts a one-sided complex frequency-domain spectrum produced by
// STFTProcessor back to a FRAME_SIZE time-domain PCM frame.
//
// Algorithm
// ─────────
//  1. Reconstruct the full-length (FFT_SIZE) spectrum by Hermitian symmetry:
//       X[FFT_SIZE - k] = conj(X[k])  for k = 1 … FFT_SIZE/2 - 1.
//  2. Compute the Inverse DFT via the IFFT (radix-2 DIT with conjugate
//     twiddle factors, divide-by-N).
//  3. Take the real part of the first FRAME_SIZE output samples.
//     The imaginary parts are zero (or negligibly small) for a Hermitian input.
//
// Round-trip note
// ───────────────
//  STFTProcessor applies a Hann window before the FFT, so a full round-trip
//  (STFT then ISTFT) reconstructs  x_out[n] = x_in[n] * hann_window[n]
//  rather than x_in[n] verbatim.  For spectral-domain processing (noise
//  suppression, equalisation, etc.) the windowed output is the correct
//  perceptual result because the analysis window is part of the signal model.
//
// This class is NOT thread-safe.  Create one instance per processing thread.

#pragma once

#include "audio_frame.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <span>

namespace audio_engine {

class ISTFTProcessor {
public:
    /// Precomputes the conjugate twiddle factors for the IFFT.
    ISTFTProcessor() noexcept;

    /// Reconstruct time-domain samples from a one-sided STFT spectrum.
    ///
    /// \param[in]  spectrum  Complex STFT bins (FFT_BINS elements).
    ///                       Typically obtained from STFTProcessor::process()
    ///                       optionally modified by NoiseSuppressor::process().
    /// \param[out] out       Reconstructed PCM samples (FRAME_SIZE elements).
    ///                       Values are the Hann-windowed time-domain signal;
    ///                       see the "Round-trip note" in the file header.
    void process(std::span<const std::complex<float>, FFT_BINS> spectrum,
                 std::span<float, FRAME_SIZE> out) noexcept;

    /// Convenience overload that accepts raw pointers.
    void process(const std::complex<float>* spectrum_data,
                 float* out_data) noexcept {
        process(
            std::span<const std::complex<float>, FFT_BINS>{spectrum_data, FFT_BINS},
            std::span<float, FRAME_SIZE>{out_data, FRAME_SIZE});
    }

private:
    // ── In-place radix-2 DIT IFFT (operates on ifft_buf_) ──────────────────

    void compute_ifft() noexcept;

    // ── State ─────────────────────────────────────────────────────────────────

    /// Conjugate twiddle factors W_N^{-k} = exp(+j 2π k / N) for the IFFT.
    std::array<std::complex<float>, FFT_SIZE / 2> twiddle_conj_{};

    /// Working buffer for the in-place IFFT.
    std::array<std::complex<float>, FFT_SIZE> ifft_buf_{};
};

} // namespace audio_engine
