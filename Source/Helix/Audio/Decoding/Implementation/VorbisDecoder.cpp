#include <Helix/Audio/Decoding/Implementation/BuiltInDecoders.h>

#include <Helix/Core/Strings/Format.h>

#include <cstring>

// Ogg Vorbis through the public domain stb_vorbis (ThirdParty/stb_vorbis, see THIRD-PARTY.txt). It works in a buffer
// of ours, so it never calls malloc; a buffer that is too small is doubled.
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_MAX_CHANNELS 8
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push, 0)
#pragma warning(disable : 4244 4245 4456 4457 4701 4702 4706 4996 4505 4127 4100 4189 4146 4334 4267)
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
#pragma GCC diagnostic ignored "-Wunused-value"
#ifdef __clang__
#pragma GCC diagnostic ignored "-Wunknown-warning-option"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wcomma"
#endif
#endif
#include <stb_vorbis/stb_vorbis.c>
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
// stb_vorbis leaves short macro names behind.
#undef L
#undef C
#undef R

namespace helix::audio
{
    namespace
    {
        constexpr usize kFirstBuffer = 256 * 1024;
        constexpr usize kLargestBuffer = 32 * 1024 * 1024;

        // Vorbis channel order to ours (front left, front right, center, LFE, back left, back right, side left, side
        // right), by channel count: out[i] = in[kOrder[channels][i]].
        constexpr u8 kOrder[9][8] = {
            {},
            {0},
            {0, 1},
            {0, 2, 1},
            {0, 1, 2, 3},
            {0, 2, 1, 3, 4},
            {0, 2, 1, 5, 3, 4},
            {0, 2, 1, 6, 5, 3, 4},
            {0, 2, 1, 7, 5, 6, 3, 4},
        };

        // Every page of the file belongs to one logical stream: chained and multiplexed files are refused.
        bool SingleStream(Span<const u8> d, String& error)
        {
            usize at = 0;
            bool first = true;
            u32 serial = 0;
            while (at + 27 <= d.Size())
            {
                if (std::memcmp(d.Data() + at, "OggS", 4) != 0)
                    return error = helix::Format("no Ogg page at byte {}", at), false;
                u32 s;
                std::memcpy(&s, d.Data() + at + 14, 4);
                if (first)
                    serial = s, first = false;
                else if (s != serial)
                    return error = "an Ogg file of more than one stream (chained or multiplexed)", false;
                u8 segments = d[at + 26];
                if (at + 27 + segments > d.Size())
                    break;
                usize body = 0;
                for (u32 i = 0; i < segments; i++)
                    body += d[at + 27 + i];
                at += 27 + segments + body;
            }
            return true;
        }

        class VorbisStream : public AudioStream
        {
        public:
            ~VorbisStream() override
            {
                if (vorbis_)
                    stb_vorbis_close(vorbis_);
            }

            bool Open(Span<const u8> data, String& error)
            {
                if (!SingleStream(data, error))
                    return false;
                if (data.Size() > 0x7FFFFFFF)
                    return error = "an Ogg file over 2 GB", false;
                for (usize size = kFirstBuffer; size <= kLargestBuffer; size *= 2)
                {
                    memory_.Resize(size);
                    stb_vorbis_alloc alloc{memory_.Data(), int(size)};
                    int code = 0;
                    vorbis_ = stb_vorbis_open_memory(data.Data(), int(data.Size()), &code, &alloc);
                    if (vorbis_)
                        break;
                    if (code != VORBIS_outofmem)
                        return error = helix::Format("not an Ogg Vorbis stream stb_vorbis reads (error {})", code), false;
                }
                if (!vorbis_)
                    return error = "an Ogg Vorbis stream that needs more than 32 MB to decode", false;
                stb_vorbis_info info = stb_vorbis_get_info(vorbis_);
                format_ = {info.sample_rate, u32(info.channels), stb_vorbis_stream_length_in_samples(vorbis_)};
                stb_vorbis_comment comments = stb_vorbis_get_comment(vorbis_);
                u64 loopEnd = 0;
                for (int i = 0; i < comments.comment_list_length; i++)
                    if (comments.comment_list[i])
                        ReadLoopComment(StringView(comments.comment_list[i]), loop_, loopEnd);
                if (loop_.present)
                {
                    if (loop_.length == 0 && loopEnd > loop_.start)
                        loop_.length = loopEnd - loop_.start;
                    if (loop_.length == 0 && format_.frames > loop_.start)
                        loop_.length = format_.frames - loop_.start;
                    if (format_.frames != 0 && loop_.start + loop_.length > format_.frames)
                        loop_ = AudioLoop();
                }
                return true;
            }

        protected:
            u64 ReadFrames(float* out, u64 frames) override
            {
                u32 c = format_.channels;
                u64 total = 0;
                float frame[8];
                while (total < frames)
                {
                    u64 chunk = frames - total < 4096 ? frames - total : 4096;
                    int n = stb_vorbis_get_samples_float_interleaved(vorbis_, int(c), out + total * c, int(chunk * c));
                    if (n <= 0)
                        break;
                    if (c > 2)
                        for (int f = 0; f < n; f++)
                        {
                            float* p = out + (total + u64(f)) * c;
                            std::memcpy(frame, p, c * sizeof(float));
                            for (u32 i = 0; i < c; i++)
                                p[i] = frame[kOrder[c][i]];
                        }
                    total += u64(n);
                }
                if (total < frames && stb_vorbis_get_error(vorbis_) != VORBIS__no_error && stb_vorbis_get_error(vorbis_) != VORBIS_seek_failed)
                {
                    // Running out of data at the end is how a stream ends; anything else is damage.
                    int e = stb_vorbis_get_error(vorbis_);
                    if (e != VORBIS_need_more_data && e != VORBIS_unexpected_eof)
                        Fail(helix::Format("a damaged Vorbis packet (error {})", e));
                }
                return total;
            }

            bool SeekFrame(u64 frame) override { return frame <= 0xFFFFFFFFu && stb_vorbis_seek(vorbis_, unsigned(frame)) != 0; }

        private:
            Array<char> memory_;
            stb_vorbis* vorbis_ = nullptr;
        };

        bool MatchesVorbis(Span<const u8> d) { return d.Size() >= 36 && std::memcmp(d.Data(), "OggS", 4) == 0 && std::memcmp(d.Data() + 29, "vorbis", 6) == 0; }

        std::unique_ptr<AudioStream> OpenVorbis(Span<const u8> data, String& error)
        {
            auto s = std::make_unique<VorbisStream>();
            if (!s->Open(data, error))
                return nullptr;
            return s;
        }

        constexpr StringView kVorbisExtensions[] = {"ogg", "oga"};
    }

    const AudioDecoder kVorbisDecoder = {"Ogg Vorbis", Span<const StringView>(kVorbisExtensions), &MatchesVorbis, &OpenVorbis};
}
