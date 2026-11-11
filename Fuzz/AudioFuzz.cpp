#include <Generated/TestSounds.h>

#include <Helix/Audio/Decoding/AudioDecoder.h>
#include <Helix/Audio/Resampling/Resampler.h>
#include <Helix/Testing/Fuzz.h>

#include <cstdlib>

using namespace helix;
using namespace helix::audio;
namespace sounds = helix::audio::testing;

namespace
{
    // A mutated copy of a test sound, opened, read in chunks, sought and read again.
    u64 Exercise(helix::testing::FuzzRound& round, const u8* seed, usize size)
    {
        Array<u8> bytes;
        bytes.Append(seed, size);
        bytes.Resize(size + 64);
        bytes.Resize(round.Mutate(bytes.Data(), size, bytes.Size()));
        String error;
        round.Step("open");
        std::unique_ptr<AudioStream> s = OpenAudio(Span<const u8>(bytes), error);
        if (!s)
            return 0;
        u32 c = s->Format().channels;
        Array<float> buffer;
        buffer.Resize(usize(1024) * c);
        round.Step("read");
        u64 total = 0;
        for (int i = 0; i < 64; i++)
        {
            u64 n = s->Read(buffer.Data(), 1 + round.Below(1024));
            total += n;
            if (n == 0)
                break;
        }
        round.Step("seek");
        u64 frames = s->Format().frames;
        if (s->Seek(frames ? round.Below(u32(frames < 0xFFFFFFFFu ? frames : 0xFFFFFFFFu)) : 0))
            s->Read(buffer.Data(), 1024);
        return total + 1;
    }
}

HX_FUZZ(Wav)
{
    switch (round.Below(4))
    {
    case 0: return Exercise(round, sounds::kWavS24, sizeof(sounds::kWavS24));
    case 1: return Exercise(round, sounds::kWavImaAdpcm, sizeof(sounds::kWavImaAdpcm));
    case 2: return Exercise(round, sounds::kWavMsAdpcm, sizeof(sounds::kWavMsAdpcm));
    default: return Exercise(round, sounds::kW64, sizeof(sounds::kW64));
    }
}

HX_FUZZ(Flac)
{
    return round.Below(2) ? Exercise(round, sounds::kFlac16, sizeof(sounds::kFlac16)) : Exercise(round, sounds::kFlac51, sizeof(sounds::kFlac51));
}

HX_FUZZ(Mp3)
{
    return round.Below(2) ? Exercise(round, sounds::kMp3, sizeof(sounds::kMp3)) : Exercise(round, sounds::kMp3Mono22k, sizeof(sounds::kMp3Mono22k));
}

HX_FUZZ(Vorbis)
{
    return round.Below(2) ? Exercise(round, sounds::kOgg, sizeof(sounds::kOgg)) : Exercise(round, sounds::kOgg51, sizeof(sounds::kOgg51));
}

// Random rates, qualities, channel counts and chunk sizes: the output count is what OutputFrames says.
HX_FUZZ(Resampler)
{
    // Odd rates only with linear filtering, whose setup costs nothing: the sinc tables of extreme ratios are large.
    static constexpr u32 kRates[] = {8000, 11025, 22050, 32000, 44100, 48000, 88200, 96000, 7, 1};
    ResampleQuality quality = ResampleQuality(round.Below(3));
    u32 choices = quality == ResampleQuality::Linear ? 10 : 8;
    u32 in = kRates[round.Below(choices)], out = kRates[round.Below(choices)];
    u32 channels = 1 + round.Below(8);
    Resampler r;
    round.Step("setup");
    if (!r.Setup(channels, in, out, quality))
        return 0;
    Array<float> input, output;
    u64 frames = round.Below(1500);
    // Ratios like 1 Hz to 96 kHz make millions of frames of each one; the output stays within a megabyte or two.
    while (frames > 0 && r.OutputFrames(frames) * channels > (1u << 18))
        frames /= 2;
    input.Resize(usize(frames) * channels);
    for (float& v : input)
        v = float(i32(round.Below(2001)) - 1000) / 1000.0f;
    output.Resize(usize(r.OutputFrames(frames) + 1) * channels);
    round.Step("process");
    u64 at = 0, written = 0;
    while (at < frames)
    {
        u64 n = 1 + round.Below(700);
        n = n < frames - at ? n : frames - at;
        written += r.Process(input.Data() + at * channels, n, output.Data() + written * channels, r.OutputFrames(frames) - written);
        at += n;
    }
    written += r.Flush(output.Data() + written * channels, r.OutputFrames(frames) - written);
    if (written != r.OutputFrames(frames))
        std::abort();
    float mixed[8 * 4];
    ConvertChannels(input.Data(), channels, mixed, 1 + round.Below(8), frames < 4 ? frames : 4);
    return 1;
}
