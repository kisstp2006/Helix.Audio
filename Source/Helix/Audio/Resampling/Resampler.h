#pragma once

#include <Helix/Core/Containers/Array.h>

#include <memory>

namespace helix::audio
{
    enum class ResampleQuality : u8
    {
        // Straight lines between samples: no filter, for previews and pitch changes of short sounds.
        Linear,
        // Windowed sinc, 16 taps at the input rate (more when downsampling), Kaiser beta 6: aliases at about -70 dB.
        Medium,
        // 32 taps, Kaiser beta 8.6: aliases at about -90 dB.
        High,
    };

    // Converts interleaved float frames from one sample rate to another in chunks of any size, with the same output
    // as one call over the whole signal. Output frame k is the input at k * inRate / outRate exactly (rational
    // stepping), so the output has no delay; Flush gives the frames that wait for input beyond the end.
    class Resampler
    {
    public:
        bool Setup(u32 channels, u32 inRate, u32 outRate, ResampleQuality quality = ResampleQuality::Medium);
        void Reset();

        u32 Channels() const { return channels_; }
        u32 InRate() const { return inRate_; }
        u32 OutRate() const { return outRate_; }
        // Taps of the filter on each side of a sample, in input frames.
        u32 HalfTaps() const { return half_; }

        // Takes every input frame and writes up to `capacity` output frames; frames it had no room for wait for the
        // next call. Returns the frames written.
        u64 Process(const float* in, u64 frames, float* out, u64 capacity);
        // The rest after the last input, as if silence followed it.
        u64 Flush(float* out, u64 capacity);
        // The output frames of `inputFrames` input frames, Flush included.
        u64 OutputFrames(u64 inputFrames) const;

    private:
        u64 Produce(float* out, u64 capacity, bool flushing);

        u32 channels_ = 0, inRate_ = 0, outRate_ = 0;
        ResampleQuality quality_ = ResampleQuality::Medium;
        u32 half_ = 1;
        // The filter at kPhases + 1 fractional positions, 2 * half_ taps each; shared by the resamplers of one ratio and
        // quality, so setting one up again (a voice changing its pitch) costs no filter design.
        std::shared_ptr<const Array<float>> table_;
        // Input frames still needed: the window of the next output starts at frame index_ of it.
        Array<float> buffer_;
        // The next output's position: its window starts at buffer frame index_, and its center lies fraction_ / outRate_
        // past the window's center frame.
        u64 index_ = 0;
        u64 fraction_ = 0;
        // Input frames seen, and output frames given.
        u64 inputSeen_ = 0, outputGiven_ = 0;
    };

    // Up and down mixes between mono, stereo, quad, 5.1 and 7.1 in the channel order of the decoders (front left,
    // front right, center, LFE, back left, back right, side left, side right). Down mixes add center and surrounds at
    // -3 dB (0.7071) as ITU-R BS.775 does and drop the LFE; the result can exceed 1. Other counts keep their first
    // channels and fill the rest with silence. `in` and `out` must not overlap.
    void ConvertChannels(const float* in, u32 inChannels, float* out, u32 outChannels, u64 frames);
}
