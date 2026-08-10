// miniaudio is header-only. This file is the one that emits the bodies.
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <iostream>
#include <tt/voice.hpp>

#include "sherpa-onnx/c-api/c-api.h"
#include <whisper.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace tt
{

    struct Voice::Impl
    {
        VoiceOptions options;

        whisper_context *whisper = nullptr;
        const SherpaOnnxOfflineTts *tts = nullptr;

        ma_device capture{};
        ma_device playback{};
        // Uninit-ing a device the constructor never reached is not safe.
        bool capture_ready = false;
        bool playback_ready = false;
        bool recording = false;

        // 16 kHz mono float, appended by the capture callback
        std::vector<float> recorded;

        // Kokoro's output, drained by the playback callback.
        // play_cursor is atomic: that callback and speak()'s wait loop run at once.
        std::vector<float> to_play;
        std::atomic<std::size_t> play_cursor{0};
        int play_rate = 24000;

        // Static for a plain function pointer, what dataCallback takes.
        // Members, not free functions: Impl is private.
        static void capture_callback(ma_device *device, void *output,
                                     const void *input, ma_uint32 frame_count);
        static void playback_callback(ma_device *device, void *output,
                                      const void *input, ma_uint32 frame_count);

        ~Impl();
    };

    // Devices first: no callback mid-flight over `recorded` or `to_play`.
    Voice::Impl::~Impl()
    {
        if (capture_ready)
        {
            ma_device_uninit(&capture);
        }
        if (playback_ready)
        {
            ma_device_uninit(&playback);
        }
        if (tts != nullptr)
        {
            SherpaOnnxDestroyOfflineTts(tts);
        }
        if (whisper != nullptr)
        {
            whisper_free(whisper);
        }
    }

    // Both on miniaudio's realtime thread: no allocation, no locking, no I/O.
    // pUserData is the impl_ pointer handed to ma_device_init.

    void Voice::Impl::capture_callback(ma_device *device, void *output,
                                       const void *input, ma_uint32 frame_count)
    {
        (void)output; // capture-only device, null here

        Impl *state = static_cast<Impl *>(device->pUserData);
        const float *samples = static_cast<const float *>(input);

        for (ma_uint32 i = 0; i < frame_count; ++i)
        {
            state->recorded.push_back(samples[i]);
        }
    }

    // Zero-fills past the end of to_play: the tail is silence, not a stale buffer.
    void Voice::Impl::playback_callback(ma_device *device, void *output,
                                        const void *input, ma_uint32 frame_count)
    {
        (void)input; // playback-only device, null here

        Impl *state = static_cast<Impl *>(device->pUserData);
        float *out = static_cast<float *>(output);

        for (ma_uint32 i = 0; i < frame_count; ++i)
        {
            const std::size_t at = state->play_cursor + i;
            out[i] = (at < state->to_play.size()) ? state->to_play[at] : 0.0f;
        }
        state->play_cursor += frame_count;
    }


    Voice::Voice(VoiceOptions options)
        : impl_(std::make_unique<Impl>())
    {
        impl_->options = std::move(options);
        Impl &state = *impl_;


        whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu = true; // Metal on Apple Silicon; harmless elsewhere

        state.whisper = whisper_init_from_file_with_params(
            state.options.whisper_model.c_str(), cparams);
        if (state.whisper == nullptr)
        {
            throw std::runtime_error("cannot load whisper model: " +
                                     state.options.whisper_model);
        }


        // Named locals, not temporaries: the config keeps the const char* past this line.
        const std::string dir = state.options.kokoro_dir;
        const std::string model = dir + "/model.onnx";
        const std::string voices = dir + "/voices.bin";
        const std::string tokens = dir + "/tokens.txt";
        const std::string espeak = dir + "/espeak-ng-data"; // the phonemiser
        const std::string dict = dir + "/dict";
        const std::string lexicon = dir + "/lexicon-us-en.txt";

        SherpaOnnxOfflineTtsConfig config;
        // vits, matcha, kitten share the struct; non-null garbage => sherpa loads a missing model.
        std::memset(&config, 0, sizeof(config));

        config.model.kokoro.model = model.c_str();
        config.model.kokoro.voices = voices.c_str();
        config.model.kokoro.tokens = tokens.c_str();
        config.model.kokoro.data_dir = espeak.c_str();
        config.model.kokoro.dict_dir = dict.c_str();
        config.model.kokoro.lexicon = lexicon.c_str(); // comma-separated if several
        config.model.num_threads = 2;
        config.model.provider = "cpu";
        config.model.debug = 0;
        // Kokoro takes the whole string; other values are ignored, loudly, every turn
        config.max_num_sentences = 1;
        // Speed belongs to SherpaOnnxGenerationConfig at speak() time, not here.

        state.tts = SherpaOnnxCreateOfflineTts(&config);
        if (state.tts == nullptr)
        {
            throw std::runtime_error("cannot load Kokoro from " + dir);
        }


        ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
        cfg.capture.format = ma_format_f32; // whisper wants f32 mono at exactly 16 kHz
        cfg.capture.channels = 1;
        cfg.sampleRate = 16000;
        cfg.dataCallback = &Impl::capture_callback;
        cfg.pUserData = impl_.get();

        if (ma_device_init(nullptr, &cfg, &state.capture) != MA_SUCCESS)
        {
            throw std::runtime_error("cannot open the microphone");
        }
        state.capture_ready = true;
    }

    // Here and not the header: deleting a unique_ptr<Impl> needs the complete type.
    Voice::~Voice() = default;


    // The first ma_device_start raises the macOS mic prompt; refused => silence forever, no error.
    void Voice::start_listening()
    {
        Impl &state = *impl_;
        if (state.recording)
        {
            return;
        }

        state.recorded.clear();
        state.recorded.reserve(16000 * 30); // 30 s of headroom for the realtime callback

        if (ma_device_start(&state.capture) != MA_SUCCESS)
        {
            throw std::runtime_error("Cannot start microphone, check system settings.");
        }
        state.recording = true;
    }

    // A whisper failure returns "" rather than throwing; an empty turn is retryable.
    std::string Voice::stop_listening()
    {
        Impl &state = *impl_;
        if (!state.recording)
        {
            return {};
        }

        // Stopping quiesces the callback; recorded is ours to read unlocked.
        ma_device_stop(&state.capture);
        state.recording = false;

        if (state.recorded.size() < 16000 / 2) // half a second, perhaps a slipped finger
        {
            return {};
        }

        whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        params.print_progress = false;
        params.print_realtime = false;
        params.print_timestamps = false;
        params.language = "en";
        params.n_threads = 4;

        if (whisper_full(state.whisper, params, state.recorded.data(),
                         static_cast<int>(state.recorded.size())) != 0)
        {
            return {};
        }

        std::string text;
        const int segments = whisper_full_n_segments(state.whisper);
        for (int i = 0; i < segments; ++i)
        {
            text += whisper_full_get_segment_text(state.whisper, i);
        }
        // TODO: strip whisper's non-speech markers ("[BLANK_AUDIO]", "(silence)").

        // stderr, not stdout: out of the conversation.
        std::cerr << "heard: \"" << text << "\"\n";
        return text;
    }

    std::string Voice::listen()
    {
        start_listening();

        std::cout << "\nRecording. Press Enter to stop. " << std::flush;
        std::string ignored;
        std::getline(std::cin, ignored);

        return stop_listening();
    }

    void Voice::speak(std::string_view text)
    {
        Impl &state = *impl_;
        if (text.empty())
        {
            return;
        }
        SherpaOnnxGenerationConfig cfg = {};
        cfg.sid = state.options.speaker;
        cfg.speed = state.options.speed; // larger is faster
        cfg.silence_scale = 0.2f;

        std::string string_text(text);
        const SherpaOnnxGeneratedAudio *audio = SherpaOnnxOfflineTtsGenerateWithConfig(state.tts, string_text.c_str(), &cfg, nullptr, nullptr);

        if (audio == nullptr)
        {
            throw std::runtime_error("Kokoro could not synthesise: " + string_text);
        }

        // Copy, not alias: `audio` is freed below, the callback reads to_play for seconds.
        state.to_play.assign(audio->samples, audio->samples + audio->n);
        state.play_cursor = 0; // must be reset before the device starts
        state.play_rate = audio->sample_rate;

        SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);

        // Not in the constructor: Kokoro reports the sample rate only once it has synthesised.
        // Built once; re-initialising per utterance is audible.
        if (!state.playback_ready)
        {
            ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
            cfg.playback.format = ma_format_f32;
            cfg.playback.channels = 1;
            cfg.sampleRate = static_cast<ma_uint32>(state.play_rate);
            cfg.dataCallback = &Impl::playback_callback;
            cfg.pUserData = impl_.get();

            if (ma_device_init(nullptr, &cfg, &state.playback) != MA_SUCCESS)
            {
                throw std::runtime_error("cannot open the speaker");
            }
            state.playback_ready = true;
        }

        if (ma_device_start(&state.playback) != MA_SUCCESS)
        {
            throw std::runtime_error("cannot start the speaker");
        }

        // Sleep, not spin. 10 ms of overshoot at the tail is inaudible.
        while (state.play_cursor < state.to_play.size())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        ma_device_stop(&state.playback);
    }

}
