# Ultra-Low-Latency Audio Isolation for Hearables

A production-quality C++20 library that isolates speech from background noise
in real time on a single Bluetooth/hearable SoC.  The entire processing chain
— from raw PCM ingestion to isolated PCM output — completes in well under one
5 ms frame budget at 16 kHz.

---

## Architecture overview

```
┌──────────────────────────────────────────────────────────────────┐
│                         Pipeline                                 │
│                                                                  │
│  ISR/DMA ──► CircularBuffer ──► STFTProcessor ──► NoiseSuppressor│
│  (producer)   (lock-free SPSC)  (Hann + FFT)    (Wiener filter) │
│                                                                  │
│                              ISTFTProcessor ──► PCM output       │
│                              (IFFT + synthesis)  (consumer)      │
└──────────────────────────────────────────────────────────────────┘
```

| Stage | Class | Purpose |
|---|---|---|
| Ingestion | `CircularBuffer<float,4096>` | Lock-free SPSC ring buffer; wait-free for producer ISR |
| Analysis | `STFTProcessor` | Hann window → zero-pad to 256 → radix-2 FFT → 129-bin spectrum |
| Suppression | `NoiseSuppressor` | Wiener filter with exponential noise tracking and VAD |
| Synthesis | `ISTFTProcessor` | Hermitian reconstruction → radix-2 IFFT → 80-sample PCM frame |
| Top level | `Pipeline` | Ties everything together; optional `DebugFrame` diagnostics |

---

## Signal parameters

| Parameter | Value |
|---|---|
| Sample rate | 16 000 Hz |
| Frame duration | 5 ms |
| Frame size | 80 samples |
| FFT size | 256 points (next power-of-two ≥ 80) |
| Frequency resolution | 62.5 Hz / bin |
| One-sided spectrum | 129 complex bins (0 Hz … 8 000 Hz) |
| Ring-buffer capacity | 4 096 samples ≈ 256 ms headroom |

---

## Key design decisions

### Lock-free SPSC circular buffer
`CircularBuffer<T, Capacity>` uses monotonically-increasing `std::atomic`
head/tail counters with cache-line separation (`alignas(64)`) to eliminate
false sharing between the ISR producer and the DSP consumer thread.
No mutex, no heap allocation after construction.

### Wiener-filter noise suppressor
`NoiseSuppressor` operates entirely in the frequency domain:

1. **Noise initialisation** – the first `NOISE_INIT_FRAMES` (10) frames are
   assumed to be noise-only; their average power spectrum seeds the noise PSD.
2. **Voice Activity Detection** – a simple energy-ratio test:  
   `speech` when `Σ power[k] / Σ noise_psd[k] > 2.0` (≈ +3 dB).
3. **Noise tracking** – during silence, the noise PSD is updated with an
   exponential moving average (`α = 0.95`) so the suppressor adapts to slowly
   changing background noise.
4. **Wiener gain** – per-bin `G[k] = SNR[k] / (SNR[k]+1)`, floored at `0.08`
   to prevent musical noise artefacts.
5. **Gain smoothing** – first-order IIR (`β = 0.85`) across successive frames
   reduces rapid fluctuations that the ear perceives as distortion.

### Inverse STFT
`ISTFTProcessor` uses the conjugate-symmetry IFFT identity:

```
IFFT(X) = (1/N) · conj( FFT( conj(X) ) )
```

This reuses the same radix-2 DIT butterfly structure as the forward FFT,
with pre-computed conjugate twiddle factors `exp(+j 2π k/N)`.

---

## Quick start

### Prerequisites
- CMake ≥ 3.20
- A C++20-capable compiler (GCC 11+, Clang 14+, MSVC 19.30+)

### Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### Run tests

```bash
ctest --test-dir build --output-on-failure
```

All **43 unit tests** across 5 suites should pass:

| Suite | Tests | What it covers |
|---|---|---|
| `CircularBufferTests` | 10 | Push/pop, wrap-around, overwrite, SPSC concurrency |
| `STFTProcessorTests` | 8 | DC tone, sinusoid peak-bin, Parseval, idempotency |
| `NoiseSuppressorTests` | 9 | Init phase, VAD, gain floor, noise tracking, reset |
| `ISTFTProcessorTests` | 7 | Round-trip, DC, frequency preservation, repeatability |
| `PipelineTests` | 7 | End-to-end framing, energy reduction, debug frame, pass-through |

### Run the latency benchmark

```bash
./build/latency_benchmark
```

Example output (Intel Core i7, Release build, `-O3 -ffast-math`):

```
Full pipeline (STFT + Wiener filter + ISTFT)
────────────────────────────────────────────
  Min     :     3.21 µs
  Mean    :     4.18 µs
  P99     :     7.02 µs
  Budget  :  5000.00 µs  (one frame duration at 16000 Hz)
  Within  : 10000 / 10000 frames (100.0%)
```

---

## Usage example

```cpp
#include "audio_engine/pipeline.hpp"
using namespace audio_engine;

Pipeline pipeline;                          // noise suppression ON by default

// ── ISR / DMA callback (producer) ────────────────────────────────────────────
void dma_isr(const float* pcm, std::size_t n) {
    pipeline.ingest(pcm, n);               // non-blocking; overwrites on overflow
}

// ── DSP thread (consumer) ─────────────────────────────────────────────────────
void dsp_thread() {
    std::array<float, FRAME_SIZE> out_pcm;
    Pipeline::DebugFrame dbg;

    while (running) {
        if (pipeline.process_next_frame(out_pcm, &dbg)) {
            // out_pcm: 80 isolated speech samples (Hann-windowed)
            // dbg.vad_active: true if speech detected
            // dbg.wiener_gain: per-bin suppression applied
            send_to_dac(out_pcm.data(), FRAME_SIZE);
        } else {
            wait_for_data();
        }
    }
}
```

### Pass-through / bypass mode

```cpp
Pipeline::Config cfg;
cfg.enable_noise_suppression = false;    // STFT → ISTFT only, no filtering
Pipeline pipeline(cfg);
```

---

## File structure

```
├── include/audio_engine/
│   ├── audio_frame.hpp        Pipeline constants (SAMPLE_RATE, FRAME_SIZE, …)
│   ├── circular_buffer.hpp    Lock-free SPSC ring buffer
│   ├── stft_processor.hpp     Short-Time Fourier Transform (analysis)
│   ├── noise_suppressor.hpp   Wiener-filter noise suppressor + VAD
│   ├── istft_processor.hpp    Inverse STFT (synthesis)
│   └── audio_engine.hpp       Low-level STFT engine (used by Pipeline)
│
├── src/
│   ├── stft_processor.cpp
│   ├── audio_engine.cpp
│   ├── noise_suppressor.cpp
│   ├── istft_processor.cpp
│   └── pipeline.cpp
│
├── tests/
│   ├── test_circular_buffer.cpp
│   ├── test_stft_processor.cpp
│   ├── test_noise_suppressor.cpp
│   ├── test_istft_processor.cpp
│   └── test_pipeline.cpp
│
├── benchmarks/
│   └── latency_benchmark.cpp  Per-frame latency profiler (min/max/P99)
│
└── CMakeLists.txt
```

---

## CMake options

| Option | Default | Description |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | `Release` uses `-O3 -ffast-math -funroll-loops` |
| `BUILD_TESTS` | `ON` | Build and register `ctest` unit tests |
| `BUILD_BENCHMARKS` | `ON` | Build `latency_benchmark` executable |

---

## Extending the pipeline

To add a custom spectral processing stage (e.g., equaliser, beamformer),
subclass or wrap `NoiseSuppressor` and insert it between the STFT and ISTFT
steps inside `Pipeline::process_next_frame()`.  The one-sided complex spectrum
(`std::array<std::complex<float>, FFT_BINS>`) is the natural exchange format.

---

## License

MIT — see `LICENSE` for details.
