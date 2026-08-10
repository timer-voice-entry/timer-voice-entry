#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace tt
{

    struct VoiceOptions
    {
        std::string whisper_model = "models/ggml-base.en.bin";
        std::string kokoro_dir = "models/kokoro-multi-lang-v1_0";
        // Speaker index within the Kokoro voices file
        int speaker = 0;
        float speed = 1.3f;
    };

    // Microphone in, speaker out. Both models stay resident.
    class Voice
    {
    public:
        explicit Voice(VoiceOptions options);
        ~Voice();

        Voice(const Voice &) = delete;
        Voice &operator=(const Voice &) = delete;

        // Returns immediately. A second call while recording does nothing.
        void start_listening();

        // Stops the microphone and transcribes. Empty on too little audio, or never started.
        std::string stop_listening();

        // Terminal push-to-talk: start, block on Enter, stop.
        std::string listen();

        // Blocks until the audio has finished playing.
        void speak(std::string_view text);

    private:
        // Pimpl: keeps the miniaudio, whisper.cpp and sherpa-onnx headers out of this one.
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}
