// pipeline.cpp
// Implementation of the end-to-end audio isolation Pipeline.

#include "audio_engine/pipeline.hpp"

namespace audio_engine {

// ── Construction ─────────────────────────────────────────────────────────────

Pipeline::Pipeline() noexcept : Pipeline(Config{}) {}

Pipeline::Pipeline(Config cfg) noexcept
    : cfg_(cfg) {}

// ── Producer API ─────────────────────────────────────────────────────────────

void Pipeline::ingest(const float* samples, std::size_t count) noexcept {
    engine_.ingest(samples, count);
}

// ── Consumer API ─────────────────────────────────────────────────────────────

bool Pipeline::process_next_frame(std::array<float, FRAME_SIZE>& out_pcm,
                                  DebugFrame* debug) noexcept {
    // 1. Ask the audio engine to dequeue one frame and compute its STFT.
    AudioEngine::SpectrumFrame sf;
    if (!engine_.process_next_frame(sf)) {
        return false;
    }

    // 2. Optionally populate the debug snapshot before modification.
    if (debug) {
        debug->spectrum        = sf.bins;
        debug->magnitude       = sf.magnitude;
        debug->power           = sf.power;
        debug->sequence_number = sf.sequence_number;
    }

    // 3. Apply Wiener-filter noise suppression (modifies sf.bins in-place).
    bool vad = false;
    if (cfg_.enable_noise_suppression) {
        vad = suppressor_.process(sf.bins, sf.power);
    }
    last_vad_ = vad;

    if (debug) {
        debug->wiener_gain = suppressor_.wiener_gain();
        debug->vad_active  = vad;
    }

    // 4. Reconstruct time-domain PCM via inverse STFT.
    istft_.process(sf.bins.data(), out_pcm.data());

    ++frame_counter_;
    return true;
}

// ── Diagnostics ──────────────────────────────────────────────────────────────

std::size_t Pipeline::buffered_samples() const noexcept {
    return engine_.buffered_samples();
}

} // namespace audio_engine
