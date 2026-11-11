#include <Generated/TestSounds.h>

#include <Helix/Audio/Decoding/AudioDecoder.h>
#include <Helix/Testing/Test.h>

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace helix;
using namespace helix::audio;
namespace sounds = helix::audio::testing;

namespace
{
    template <usize N>
    Span<const u8> Bytes(const unsigned char (&a)[N])
    {
        return Span<const u8>(a, N);
    }

    template <usize N>
    Array<float> Reference(const unsigned char (&a)[N])
    {
        Array<float> f;
        f.Resize(N / 4);
        std::memcpy(f.Data(), a, f.Size() * 4);
        return f;
    }

    bool Decode(Span<const u8> bytes, AudioData& out, StringView extension = {})
    {
        String error;
        if (!DecodeAudio(bytes, out, error, extension))
        {
            std::printf("    %s\n", error.CStr());
            return false;
        }
        return true;
    }

    // The largest difference, and whether the lengths agree.
    bool Close(const Array<float>& a, const Array<float>& b, double tolerance, const char* what)
    {
        if (a.Size() != b.Size())
        {
            std::printf("    %s: %zu samples, the reference has %zu\n", what, a.Size(), b.Size());
            return false;
        }
        double worst = 0;
        for (usize i = 0; i < a.Size(); i++)
            worst = std::fmax(worst, std::fabs(double(a[i]) - b[i]));
        if (worst > tolerance)
            std::printf("    %s: off by %g (allowed %g)\n", what, worst, tolerance);
        return worst <= tolerance;
    }

    // The first `count` samples of both.
    bool ClosePrefix(const Array<float>& a, const Array<float>& b, usize count, double tolerance, const char* what)
    {
        if (a.Size() < count || b.Size() < count)
        {
            std::printf("    %s: %zu and %zu samples, %zu compared\n", what, a.Size(), b.Size(), count);
            return false;
        }
        Array<float> x, y;
        x.Append(a.Data(), count);
        y.Append(b.Data(), count);
        return Close(x, y, tolerance, what);
    }

    const Array<float>& Signal()
    {
        static Array<float> s = [] {
            AudioData d;
            Decode(Bytes(sounds::kWavFloat32), d);
            return d.samples;
        }();
        return s;
    }
}

HX_TEST(Decoding, WavSampleFormats)
{
    AudioData d;
    HX_REQUIRE(Decode(Bytes(sounds::kWavFloat32), d));
    HX_CHECK(d.format.sampleRate == 44100u && d.format.channels == 1u && d.format.frames == 4410u && !d.loop.present);
    // A 440 Hz sine at the generator's amplitude of 1/8.
    HX_CHECK(std::fabs(d.samples[25] - 0.125 * std::sin(2 * 3.14159265358979 * 440 * 25 / 44100.0)) < 1e-4);
    struct Case
    {
        Span<const u8> bytes;
        double tolerance;
        const char* name;
    };
    const Case cases[] = {
        {Bytes(sounds::kWavFloat64), 1e-7, "float64"},  {Bytes(sounds::kWavU8), 1.01 / 128, "u8"},  {Bytes(sounds::kWavS16), 1.01 / 32768, "s16"},
        {Bytes(sounds::kWavS24), 1.01 / 8388608, "s24"}, {Bytes(sounds::kWavS32), 1e-7, "s32"},     {Bytes(sounds::kWavRf64), 1.01 / 32768, "RF64"},
    };
    for (const Case& c : cases)
    {
        AudioData x;
        HX_CHECK(Decode(c.bytes, x) && x.format.frames == 4410u && Close(x.samples, Signal(), c.tolerance, c.name));
    }
    // ffmpeg's Wave64 data chunk counts the padding to 8 bytes as data: two more frames, silent.
    AudioData w64;
    HX_CHECK(Decode(Bytes(sounds::kW64), w64) && w64.format.frames == 4412u && ClosePrefix(w64.samples, Signal(), 4410, 1.01 / 32768, "W64"));
    HX_CHECK(w64.samples[4410] == 0 && w64.samples[4411] == 0);
}

HX_TEST(Decoding, CompandedAndAdpcmWav)
{
    struct Case
    {
        Span<const u8> bytes;
        Array<float> reference;
        const char* name;
        usize samples;
    };
    // The IMA file has no fact chunk, so its last block counts whole (6123 frames); the MS file's fact chunk says 4410,
    // where the reference decoder gives the whole last block.
    Case cases[] = {
        {Bytes(sounds::kWavALaw), Reference(sounds::kWavALawReference), "A-law", 4410},
        {Bytes(sounds::kWavMuLaw), Reference(sounds::kWavMuLawReference), "mu-law", 4410},
        {Bytes(sounds::kWavImaAdpcm), Reference(sounds::kWavImaAdpcmReference), "IMA ADPCM", 6123},
        {Bytes(sounds::kWavMsAdpcm), Reference(sounds::kWavMsAdpcmReference), "MS ADPCM", 4410},
        {Bytes(sounds::kWavStereoS16), Reference(sounds::kWavStereoS16Reference), "stereo", 8820},
    };
    for (Case& c : cases)
    {
        AudioData d;
        HX_CHECK(Decode(c.bytes, d));
        usize expected = c.samples;
        HX_CHECK(d.samples.Size() == expected && ClosePrefix(d.samples, c.reference, expected, 1e-6, c.name));
        if (d.samples.Size() != expected)
            std::printf("    %s: %zu samples, the reference %zu\n", c.name, d.samples.Size(), c.reference.Size());
    }
}

HX_TEST(Decoding, Flac)
{
    AudioData d;
    HX_REQUIRE(Decode(Bytes(sounds::kFlac16), d));
    HX_CHECK(d.format.channels == 2u && d.format.frames == 4410u && Close(d.samples, Reference(sounds::kFlac16Reference), 1e-7, "FLAC 16"));
    HX_REQUIRE(Decode(Bytes(sounds::kFlac24), d));
    HX_CHECK(Close(d.samples, Signal(), 1.01 / 8388608, "FLAC 24"));
    HX_REQUIRE(Decode(Bytes(sounds::kFlac51), d));
    HX_CHECK(d.format.channels == 6u && d.format.sampleRate == 48000u && Close(d.samples, Reference(sounds::kFlac51Reference), 1e-7, "FLAC 5.1"));
}

HX_TEST(Decoding, Mp3)
{
    AudioData d;
    HX_REQUIRE(Decode(Bytes(sounds::kMp3), d));
    HX_CHECK(d.format.channels == 2u && d.format.sampleRate == 44100u);
    // The encoder delay and padding are left out, as the reference decoder does.
    HX_CHECK(Close(d.samples, Reference(sounds::kMp3Reference), 2e-4, "MP3"));
    HX_REQUIRE(Decode(Bytes(sounds::kMp3Mono22k), d));
    HX_CHECK(d.format.channels == 1u && d.format.sampleRate == 22050u && Close(d.samples, Reference(sounds::kMp3Mono22kReference), 2e-4, "MP3 22 kHz"));
}

HX_TEST(Decoding, OggVorbis)
{
    AudioData d;
    HX_REQUIRE(Decode(Bytes(sounds::kOgg), d));
    // The file's last granule says 4410 frames; the reference decoder stops 128 frames early.
    Array<float> reference = Reference(sounds::kOggReference);
    HX_CHECK(d.format.channels == 2u && d.format.sampleRate == 44100u && d.format.frames == 4410u &&
             ClosePrefix(d.samples, reference, reference.Size(), 2e-4, "Vorbis"));
    HX_CHECK(d.loop.present && d.loop.start == 1000u && d.loop.length == 2000u);
    // 5.1: Vorbis orders the channels front left, center, front right, back pair, LFE; the decoder gives the WAV order.
    HX_REQUIRE(Decode(Bytes(sounds::kOgg51), d));
    Array<float> surround = Reference(sounds::kOgg51Reference);
    HX_CHECK(d.format.channels == 6u && d.format.sampleRate == 48000u && ClosePrefix(d.samples, surround, surround.Size(), 2e-4, "Vorbis 5.1"));
}

HX_TEST(Decoding, SeekingAndChunks)
{
    const Span<const u8> files[] = {Bytes(sounds::kWavS16), Bytes(sounds::kWavImaAdpcm), Bytes(sounds::kWavMsAdpcm), Bytes(sounds::kFlac16),
                                    Bytes(sounds::kMp3), Bytes(sounds::kOgg)};
    for (Span<const u8> file : files)
    {
        AudioData whole;
        HX_REQUIRE(Decode(file, whole));
        u32 c = whole.format.channels;
        String error;
        // A seek lands where reading from the start gets.
        std::unique_ptr<AudioStream> s = OpenAudio(file, error);
        HX_REQUIRE(s != nullptr);
        for (u64 at : {1234ull, 17ull, 3000ull, 0ull})
        {
            HX_REQUIRE(s->Seek(at) && s->Position() == at);
            Array<float> part;
            part.Resize(500 * c);
            u64 n = s->Read(part.Data(), 500);
            bool same = n == 500;
            for (u64 i = 0; i < n * c && same; i++)
                same = std::fabs(part[i] - whole.samples[at * c + i]) < 1e-5f;
            HX_CHECK(same);
            if (!same)
                std::printf("    %s: seek to %llu read %llu frames that differ\n", OpenAudio(file, error) ? "" : "?", (unsigned long long)at, (unsigned long long)n);
        }
        // Chunks of odd sizes read the same samples.
        s = OpenAudio(file, error);
        Array<float> chunked;
        u64 sizes[] = {1, 7, 333, 64, 1000};
        for (u32 k = 0;; k++)
        {
            usize at = chunked.Size();
            u64 want = sizes[k % 5];
            chunked.Resize(at + usize(want) * c);
            u64 n = s->Read(chunked.Data() + at, want);
            chunked.Resize(at + usize(n) * c);
            if (n == 0)
                break;
        }
        HX_CHECK(Close(chunked, whole.samples, 0, "chunks") && s->Position() == whole.format.frames && !s->Failed());
        HX_CHECK(!s->Seek(whole.format.frames + 1) && s->Failed());
    }
}

HX_TEST(Decoding, WavLoopPoints)
{
    // The 16 bit file with a smpl chunk of one loop from 100 to 299 added at its end.
    Array<u8> bytes;
    bytes.Append(sounds::kWavS16, sizeof(sounds::kWavS16));
    auto u32le = [&](u32 v) {
        for (int i = 0; i < 4; i++)
            bytes.Push(u8(v >> (8 * i)));
    };
    bytes.Append(reinterpret_cast<const u8*>("smpl"), 4);
    u32le(36 + 24);
    for (int i = 0; i < 7; i++)
        u32le(0);
    u32le(1);
    u32le(0);
    u32le(0);
    u32le(0);
    u32le(100);
    u32le(299);
    u32le(0);
    u32le(0);
    u32 riff = u32(bytes.Size() - 8);
    std::memcpy(bytes.Data() + 4, &riff, 4);
    AudioData d;
    HX_REQUIRE(Decode(Span<const u8>(bytes), d));
    HX_CHECK(d.loop.present && d.loop.start == 100u && d.loop.length == 200u && d.format.frames == 4410u);
}

HX_TEST(Decoding, RegistryAndDamage)
{
    AudioDecoder d;
    HX_CHECK(FindAudioDecoder(Bytes(sounds::kOgg), {}, d) && d.name == StringView("Ogg Vorbis"));
    HX_CHECK(FindAudioDecoder(Bytes(sounds::kFlac16), {}, d) && d.name == StringView("FLAC"));
    HX_CHECK(FindAudioDecoder(Bytes(sounds::kMp3), {}, d) && d.name == StringView("MP3"));
    HX_CHECK(FindAudioDecoder(Bytes(sounds::kW64), {}, d) && d.name == StringView("WAV"));
    const u8 junk[16] = {};
    HX_CHECK(!FindAudioDecoder(Span<const u8>(junk), {}, d) && FindAudioDecoder(Span<const u8>(junk), ".MP3", d) && d.name == StringView("MP3"));
    AudioData out;
    String error;
    HX_CHECK(!DecodeAudio(Span<const u8>(junk), out, error) && error == StringView("not a sound of a known format"));
    HX_CHECK(!DecodeAudio(Span<const u8>(junk), out, error, "flac") && error == StringView("not a FLAC stream dr_flac reads"));

    // A registered decoder replaces the one of its name.
    Array<AudioDecoder> before;
    AudioDecoders(before);
    AudioDecoder custom = before[0];
    custom.matches = [](Span<const u8> data) { return data.Size() == 3; };
    RegisterAudioDecoder(custom);
    const u8 three[3] = {1, 2, 3};
    HX_CHECK(FindAudioDecoder(Span<const u8>(three), {}, d) && d.name == before[0].name);
    RegisterAudioDecoder(before[0]);
    Array<AudioDecoder> after;
    AudioDecoders(after);
    HX_CHECK(after.Size() == before.Size());

    // Every cut of every file opens and reads, or fails; none reads outside.
    const Span<const u8> files[] = {Bytes(sounds::kWavMsAdpcm), Bytes(sounds::kFlac16), Bytes(sounds::kMp3), Bytes(sounds::kOgg)};
    for (Span<const u8> file : files)
        for (usize n = 0; n < file.Size(); n += 61)
        {
            AudioData cut;
            String e;
            DecodeAudio(Span<const u8>(file.Data(), n), cut, e);
        }
}
