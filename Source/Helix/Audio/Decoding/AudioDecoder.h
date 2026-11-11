#pragma once

#include <Helix/Core/Basics/Span.h>
#include <Helix/Core/Containers/Array.h>
#include <Helix/Core/Strings/String.h>

#include <memory>

namespace helix::audio
{
    struct AudioFormat
    {
        u32 sampleRate = 0;
        u32 channels = 0;
        // 0 when the stream does not tell.
        u64 frames = 0;
    };

    // A loop region in frames, from the file (WAV smpl, LOOPSTART and LOOPLENGTH comments of Vorbis and FLAC).
    struct AudioLoop
    {
        bool present = false;
        u64 start = 0;
        u64 length = 0;
    };

    // Channel order of every decoder: front left, front right, center, LFE, back left, back right, side left, side right
    // (the WAV and FLAC order; Vorbis files are reordered to it).
    inline constexpr u32 kMaxChannels = 8;

    // An open stream of one decoder over bytes in memory (a whole file, or a mapped one for streaming), which must outlive
    // it. Reads interleaved float frames in chunks of any size.
    class AudioStream
    {
    public:
        virtual ~AudioStream() = default;

        const AudioFormat& Format() const { return format_; }
        const AudioLoop& Loop() const { return loop_; }
        u64 Position() const { return position_; }
        bool Failed() const { return failed_; }
        const String& Error() const { return error_; }

        // Up to `frames` frames into `out` (frames * channels floats); fewer at the end, 0 after it or on an error.
        u64 Read(float* out, u64 frames);
        // To a frame from the start; false (and Failed) when the stream cannot get there.
        bool Seek(u64 frame);

    protected:
        virtual u64 ReadFrames(float* out, u64 frames) = 0;
        virtual bool SeekFrame(u64 frame) = 0;
        bool Fail(StringView what);

        AudioFormat format_;
        AudioLoop loop_;
        u64 position_ = 0;
        bool failed_ = false;
        String error_;
    };

    // A file format that can be decoded, found by its signature first and by the extension for formats without one.
    struct AudioDecoder
    {
        StringView name;
        // Lower case, without the dot. The array must outlive the registration.
        Span<const StringView> extensions;
        bool (*matches)(Span<const u8> data) = nullptr;
        // Null with `error` set when the data is not a stream of the format.
        std::unique_ptr<AudioStream> (*open)(Span<const u8> data, String& error) = nullptr;
    };

    // Adds a decoder, or replaces the one with the same name. The built in decoders (WAV, FLAC, MP3, Ogg Vorbis) are
    // registered on first use.
    void RegisterAudioDecoder(const AudioDecoder& decoder);
    void AudioDecoders(Array<AudioDecoder>& out);
    bool FindAudioDecoder(Span<const u8> data, StringView extension, AudioDecoder& out);

    // Opens any registered format; `extension` (with or without the dot) helps formats without a signature.
    std::unique_ptr<AudioStream> OpenAudio(Span<const u8> data, String& error, StringView extension = {});

    // A whole clip, interleaved float.
    struct AudioData
    {
        AudioFormat format;
        AudioLoop loop;
        Array<float> samples;
    };

    // Decodes a whole clip; streams that do not tell their length are read to the end.
    bool DecodeAudio(Span<const u8> data, AudioData& out, String& error, StringView extension = {});
}
