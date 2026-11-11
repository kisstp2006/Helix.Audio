#include <Helix/Audio/Resampling/Resampler.h>

#include <Helix/Platform/Threading/Thread.h>

#include <cmath>
#include <cstring>

namespace helix::audio
{
    namespace
    {
        constexpr u32 kPhases = 256;
        constexpr u32 kMaxHalf = 512;
        constexpr double kPi = 3.14159265358979323846;

        // The zeroth order modified Bessel function of the first kind, by its series.
        double BesselI0(double x)
        {
            double sum = 1, term = 1;
            for (int k = 1; k < 64; k++)
            {
                term *= (x / (2 * k)) * (x / (2 * k));
                sum += term;
                if (term < sum * 1e-17)
                    break;
            }
            return sum;
        }

        double Kaiser(double x, double beta)
        {
            if (x <= -1 || x >= 1)
                return 0;
            return BesselI0(beta * std::sqrt(1 - x * x)) / BesselI0(beta);
        }

        double Sinc(double x) { return x == 0 ? 1 : std::sin(kPi * x) / (kPi * x); }

        std::shared_ptr<const Array<float>> Design(u32 half, double scale, ResampleQuality quality)
        {
            double cutoff = scale * (quality == ResampleQuality::Medium ? 0.90 : 0.94);
            double beta = quality == ResampleQuality::Medium ? 6.0 : 8.6;
            u32 taps = 2 * half;
            auto table = std::make_shared<Array<float>>();
            table->Resize(usize(kPhases + 1) * taps);
            for (u32 p = 0; p <= kPhases; p++)
            {
                double frac = double(p) / kPhases;
                double sum = 0;
                float* row = table->Data() + usize(p) * taps;
                for (u32 j = 0; j < taps; j++)
                {
                    double x = double(i32(j) - i32(half) + 1) - frac;
                    double h = cutoff * Sinc(cutoff * x) * Kaiser(x / half, beta);
                    row[j] = float(h);
                    sum += h;
                }
                // Unit gain at DC for every phase.
                for (u32 j = 0; j < taps; j++)
                    row[j] = float(row[j] / sum);
            }
            return table;
        }

        // The last tables designed, by reduced ratio and quality.
        struct Cache
        {
            struct Entry
            {
                u32 in, out;
                ResampleQuality quality;
                std::shared_ptr<const Array<float>> table;
            };
            platform::Mutex mutex;
            Array<Entry> entries;
        };

        std::shared_ptr<const Array<float>> FilterTable(u32 in, u32 out, ResampleQuality quality, u32 half, double scale)
        {
            static Cache cache;
            {
                platform::ScopedLock lock(cache.mutex);
                for (const Cache::Entry& e : cache.entries)
                    if (e.in == in && e.out == out && e.quality == quality)
                        return e.table;
            }
            std::shared_ptr<const Array<float>> table = Design(half, scale, quality);
            platform::ScopedLock lock(cache.mutex);
            if (cache.entries.Size() >= 16)
                cache.entries.RemoveAt(0);
            cache.entries.Push({in, out, quality, table});
            return table;
        }
    }

    bool Resampler::Setup(u32 channels, u32 inRate, u32 outRate, ResampleQuality quality)
    {
        *this = Resampler();
        if (channels == 0 || channels > 64 || inRate == 0 || outRate == 0)
            return false;
        // The same ratio in smaller numbers keeps the stepping exact and the numbers small.
        u32 a = inRate, b = outRate;
        while (b)
        {
            u32 t = a % b;
            a = b;
            b = t;
        }
        channels_ = channels;
        inRate_ = inRate / a;
        outRate_ = outRate / a;
        quality_ = quality;
        if (quality == ResampleQuality::Linear || inRate_ == outRate_)
            half_ = 1;
        else
        {
            double scale = outRate_ < inRate_ ? double(outRate_) / inRate_ : 1.0;
            u32 base = quality == ResampleQuality::Medium ? 8 : 16;
            double width = std::ceil(base / scale);
            if (width > kMaxHalf)
            {
                *this = Resampler();
                return false;
            }
            half_ = u32(width);
            table_ = FilterTable(inRate_, outRate_, quality, half_, scale);
        }
        Reset();
        return true;
    }

    void Resampler::Reset()
    {
        buffer_.Clear();
        // History before the first frame, so the first output is centered on it.
        buffer_.Resize(usize(half_ - 1) * channels_, 0.0f);
        index_ = 0;
        fraction_ = 0;
        inputSeen_ = 0;
        outputGiven_ = 0;
    }

    u64 Resampler::OutputFrames(u64 inputFrames) const
    {
        if (inRate_ == 0)
            return 0;
        return (inputFrames * outRate_ + inRate_ - 1) / inRate_;
    }

    u64 Resampler::Produce(float* out, u64 capacity, bool flushing)
    {
        u32 c = channels_;
        u32 taps = 2 * half_;
        u64 total = OutputFrames(inputSeen_);
        u64 written = 0;
        while (written < capacity && outputGiven_ < total)
        {
            usize available = buffer_.Size() / c;
            if (index_ + taps > available)
            {
                if (!flushing)
                    break;
                buffer_.Resize((index_ + taps) * c, 0.0f);
            }
            const float* x = buffer_.Data() + index_ * c;
            float* y = out + written * c;
            if (inRate_ == outRate_)
                std::memcpy(y, x + usize(half_ - 1) * c, c * sizeof(float));
            else if (half_ == 1)
            {
                float f = float(double(fraction_) / outRate_);
                for (u32 ch = 0; ch < c; ch++)
                    y[ch] = x[ch] + (x[c + ch] - x[ch]) * f;
            }
            else
            {
                double phase = double(fraction_) / outRate_ * kPhases;
                u32 p = u32(phase);
                float w = float(phase - p);
                const float* h0 = table_->Data() + usize(p) * taps;
                const float* h1 = h0 + taps;
                // The taps between the two phases first, then one dot product per channel.
                float h[2 * kMaxHalf];
                for (u32 j = 0; j < taps; j++)
                    h[j] = h0[j] + (h1[j] - h0[j]) * w;
                if (c == 1)
                {
                    float s = 0;
                    for (u32 j = 0; j < taps; j++)
                        s += x[j] * h[j];
                    y[0] = s;
                }
                else if (c == 2)
                {
                    float s0 = 0, s1 = 0;
                    for (u32 j = 0; j < taps; j++)
                    {
                        s0 += x[2 * j] * h[j];
                        s1 += x[2 * j + 1] * h[j];
                    }
                    y[0] = s0;
                    y[1] = s1;
                }
                else
                {
                    for (u32 ch = 0; ch < c; ch++)
                        y[ch] = 0;
                    for (u32 j = 0; j < taps; j++)
                    {
                        const float* xj = x + usize(j) * c;
                        for (u32 ch = 0; ch < c; ch++)
                            y[ch] += xj[ch] * h[j];
                    }
                }
            }
            written++;
            outputGiven_++;
            fraction_ += inRate_;
            index_ += fraction_ / outRate_;
            fraction_ %= outRate_;
        }
        // Frames before the next window are not needed again.
        usize drop = index_ < buffer_.Size() / c ? usize(index_) : buffer_.Size() / c;
        if (drop > 0)
        {
            std::memmove(buffer_.Data(), buffer_.Data() + drop * c, (buffer_.Size() - drop * c) * sizeof(float));
            buffer_.Resize(buffer_.Size() - drop * c);
            index_ -= drop;
        }
        return written;
    }

    u64 Resampler::Process(const float* in, u64 frames, float* out, u64 capacity)
    {
        if (channels_ == 0)
            return 0;
        buffer_.Append(in, usize(frames) * channels_);
        inputSeen_ += frames;
        return Produce(out, capacity, false);
    }

    u64 Resampler::Flush(float* out, u64 capacity)
    {
        if (channels_ == 0)
            return 0;
        return Produce(out, capacity, true);
    }

    namespace
    {
        constexpr float kHalfPower = 0.70710678f;
        enum : u32
        {
            FL,
            FR,
            FC,
            LFE,
            BL,
            BR,
            SL,
            SR
        };

        bool Known(u32 n) { return n == 1 || n == 2 || n == 4 || n == 6 || n == 8; }

        // The position of a speaker in a layout of n channels, ~0u when the layout has none.
        u32 Slot(u32 n, u32 speaker)
        {
            switch (n)
            {
            case 2: return speaker <= FR ? speaker : ~0u;
            // Quad: front pair, back pair.
            case 4: return speaker <= FR ? speaker : speaker == BL ? 2 : speaker == BR ? 3 : ~0u;
            case 6: return speaker <= BR ? speaker : ~0u;
            case 8: return speaker;
            default: return ~0u;
            }
        }

        // m[out][in] for known layouts.
        void Matrix(u32 in, u32 out, float m[8][8])
        {
            std::memset(m, 0, sizeof(float) * 64);
            if (in == 1)
            {
                // Mono plays on the front pair.
                for (u32 o = 0; o < out && o < 2; o++)
                    m[o][0] = 1;
                return;
            }
            if (out == 1)
            {
                // Half of the stereo down mix of each side.
                float s[8][8];
                Matrix(in, 2, s);
                for (u32 i = 0; i < in; i++)
                    m[0][i] = 0.5f * (s[0][i] + s[1][i]);
                return;
            }
            // Every input speaker to the output speaker of the same name, or folded into a neighbour at -3 dB.
            for (u32 speaker = FL; speaker <= SR; speaker++)
            {
                u32 i = Slot(in, speaker);
                if (i == ~0u)
                    continue;
                u32 o = Slot(out, speaker);
                if (o != ~0u)
                {
                    m[o][i] += 1;
                    continue;
                }
                switch (speaker)
                {
                case FC:
                    m[Slot(out, FL)][i] += kHalfPower;
                    m[Slot(out, FR)][i] += kHalfPower;
                    break;
                case LFE: break;
                case BL:
                case SL:
                {
                    u32 to = Slot(out, BL) != ~0u ? Slot(out, BL) : Slot(out, FL);
                    m[to][i] += kHalfPower;
                    break;
                }
                case BR:
                case SR:
                {
                    u32 to = Slot(out, BR) != ~0u ? Slot(out, BR) : Slot(out, FR);
                    m[to][i] += kHalfPower;
                    break;
                }
                default: break;
                }
            }
        }
    }

    void ConvertChannels(const float* in, u32 inChannels, float* out, u32 outChannels, u64 frames)
    {
        if (inChannels == outChannels)
        {
            std::memcpy(out, in, usize(frames) * inChannels * sizeof(float));
            return;
        }
        if (!Known(inChannels) || !Known(outChannels))
        {
            for (u64 f = 0; f < frames; f++)
                for (u32 o = 0; o < outChannels; o++)
                    out[f * outChannels + o] = o < inChannels ? in[f * inChannels + o] : 0.0f;
            return;
        }
        float m[8][8];
        Matrix(inChannels, outChannels, m);
        for (u64 f = 0; f < frames; f++)
        {
            const float* x = in + f * inChannels;
            float* y = out + f * outChannels;
            for (u32 o = 0; o < outChannels; o++)
            {
                float v = 0;
                for (u32 i = 0; i < inChannels; i++)
                    v += m[o][i] * x[i];
                y[o] = v;
            }
        }
    }
}
