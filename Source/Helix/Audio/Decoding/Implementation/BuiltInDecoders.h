#pragma once

#include <Helix/Audio/Decoding/AudioDecoder.h>

namespace helix::audio
{
    extern const AudioDecoder kWavDecoder;
    extern const AudioDecoder kFlacDecoder;
    extern const AudioDecoder kMp3Decoder;
    extern const AudioDecoder kVorbisDecoder;

    const AudioDecoder* const* BuiltInDecoders(usize& count);

    // Blocks the copied decoders allocate: sizes in front, larger than any clip refused.
    namespace third
    {
        void* Allocate(usize size);
        void* Reallocate(void* memory, usize size);
        void Free(void* memory);
    }

    // LOOPSTART / LOOPLENGTH (or LOOPEND) of a Vorbis comment "NAME=value", into `loop`.
    void ReadLoopComment(StringView comment, AudioLoop& loop, u64& loopEnd);
}
