#include <Helix/Audio/Decoding/Implementation/BuiltInDecoders.h>

#include <Helix/Core/Basics/Assert.h>
#include <Helix/Core/Memory/Allocator.h>
#include <Helix/Core/Strings/Format.h>

#include <charconv>
#include <cstring>

// WAV, FLAC and MP3 through the public domain dr_wav, dr_flac and dr_mp3 (ThirdParty/, see THIRD-PARTY.txt).
namespace helix::audio::third
{
    // A block keeps its size in front of it; the copies free without one.
    constexpr usize kHeader = 16;
    // Damaged headers ask for huge blocks; no clip needs one this large.
    constexpr usize kLargest = sizeof(void*) == 4 ? usize(1) << 28 : usize(1) << 32;

    void* Allocate(usize size)
    {
        if (size > kLargest)
            return nullptr;
        u8* block = static_cast<u8*>(HeapAllocator().Allocate(size + kHeader, kHeader));
        if (!block)
            return nullptr;
        std::memcpy(block, &size, sizeof(size));
        return block + kHeader;
    }

    void Free(void* memory)
    {
        if (!memory)
            return;
        u8* block = static_cast<u8*>(memory) - kHeader;
        usize size;
        std::memcpy(&size, block, sizeof(size));
        HeapAllocator().Free(block, size + kHeader, kHeader);
    }

    void* Reallocate(void* memory, usize size)
    {
        if (size == 0)
        {
            Free(memory);
            return nullptr;
        }
        void* moved = Allocate(size);
        if (moved && memory)
        {
            usize old;
            std::memcpy(&old, static_cast<u8*>(memory) - kHeader, sizeof(old));
            std::memcpy(moved, memory, old < size ? old : size);
        }
        if (moved)
            Free(memory);
        return moved;
    }

    void* OnMalloc(size_t size, void*) { return Allocate(size); }
    void* OnRealloc(void* p, size_t size, void*) { return Reallocate(p, size); }
    void OnFree(void* p, void*) { Free(p); }
}

#define DR_WAV_NO_STDIO
#define DR_FLAC_NO_STDIO
#define DR_MP3_NO_STDIO
#define DR_WAV_NO_WCHAR
#define DRWAV_ASSERT(expression) HX_ASSERT(expression)
#define DRFLAC_ASSERT(expression) HX_ASSERT(expression)
#define DRMP3_ASSERT(expression) HX_ASSERT(expression)
#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push, 0)
#pragma warning(disable : 4244 4245 4456 4457 4701 4702 4706 4996 4505 4127 4100 4189 4146 4334)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wfloat-conversion"
#pragma GCC diagnostic ignored "-Wcast-align"
#ifdef __clang__
#pragma GCC diagnostic ignored "-Wunknown-warning-option"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wcomma"
#endif
#endif
#include <dr_flac/dr_flac.h>
#include <dr_mp3/dr_mp3.h>
#include <dr_wav/dr_wav.h>
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

namespace helix::audio
{
    void ReadLoopComment(StringView comment, AudioLoop& loop, u64& loopEnd)
    {
        usize eq = comment.Find('=');
        if (eq == StringView::npos)
            return;
        StringView key = comment.Substr(0, eq), value = comment.Substr(eq + 1);
        auto is = [&](StringView name) {
            if (key.Size() != name.Size())
                return false;
            for (usize i = 0; i < key.Size(); i++)
                if ((key[i] >= 'a' && key[i] <= 'z' ? char(key[i] - 32) : key[i]) != name[i])
                    return false;
            return true;
        };
        u64 v = 0;
        auto r = std::from_chars(value.Data(), value.Data() + value.Size(), v);
        if (r.ec != std::errc() || r.ptr != value.Data() + value.Size())
            return;
        if (is("LOOPSTART"))
            loop.start = v, loop.present = true;
        else if (is("LOOPLENGTH"))
            loop.length = v;
        else if (is("LOOPEND"))
            loopEnd = v;
    }

    namespace
    {
        template <class T>
        T Callbacks()
        {
            T c{};
            c.onMalloc = &third::OnMalloc;
            c.onRealloc = &third::OnRealloc;
            c.onFree = &third::OnFree;
            return c;
        }

        // Comment loops: a length, or an end; a loop without either runs to the end of the clip.
        void FinishLoop(AudioLoop& loop, u64 end, u64 frames)
        {
            if (!loop.present)
                return;
            if (loop.length == 0 && end > loop.start)
                loop.length = end - loop.start;
            if (loop.length == 0 && frames > loop.start)
                loop.length = frames - loop.start;
            if (frames != 0 && (loop.start >= frames || loop.start + loop.length > frames))
                loop = AudioLoop();
        }

        // ------------------------------------------------------------------ WAV

        class WavStream : public AudioStream
        {
        public:
            ~WavStream() override
            {
                if (open_)
                    drwav_uninit(&wav_);
            }

            bool Open(Span<const u8> data, String& error)
            {
                drwav_allocation_callbacks cb = Callbacks<drwav_allocation_callbacks>();
                if (!drwav_init_memory_with_metadata(&wav_, data.Data(), data.Size(), 0, &cb))
                    return error = "not a WAV file of a format dr_wav reads", false;
                open_ = true;
                format_ = {wav_.sampleRate, wav_.channels, wav_.totalPCMFrameCount};
                u16 tag = wav_.translatedFormatTag;
                if (tag != DR_WAVE_FORMAT_PCM && tag != DR_WAVE_FORMAT_IEEE_FLOAT && tag != DR_WAVE_FORMAT_ALAW && tag != DR_WAVE_FORMAT_MULAW &&
                    tag != DR_WAVE_FORMAT_ADPCM && tag != DR_WAVE_FORMAT_DVI_ADPCM)
                    return error = helix::Format("WAV format tag {}", tag), false;
                for (drwav_uint32 i = 0; i < wav_.metadataCount; i++)
                {
                    const drwav_metadata& m = wav_.pMetadata[i];
                    if (m.type == drwav_metadata_type_smpl && m.data.smpl.sampleLoopCount > 0 && m.data.smpl.pLoops)
                    {
                        const drwav_smpl_loop& l = m.data.smpl.pLoops[0];
                        if (l.lastSampleOffset >= l.firstSampleOffset)
                            loop_ = {true, l.firstSampleOffset, u64(l.lastSampleOffset) - l.firstSampleOffset + 1};
                        if (format_.frames != 0 && loop_.start + loop_.length > format_.frames)
                            loop_ = AudioLoop();
                        break;
                    }
                }
                return true;
            }

        protected:
            u64 ReadFrames(float* out, u64 frames) override { return drwav_read_pcm_frames_f32(&wav_, frames, out); }
            bool SeekFrame(u64 frame) override { return drwav_seek_to_pcm_frame(&wav_, frame) != 0; }

        private:
            drwav wav_{};
            bool open_ = false;
        };

        bool MatchesWav(Span<const u8> d)
        {
            if (d.Size() < 12)
                return false;
            bool riff = (std::memcmp(d.Data(), "RIFF", 4) == 0 || std::memcmp(d.Data(), "RF64", 4) == 0 || std::memcmp(d.Data(), "BW64", 4) == 0) &&
                        std::memcmp(d.Data() + 8, "WAVE", 4) == 0;
            // Sony Wave64: the riff GUID.
            static constexpr u8 kW64[16] = {0x72, 0x69, 0x66, 0x66, 0x2E, 0x91, 0xCF, 0x11, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00};
            bool w64 = d.Size() >= 16 && std::memcmp(d.Data(), kW64, 16) == 0;
            return riff || w64;
        }

        std::unique_ptr<AudioStream> OpenWav(Span<const u8> data, String& error)
        {
            auto s = std::make_unique<WavStream>();
            if (!s->Open(data, error))
                return nullptr;
            return s;
        }

        // ------------------------------------------------------------------ FLAC

        class FlacStream : public AudioStream
        {
        public:
            ~FlacStream() override
            {
                if (flac_)
                    drflac_close(flac_);
            }

            bool Open(Span<const u8> data, String& error)
            {
                drflac_allocation_callbacks cb = Callbacks<drflac_allocation_callbacks>();
                flac_ = drflac_open_memory_with_metadata(data.Data(), data.Size(), &OnMeta, this, &cb);
                if (!flac_)
                    return error = "not a FLAC stream dr_flac reads", false;
                format_ = {flac_->sampleRate, flac_->channels, flac_->totalPCMFrameCount};
                FinishLoop(loop_, loopEnd_, format_.frames);
                return true;
            }

        protected:
            u64 ReadFrames(float* out, u64 frames) override { return drflac_read_pcm_frames_f32(flac_, frames, out); }
            bool SeekFrame(u64 frame) override { return drflac_seek_to_pcm_frame(flac_, frame) != 0; }

        private:
            static void OnMeta(void* user, drflac_metadata* m)
            {
                if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT)
                    return;
                FlacStream& s = *static_cast<FlacStream*>(user);
                drflac_vorbis_comment_iterator it;
                drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount, m->data.vorbis_comment.pComments);
                drflac_uint32 length;
                while (const char* c = drflac_next_vorbis_comment(&it, &length))
                    ReadLoopComment(StringView(c, length), s.loop_, s.loopEnd_);
            }

            drflac* flac_ = nullptr;
            u64 loopEnd_ = 0;
        };

        bool MatchesFlac(Span<const u8> d) { return d.Size() >= 4 && std::memcmp(d.Data(), "fLaC", 4) == 0; }

        std::unique_ptr<AudioStream> OpenFlac(Span<const u8> data, String& error)
        {
            auto s = std::make_unique<FlacStream>();
            if (!s->Open(data, error))
                return nullptr;
            return s;
        }

        // ------------------------------------------------------------------ MP3

        class Mp3Stream : public AudioStream
        {
        public:
            ~Mp3Stream() override
            {
                if (open_)
                    drmp3_uninit(&mp3_);
            }

            bool Open(Span<const u8> data, String& error)
            {
                drmp3_allocation_callbacks cb = Callbacks<drmp3_allocation_callbacks>();
                if (!drmp3_init_memory(&mp3_, data.Data(), data.Size(), &cb))
                    return error = "not an MP3 stream dr_mp3 reads", false;
                open_ = true;
                // The count decodes the frame headers once; the stream is back at its start after it.
                format_ = {mp3_.sampleRate, mp3_.channels, drmp3_get_pcm_frame_count(&mp3_)};
                if (format_.frames == 0)
                    return error = "an MP3 stream without frames", false;
                return true;
            }

        protected:
            u64 ReadFrames(float* out, u64 frames) override { return drmp3_read_pcm_frames_f32(&mp3_, frames, out); }
            bool SeekFrame(u64 frame) override { return drmp3_seek_to_pcm_frame(&mp3_, frame) != 0; }

        private:
            drmp3 mp3_{};
            bool open_ = false;
        };

        // MP3 has no file signature: an ID3v2 tag, or a frame sync with a layer III header.
        bool MatchesMp3(Span<const u8> d)
        {
            if (d.Size() >= 3 && std::memcmp(d.Data(), "ID3", 3) == 0)
                return true;
            return d.Size() >= 4 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0 && (d[1] & 0x06) == 0x02 && (d[2] & 0xF0) != 0xF0 && (d[1] & 0x18) != 0x08;
        }

        std::unique_ptr<AudioStream> OpenMp3(Span<const u8> data, String& error)
        {
            auto s = std::make_unique<Mp3Stream>();
            if (!s->Open(data, error))
                return nullptr;
            return s;
        }

        constexpr StringView kWavExtensions[] = {"wav", "wave", "w64", "rf64", "bwf"};
        constexpr StringView kFlacExtensions[] = {"flac"};
        constexpr StringView kMp3Extensions[] = {"mp3"};
    }

    const AudioDecoder kWavDecoder = {"WAV", Span<const StringView>(kWavExtensions), &MatchesWav, &OpenWav};
    const AudioDecoder kFlacDecoder = {"FLAC", Span<const StringView>(kFlacExtensions), &MatchesFlac, &OpenFlac};
    const AudioDecoder kMp3Decoder = {"MP3", Span<const StringView>(kMp3Extensions), &MatchesMp3, &OpenMp3};

    const AudioDecoder* const* BuiltInDecoders(usize& count)
    {
        static const AudioDecoder* const kAll[] = {&kWavDecoder, &kFlacDecoder, &kVorbisDecoder, &kMp3Decoder};
        count = 4;
        return kAll;
    }
}
