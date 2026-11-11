#include <Generated/TestSounds.h>

#include <Helix/Audio/Decoding/AudioDecoder.h>
#include <Helix/Audio/Resampling/Resampler.h>
#include <Helix/Testing/Bench.h>

#include <cmath>

using namespace helix;
using namespace helix::audio;
namespace sounds = helix::audio::testing;

namespace
{
    // Decodes a 0.1 s test sound whole; items are frames, so ns per item over 22.7 us (44.1 kHz) is the share of real time.
    void Decode(helix::testing::BenchState& state, const u8* data, usize size)
    {
        AudioData d;
        String error;
        DecodeAudio(Span<const u8>(data, size), d, error);
        state.SetItemsPerIteration(d.format.frames);
        while (state.KeepRunning())
        {
            DecodeAudio(Span<const u8>(data, size), d, error);
            helix::testing::DoNotOptimize(d.samples.Data());
        }
    }

    void Resample(helix::testing::BenchState& state, ResampleQuality q)
    {
        Array<float> in, out;
        in.Resize(2 * 44100);
        for (usize i = 0; i < in.Size(); i++)
            in[i] = float(std::sin(double(i) * 0.03));
        Resampler r;
        r.Setup(2, 44100, 48000, q);
        out.Resize(usize(r.OutputFrames(44100)) * 2);
        state.SetItemsPerIteration(44100);
        while (state.KeepRunning())
        {
            r.Reset();
            r.Process(in.Data(), 44100, out.Data(), r.OutputFrames(44100));
            helix::testing::DoNotOptimize(out.Data());
        }
    }
}

HX_BENCH(DecodeWav16) { Decode(state, sounds::kWavStereoS16, sizeof(sounds::kWavStereoS16)); }
HX_BENCH(DecodeImaAdpcm) { Decode(state, sounds::kWavImaAdpcm, sizeof(sounds::kWavImaAdpcm)); }
HX_BENCH(DecodeFlacStereo) { Decode(state, sounds::kFlac16, sizeof(sounds::kFlac16)); }
HX_BENCH(DecodeMp3Stereo) { Decode(state, sounds::kMp3, sizeof(sounds::kMp3)); }
HX_BENCH(DecodeVorbisStereo) { Decode(state, sounds::kOgg, sizeof(sounds::kOgg)); }
HX_BENCH(ResampleStereoLinear) { Resample(state, ResampleQuality::Linear); }
HX_BENCH(ResampleStereoMedium) { Resample(state, ResampleQuality::Medium); }
HX_BENCH(ResampleStereoHigh) { Resample(state, ResampleQuality::High); }
