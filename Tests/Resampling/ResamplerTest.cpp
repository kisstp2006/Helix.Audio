#include <Helix/Audio/Resampling/Resampler.h>
#include <Helix/Testing/Test.h>

#include <cmath>
#include <cstdio>

using namespace helix;
using namespace helix::audio;

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    Array<float> Sine(double frequency, u32 rate, u64 frames, u32 channels = 1)
    {
        Array<float> s;
        s.Resize(usize(frames) * channels);
        for (u64 i = 0; i < frames; i++)
            for (u32 c = 0; c < channels; c++)
                s[usize(i * channels + c)] = float(std::sin(2 * kPi * frequency * double(i) / rate + c));
        return s;
    }

    // Everything through one Process call and Flush.
    Array<float> Run(Resampler& r, const Array<float>& in)
    {
        u32 c = r.Channels();
        u64 frames = in.Size() / c;
        Array<float> out;
        out.Resize(usize(r.OutputFrames(frames)) * c);
        u64 n = r.Process(in.Data(), frames, out.Data(), r.OutputFrames(frames));
        n += r.Flush(out.Data() + n * c, r.OutputFrames(frames) - n);
        out.Resize(usize(n) * c);
        return out;
    }

    // RMS of a signal from frame `from` to `to`.
    double Rms(const Array<float>& s, usize from, usize to)
    {
        double sum = 0;
        for (usize i = from; i < to; i++)
            sum += double(s[i]) * s[i];
        return std::sqrt(sum / double(to - from));
    }
}

HX_TEST(Resampling, PassBandKeepsSinesInPlace)
{
    // 1 kHz from 44.1 to 48 kHz and back down to 32 kHz: the output is the same sine at the output rate, no delay.
    struct Case
    {
        u32 in, out;
        ResampleQuality q;
        double tolerance;
    };
    const Case cases[] = {
        {44100, 48000, ResampleQuality::Medium, 2e-3}, {44100, 48000, ResampleQuality::High, 2e-4}, {48000, 32000, ResampleQuality::Medium, 2e-3},
        {48000, 32000, ResampleQuality::High, 2e-4},   {44100, 48000, ResampleQuality::Linear, 2e-2}, {22050, 44100, ResampleQuality::High, 2e-4},
    };
    for (const Case& c : cases)
    {
        Resampler r;
        HX_REQUIRE(r.Setup(1, c.in, c.out, c.q));
        Array<float> out = Run(r, Sine(1000, c.in, c.in / 4));
        HX_CHECK(out.Size() == usize(r.OutputFrames(c.in / 4)));
        double worst = 0;
        // The edges see the silence before and after the signal.
        for (usize k = 200; k + 200 < out.Size(); k++)
            worst = std::fmax(worst, std::fabs(out[k] - std::sin(2 * kPi * 1000 * double(k) / c.out)));
        HX_CHECK(worst < c.tolerance);
        if (worst >= c.tolerance)
            std::printf("    %u -> %u quality %d: off by %g\n", c.in, c.out, int(c.q), worst);
    }
}

HX_TEST(Resampling, StopBandRemovesAliases)
{
    // 15 kHz is above the Nyquist frequency of 22.05 kHz: downsampling must remove it, not fold it to 7 kHz.
    for (ResampleQuality q : {ResampleQuality::Medium, ResampleQuality::High})
    {
        Resampler r;
        HX_REQUIRE(r.Setup(1, 48000, 22050, q));
        Array<float> out = Run(r, Sine(15000, 48000, 24000));
        double db = 20 * std::log10(Rms(out, 500, out.Size() - 500) / std::sqrt(0.5));
        double limit = q == ResampleQuality::Medium ? -55 : -80;
        HX_CHECK(db < limit);
        std::printf("    quality %d: a 15 kHz alias at %.1f dB\n", int(q), db);
    }
    // Linear filtering does not, and says so by being loud.
    Resampler l;
    HX_REQUIRE(l.Setup(1, 48000, 22050, ResampleQuality::Linear));
    Array<float> out = Run(l, Sine(15000, 48000, 24000));
    HX_CHECK(Rms(out, 500, out.Size() - 500) > 0.1);
}

HX_TEST(Resampling, ChunksGiveTheSameOutput)
{
    for (ResampleQuality q : {ResampleQuality::Linear, ResampleQuality::Medium, ResampleQuality::High})
    {
        Array<float> in = Sine(440, 44100, 20000, 2);
        Resampler whole;
        HX_REQUIRE(whole.Setup(2, 44100, 48000, q));
        Array<float> expected = Run(whole, in);

        Resampler r;
        HX_REQUIRE(r.Setup(2, 44100, 48000, q));
        Array<float> out;
        u64 sizes[] = {1, 17, 300, 4096, 2, 999};
        u64 at = 0;
        float buffer[2 * 600];
        for (u32 k = 0; at < 20000; k++)
        {
            u64 n = sizes[k % 6] < 20000 - at ? sizes[k % 6] : 20000 - at;
            // Small output room too: the rest waits for the next call.
            u64 got = r.Process(in.Data() + at * 2, n, buffer, 300);
            out.Append(buffer, usize(got) * 2);
            at += n;
            while ((got = r.Process(nullptr, 0, buffer, 600)) > 0)
                out.Append(buffer, usize(got) * 2);
        }
        u64 got;
        while ((got = r.Flush(buffer, 600)) > 0)
            out.Append(buffer, usize(got) * 2);
        bool same = out.Size() == expected.Size();
        for (usize i = 0; i < out.Size() && same; i++)
            same = out[i] == expected[i];
        HX_CHECK(same);
        if (!same)
            std::printf("    quality %d: %zu samples, %zu in one call\n", int(q), out.Size(), expected.Size());
    }
}

HX_TEST(Resampling, SameRateAndSetup)
{
    Resampler r;
    HX_REQUIRE(r.Setup(2, 48000, 48000, ResampleQuality::High));
    Array<float> in = Sine(100, 48000, 1000, 2);
    Array<float> out = Run(r, in);
    bool same = out.Size() == in.Size();
    for (usize i = 0; i < out.Size() && same; i++)
        same = out[i] == in[i];
    HX_CHECK(same);
    // Ratios reduce: 88200 / 96000 is 147 / 160.
    HX_CHECK(r.Setup(1, 88200, 96000) && r.InRate() == 147u && r.OutRate() == 160u && r.HalfTaps() == 8u);
    HX_CHECK(r.Setup(1, 96000, 8000, ResampleQuality::High) && r.HalfTaps() == 192u);
    HX_CHECK(!r.Setup(0, 44100, 48000) && !r.Setup(1, 0, 48000) && !r.Setup(1, 1000000, 1));
    HX_CHECK(r.OutputFrames(100) == 0u);
}

HX_TEST(Resampling, ChannelConversion)
{
    const float stereo[] = {1, -1, 0.5f, 0.25f};
    float mono[2];
    ConvertChannels(stereo, 2, mono, 1, 2);
    HX_CHECK(mono[0] == 0 && mono[1] == 0.375f);
    float up[4];
    const float one[] = {0.5f, -0.5f};
    ConvertChannels(one, 1, up, 2, 2);
    HX_CHECK(up[0] == 0.5f && up[1] == 0.5f && up[2] == -0.5f && up[3] == -0.5f);
    // 5.1 to stereo: L + 0.707 C + 0.707 BL, the LFE left out.
    const float six[] = {1, 2, 3, 4, 5, 6};
    float two[2];
    ConvertChannels(six, 6, two, 2, 1);
    HX_CHECK(std::fabs(two[0] - (1 + 0.70710678f * 3 + 0.70710678f * 5)) < 1e-5f && std::fabs(two[1] - (2 + 0.70710678f * 3 + 0.70710678f * 6)) < 1e-5f);
    // 7.1 to 5.1: the sides fold into the backs.
    const float eight[] = {1, 2, 3, 4, 5, 6, 7, 8};
    float five[6];
    ConvertChannels(eight, 8, five, 6, 1);
    HX_CHECK(five[0] == 1 && five[2] == 3 && five[3] == 4 && std::fabs(five[4] - (5 + 0.70710678f * 7)) < 1e-5f && std::fabs(five[5] - (6 + 0.70710678f * 8)) < 1e-5f);
    // Stereo to 7.1 fills the front pair only; 3 channels to 2 keep the first two.
    float wide[8];
    ConvertChannels(stereo, 2, wide, 8, 1);
    HX_CHECK(wide[0] == 1 && wide[1] == -1 && wide[2] == 0 && wide[7] == 0);
    const float three[] = {1, 2, 3};
    float first[2];
    ConvertChannels(three, 3, first, 2, 1);
    HX_CHECK(first[0] == 1 && first[1] == 2);
}
