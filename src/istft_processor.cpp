// istft_processor.cpp
// Implementation of the Inverse Short-Time Fourier Transform.
//
// The IFFT is computed via the conjugate-symmetry property:
//   IFFT(X) = (1/N) * conj( FFT( conj(X) ) )
//
// This lets us re-use the same radix-2 DIT butterfly structure as the forward
// FFT (with conjugated twiddle factors) without code duplication.

#include "audio_engine/istft_processor.hpp"

#include <cmath>
#include <numbers>

namespace audio_engine {

// ── Construction ─────────────────────────────────────────────────────────────

ISTFTProcessor::ISTFTProcessor() noexcept {
    // Conjugate twiddle factors for the IFFT:
    //   W_N^{-k} = exp(+j 2π k / N)
    for (std::size_t k = 0; k < FFT_SIZE / 2; ++k) {
        const float angle =
            +2.0f * std::numbers::pi_v<float> *
            static_cast<float>(k) / static_cast<float>(FFT_SIZE);
        twiddle_conj_[k] = {std::cos(angle), std::sin(angle)};
    }
}

// ── process() ────────────────────────────────────────────────────────────────

void ISTFTProcessor::process(
    std::span<const std::complex<float>, FFT_BINS> spectrum,
    std::span<float, FRAME_SIZE> out) noexcept {

    // 1. Reconstruct the full FFT_SIZE complex spectrum using Hermitian
    //    symmetry: X[FFT_SIZE - k] = conj(X[k]).
    //
    //    One-sided layout (bins 0 … FFT_BINS-1 = 0 … FFT_SIZE/2):
    //      k = 0          → DC  (real)
    //      k = 1 … N/2-1  → positive frequencies
    //      k = N/2        → Nyquist (real)

    ifft_buf_[0] = spectrum[0];                      // DC
    ifft_buf_[FFT_SIZE / 2] = spectrum[FFT_SIZE / 2]; // Nyquist

    for (std::size_t k = 1; k < FFT_SIZE / 2; ++k) {
        ifft_buf_[k]            = spectrum[k];
        ifft_buf_[FFT_SIZE - k] = std::conj(spectrum[k]);
    }

    // 2. In-place IFFT.
    compute_ifft();

    // 3. Extract real part (scaled by 1/N) into the output buffer.
    //    The imaginary parts are zero for a Hermitian input; we discard them.
    constexpr float INV_N = 1.0f / static_cast<float>(FFT_SIZE);
    for (std::size_t n = 0; n < FRAME_SIZE; ++n) {
        out[n] = ifft_buf_[n].real() * INV_N;
    }
}

// ── compute_ifft() – radix-2 DIT with conjugate twiddle factors ──────────────
//
// Identical structure to the forward FFT in STFTProcessor::compute_fft(),
// but uses twiddle_conj_[k] = W_N^{-k} = exp(+j 2π k / N) instead of
// W_N^{k} = exp(-j 2π k / N).  The 1/N normalisation is applied by the
// caller (process()) to avoid redundant multiplications per butterfly.

void ISTFTProcessor::compute_ifft() noexcept {
    const std::size_t N = FFT_SIZE;
    const unsigned int LOG2_N = 8u; // log2(256)

    // ── Bit-reversal permutation ──────────────────────────────────────────────
    for (std::size_t i = 0; i < N; ++i) {
        std::size_t r = 0;
        std::size_t tmp = i;
        for (unsigned int b = 0; b < LOG2_N; ++b) {
            r = (r << 1u) | (tmp & 1u);
            tmp >>= 1u;
        }
        if (r > i) {
            std::swap(ifft_buf_[i], ifft_buf_[r]);
        }
    }

    // ── Butterfly passes ─────────────────────────────────────────────────────
    for (unsigned int s = 1; s <= LOG2_N; ++s) {
        const std::size_t span   = std::size_t{1} << s;
        const std::size_t half   = span >> 1u;
        const std::size_t stride = N >> s;

        for (std::size_t k = 0; k < N; k += span) {
            for (std::size_t j = 0; j < half; ++j) {
                const auto& tw = twiddle_conj_[j * stride];
                const auto  u  = ifft_buf_[k + j];
                const auto  t  = tw * ifft_buf_[k + j + half];
                ifft_buf_[k + j]        = u + t;
                ifft_buf_[k + j + half] = u - t;
            }
        }
    }
}

} // namespace audio_engine
