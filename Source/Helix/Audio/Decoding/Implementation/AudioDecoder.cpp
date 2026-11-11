#include <Helix/Audio/Decoding/AudioDecoder.h>
#include <Helix/Audio/Decoding/Implementation/BuiltInDecoders.h>

#include <Helix/Core/Strings/Format.h>
#include <Helix/Platform/Threading/Thread.h>

namespace helix::audio
{
    namespace
    {
        // Longer clips are streamed, not decoded whole: about 12 minutes of 8 channels at 48 kHz.
        constexpr u64 kMaxDecodedSamples = u64(1) << 28;

        struct Registry
        {
            platform::Mutex mutex;
            Array<AudioDecoder> decoders;

            Registry()
            {
                usize count = 0;
                const AudioDecoder* const* builtIn = BuiltInDecoders(count);
                for (usize i = 0; i < count; i++)
                    decoders.Push(*builtIn[i]);
            }
        };

        Registry& Decoders()
        {
            static Registry registry;
            return registry;
        }

        bool SameExtension(StringView a, StringView b)
        {
            if (a.Size() != b.Size())
                return false;
            for (usize i = 0; i < a.Size(); i++)
            {
                char x = a[i] >= 'A' && a[i] <= 'Z' ? char(a[i] + 32) : a[i];
                if (x != b[i])
                    return false;
            }
            return true;
        }
    }

    bool AudioStream::Fail(StringView what)
    {
        if (!failed_)
        {
            failed_ = true;
            error_ = String(what);
        }
        return false;
    }

    u64 AudioStream::Read(float* out, u64 frames)
    {
        if (failed_ || frames == 0)
            return 0;
        u64 n = ReadFrames(out, frames);
        position_ += n;
        return n;
    }

    bool AudioStream::Seek(u64 frame)
    {
        if (failed_)
            return false;
        if (format_.frames != 0 && frame > format_.frames)
            return Fail(helix::Format("a seek to frame {} of {}", frame, format_.frames));
        if (!SeekFrame(frame))
            return Fail(helix::Format("a seek to frame {} failed", frame));
        position_ = frame;
        return true;
    }

    void RegisterAudioDecoder(const AudioDecoder& decoder)
    {
        Registry& r = Decoders();
        platform::ScopedLock lock(r.mutex);
        for (AudioDecoder& d : r.decoders)
            if (d.name == decoder.name)
            {
                d = decoder;
                return;
            }
        r.decoders.Push(decoder);
    }

    void AudioDecoders(Array<AudioDecoder>& out)
    {
        Registry& r = Decoders();
        platform::ScopedLock lock(r.mutex);
        out = r.decoders;
    }

    bool FindAudioDecoder(Span<const u8> data, StringView extension, AudioDecoder& out)
    {
        if (!extension.Empty() && extension[0] == '.')
            extension = extension.Substr(1);
        Registry& r = Decoders();
        platform::ScopedLock lock(r.mutex);
        for (const AudioDecoder& d : r.decoders)
            if (d.matches && d.matches(data))
            {
                out = d;
                return true;
            }
        for (const AudioDecoder& d : r.decoders)
            for (StringView e : d.extensions)
                if (!extension.Empty() && SameExtension(extension, e))
                {
                    out = d;
                    return true;
                }
        return false;
    }

    std::unique_ptr<AudioStream> OpenAudio(Span<const u8> data, String& error, StringView extension)
    {
        AudioDecoder decoder;
        if (!FindAudioDecoder(data, extension, decoder))
        {
            error = "not a sound of a known format";
            return nullptr;
        }
        std::unique_ptr<AudioStream> s = decoder.open(data, error);
        if (s && (s->Format().channels == 0 || s->Format().channels > kMaxChannels || s->Format().sampleRate == 0))
        {
            error = helix::Format("{} channels at {} Hz", s->Format().channels, s->Format().sampleRate);
            return nullptr;
        }
        return s;
    }

    bool DecodeAudio(Span<const u8> data, AudioData& out, String& error, StringView extension)
    {
        out = AudioData();
        std::unique_ptr<AudioStream> s = OpenAudio(data, error, extension);
        if (!s)
            return false;
        out.format = s->Format();
        out.loop = s->Loop();
        u32 channels = out.format.channels;
        if (out.format.frames * channels > kMaxDecodedSamples)
            return error = helix::Format("{} frames are too long to decode whole", out.format.frames), false;
        out.samples.Reserve(usize(out.format.frames) * channels);
        constexpr u64 kChunk = 4096;
        while (true)
        {
            usize at = out.samples.Size();
            if (at + kChunk * channels > kMaxDecodedSamples)
                return error = "a stream too long to decode whole", false;
            out.samples.Resize(at + usize(kChunk) * channels);
            u64 n = s->Read(out.samples.Data() + at, kChunk);
            out.samples.Resize(at + usize(n) * channels);
            if (n == 0)
                break;
        }
        if (s->Failed())
            return error = s->Error(), false;
        out.format.frames = out.samples.Size() / channels;
        return true;
    }
}
