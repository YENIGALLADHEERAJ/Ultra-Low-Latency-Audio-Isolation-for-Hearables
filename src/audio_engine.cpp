// audio_engine.cpp
// Implementation of the top-level AudioEngine.

#include "audio_engine/audio_engine.hpp"

namespace audio_engine {

void AudioEngine::ingest(const float* samples, std::size_t count) noexcept {
    // push_overwrite is non-blocking: the oldest samples are discarded if the
    // ring buffer cannot accommodate the new data.
    input_buffer_.push_overwrite(samples, count);
}

bool AudioEngine::process_next_frame(SpectrumFrame& out) noexcept {
    if (!input_buffer_.pop(frame_scratch_.data(), FRAME_SIZE)) {
        return false;
    }

    stft_.process(frame_scratch_.data(), out.bins.data());

    out.magnitude       = stft_.magnitude();
    out.power           = stft_.power();
    out.sequence_number = ++frame_counter_;

    return true;
}

} // namespace audio_engine
