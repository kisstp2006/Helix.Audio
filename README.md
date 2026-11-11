# Helix.Audio

Audio decoding and resampling of the Helix engine: WAV, FLAC, MP3 and Ogg Vorbis behind one streaming API, a
resampler for any rate ratio, and channel up and down mixes. The decoders are the public domain dr_wav, dr_flac,
dr_mp3 and stb_vorbis (see `THIRD-PARTY.txt`); the API, the registry, the resampler and the channel mixes are own code.

## Contents

| Path | Content |
|---|---|
| `Source/Helix/Audio/Decoding/AudioDecoder.h` | `AudioStream`, `AudioDecoder` and its registry, `OpenAudio`, `DecodeAudio` |
| `Source/Helix/Audio/Resampling/Resampler.h` | `Resampler` (linear, windowed sinc at two qualities), `ConvertChannels` |
| `Tests/`, `Benchmarks/`, `Fuzz/` | Every format against a reference decode, seeking, chunks, loop points, resampler pass and stop band |
| `Tools/GenerateTestSounds.py` | Makes the test sounds and reference decodes with ffmpeg into `Tests/Generated/TestSounds.h` |
| `ThirdParty/` | dr_wav, dr_flac, dr_mp3 unchanged; stb_vorbis with three fixes listed in `THIRD-PARTY.txt` |

## Decoding

```cpp
using namespace helix::audio;

helix::String error;
std::unique_ptr<AudioStream> stream = OpenAudio(fileBytes, error, ".ogg");  // the bytes must outlive the stream
const AudioFormat& f = stream->Format();     // sample rate, channels, frames (0 when unknown)
float chunk[1024 * kMaxChannels];
while (u64 n = stream->Read(chunk, 1024))   // interleaved float frames
    Play(chunk, n);
stream->Seek(48000);                         // to a frame from the start
```

- Formats are found by their signature, and by the extension where there is none; `RegisterAudioDecoder` adds or
  replaces one.
- WAV: RIFF, RF64 and Wave64; PCM 8, 16, 24 and 32 bit, float 32 and 64, A-law, mu-law, IMA and MS ADPCM; the first
  loop of a `smpl` chunk.
- FLAC: every bit depth and channel layout, seek tables, LOOPSTART / LOOPLENGTH (or LOOPEND) comments.
- MP3: MPEG 1, 2 and 2.5 layer III, without the encoder delay and padding the LAME tag gives.
- Ogg Vorbis: one logical stream (chained and multiplexed files are refused), loop comments as for FLAC; the decoder
  works in a buffer of ours instead of the C heap.
- Channels come in one order for every format: front left, front right, center, LFE, back left, back right, side
  left, side right (Vorbis files are reordered to it).
- `DecodeAudio` decodes a whole clip into `AudioData`. Damaged data is an error, never a read outside the bytes.

## Resampling

```cpp
Resampler r;
r.Setup(2, 44100, 48000, ResampleQuality::High);
u64 written = r.Process(in, inFrames, out, capacity);   // any chunk sizes, the same output as one call
written += r.Flush(out + written * 2, capacity - written);
```

Output frame k is the input at k * inRate / outRate exactly (rational stepping, no drift and no delay). Linear
interpolates; Medium and High are Kaiser windowed sinc filters (16 and 32 taps, wider when downsampling) whose aliases
measure about -70 and -90 dB; the filter tables are shared by resamplers of one ratio and quality.

`ConvertChannels` mixes between mono, stereo, quad, 5.1 and 7.1: down mixes add the center and surrounds at -3 dB as
ITU-R BS.775 does and drop the LFE; mono plays on the front pair.

## Speed

Release, one thread, stereo at 44.1 kHz, in times real time: WAV about 18000, IMA ADPCM 1900, FLAC 1600, MP3 900,
Vorbis 200; resampling to 48 kHz 2200 (linear), 800 (Medium), 540 (High).

## Building

The repository builds with the presets of Helix.CMake (Windows, Web, Android); the submodules in `External/` are
Helix.CMake, Helix.Testing, Helix.Core and Helix.Platform.

## License

GPL-3.0, see `LICENSE`. The copies in `ThirdParty/` are public domain, see `THIRD-PARTY.txt`.
