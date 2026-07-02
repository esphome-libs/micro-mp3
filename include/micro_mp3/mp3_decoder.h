// Copyright 2026 Kevin Ahrendt
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/// @file
/// @brief MP3 streaming decoder wrapper

#pragma once

#include <stddef.h>
#include <stdint.h>

// Forward declaration for private method signatures (defined by OpenCore decoder)
struct tPVMP3DecoderExternal;  // NOLINT(readability-identifier-naming)

namespace micro_mp3 {

/// @brief Result codes for Mp3Decoder operations
///
/// Error Checking Pattern:
///       - Use `result < 0` to check for errors
///       - Use `result >= 0` to check for non-error (success or informational)
///       - Use `samples_decoded > 0` to check if samples were decoded
///
/// Informational codes: Positive values (> 0), not errors, safe to continue
/// Success code: MP3_OK (0)
/// Error codes: All negative values (< 0)
///
/// Three negative codes are recoverable and should be handled before a generic
/// `result < 0` bail: MP3_DECODE_ERROR (skip the bad frame and continue),
/// MP3_STREAM_INFO_CHANGED (reconfigure from the accessors and continue), and
/// MP3_OUTPUT_BUFFER_TOO_SMALL (enlarge the output buffer and continue).
enum Mp3Result : int8_t {
    // Success / informational (>= 0)
    MP3_OK = 0,                 // Success (check samples_decoded output parameter)
    MP3_NEED_MORE_DATA = 1,     // Incomplete frame, feed more bytes and call decode() again
    MP3_STREAM_INFO_READY = 2,  // Stream format parsed from frame header; samples_decoded is 0
                                // Advance by bytes_consumed and call decode() again for PCM

    // Errors (< 0)
    MP3_INPUT_INVALID = -1,            // Invalid input (nullptr or bad data)
    MP3_ALLOCATION_FAILED = -2,        // Memory allocation failed
    MP3_OUTPUT_BUFFER_TOO_SMALL = -3,  // Output buffer too small for the decoded frame
    MP3_DECODE_ERROR = -4,             // MP3 decode failed (corrupted/invalid frame); recoverable
    MP3_STREAM_INFO_CHANGED = -5       // Stream format (sample rate, channels, or MPEG version)
                                       // changed mid-stream
};

/// @brief MPEG version identifiers
///
/// Values match the OpenCore internal representation.
enum Mp3Version : uint8_t {
    MP3_MPEG1 = 0,    // MPEG 1 (1152 samples per channel per frame)
    MP3_MPEG2 = 1,    // MPEG 2 (576 samples per channel per frame)
    MP3_MPEG2_5 = 2,  // MPEG 2.5 (576 samples per channel per frame)
};

/// @brief Equalizer presets for Mp3Decoder
///
/// Built-in 32-subband equalizer presets from the OpenCore decoder.
/// Applied in the frequency domain before polyphase synthesis.
/// All non-flat presets only attenuate (gains <= 0 dB), so they
/// will not clip but may reduce overall volume.
///
/// Can be changed between decode() calls for immediate effect.
enum Mp3Equalizer : uint8_t {
    MP3_EQ_FLAT = 0,        // No equalization (default)
    MP3_EQ_BASS_BOOST = 1,  // Boost low frequencies relative to high
    MP3_EQ_ROCK = 2,        // Rock preset (bass + mid emphasis)
    MP3_EQ_POP = 3,         // Pop preset (mid-high cut)
    MP3_EQ_JAZZ = 4,        // Jazz preset (low-mid emphasis)
    MP3_EQ_CLASSICAL = 5,   // Classical preset (low emphasis)
    MP3_EQ_TALK = 6,        // Talk/speech preset (mid emphasis, high cut)
};

/// Maximum PCM samples per channel per frame (1152 for MPEG1, 576 for MPEG2/2.5)
static constexpr size_t MP3_MAX_SAMPLES_PER_FRAME = 1152;

/// Maximum number of output channels
static constexpr size_t MP3_MAX_OUTPUT_CHANNELS = 2;

/// Minimum output buffer size in bytes for decode()
static constexpr size_t MP3_MIN_OUTPUT_BUFFER_BYTES =
    MP3_MAX_SAMPLES_PER_FRAME * MP3_MAX_OUTPUT_CHANNELS * sizeof(int16_t);  // 4608

/// Internal input buffer size, large enough for the maximum Layer III frame.
/// Worst case is 1441 bytes, from the highest bitrate and lowest sample rate:
///   MPEG1:   144 * 320kbps / 32kHz  + 1 = 1441  (32kHz is the lowest MPEG1 rate)
///   MPEG2.5:  72 * 160kbps /  8kHz  + 1 = 1441  ( 8kHz is the lowest MPEG2.5 rate)
/// Free-format frames (bitrate_index == 0) are not supported and rejected during header parsing.
static constexpr size_t MP3_INPUT_BUFFER_SIZE = 1536;

/// @brief Parsed MP3 frame header information
struct Mp3FrameInfo {
    int32_t frame_length;   // Frame size in bytes (>0), 0 = need more data, -1 = invalid
    uint32_t sample_rate;   // Sample rate in Hz
    uint32_t bitrate_kbps;  // Bitrate in kbps
    uint8_t channels;       // 1 = mono, 2 = stereo
    Mp3Version version;     // MPEG version
};

/**
 * @brief Streaming MP3 Decoder
 *
 * This class provides a high-level interface for decoding MP3 audio.
 * It handles:
 * - MP3 frame header parsing and synchronization
 * - Streaming decode with user-managed buffers
 * - Internal input buffering for frame accumulation
 * - MPEG1, MPEG2, and MPEG2.5 Layer III
 * - Gapless trimming (Xing/Info/LAME encoder delay and end padding)
 *
 * @warning Thread Safety: This class is NOT thread-safe. Each decoder instance
 *          must be accessed from only one thread at a time.
 *
 * @note Lazy Allocation: The constructor always succeeds and does not allocate
 *       any resources. All allocations (decoder state, internal buffers) are
 *       deferred until the first call to decode().
 *
 *       **First decode() call**: May return MP3_ALLOCATION_FAILED if memory
 *       allocation fails. If this occurs, the decoder remains in an uninitialized
 *       state, and subsequent calls will retry allocation.
 *
 * Usage:
 * 1. Create decoder instance (constructor always succeeds)
 * 2. Call decode() with chunks of MP3 data
 * 3. Check return value:
 *    - MP3_STREAM_INFO_READY (2): first call only; stream format parsed from
 *      frame header (no audio decoded). Advance input by bytes_consumed.
 *      Set up your audio pipeline using get_sample_rate() / get_channels() /
 *      get_bitrate(), then call decode() again to get the first frame's PCM.
 *    - MP3_NEED_MORE_DATA (1): partial frame buffered, feed more data.
 *    - MP3_OK (0): success; check samples_decoded.
 *    - MP3_OUTPUT_BUFFER_TOO_SMALL (-3): recoverable; the output buffer was too
 *      small for the frame. Enlarge it and call decode() again (the frame is
 *      retained). Cannot occur when output_size >= MP3_MIN_OUTPUT_BUFFER_BYTES.
 *    - MP3_DECODE_ERROR (-4): recoverable (bad frame skipped, advance and retry).
 *    - MP3_STREAM_INFO_CHANGED (-5): recoverable; the sample rate, channel count,
 *      or MPEG version changed mid-stream (no audio decoded). Re-read
 *      get_sample_rate() / get_channels() / get_version(), reconfigure your
 *      pipeline, then call decode() again to decode the new-format frame.
 *    - Other negative codes: fatal (allocation failure, invalid input, etc.).
 * 4. Check samples_decoded to see if you got audio samples
 * 5. Advance input pointer by bytes_consumed (always set, even on error)
 * 6. Repeat until stream is complete
 *
 * Example:
 * @code
 * Mp3Decoder decoder;  // Constructor always succeeds
 * std::vector<int16_t> pcm_buffer(MP3_MIN_OUTPUT_BUFFER_BYTES / sizeof(int16_t));
 *
 * while (have_data) {
 *     size_t consumed, samples;
 *     Mp3Result result = decoder.decode(
 *         input_ptr, input_len,
 *         reinterpret_cast<uint8_t*>(pcm_buffer.data()),
 *         pcm_buffer.size() * sizeof(int16_t),
 *         consumed, samples
 *     );
 *
 *     input_ptr += consumed;
 *     input_len -= consumed;
 *
 *     if (result == MP3_STREAM_INFO_READY || result == MP3_STREAM_INFO_CHANGED) {
 *         setup_pipeline(decoder.get_sample_rate(), decoder.get_channels());
 *         continue;  // No audio samples yet; re-call to decode the frame
 *     }
 *     if (result == MP3_DECODE_ERROR) {
 *         continue;  // Skip corrupt frame
 *     }
 *     if (result < 0) {
 *         break;  // Fatal error
 *     }
 *
 *     if (samples > 0) {
 *         process_audio(pcm_buffer, samples);
 *     }
 * }
 * @endcode
 */
class Mp3Decoder {
public:
    // ========================================
    // Lifecycle
    // ========================================

    /// @brief Construct a new MP3 Decoder
    ///
    /// The constructor always succeeds and does not allocate any resources.
    /// All allocations are deferred to the first call to decode().
    /// The decoder reports the actual channel count from the stream (1 or 2).
    Mp3Decoder();

    /// @brief Destroy the decoder and free resources
    ~Mp3Decoder();

    // Non-copyable, non-movable
    Mp3Decoder(const Mp3Decoder&) = delete;
    Mp3Decoder& operator=(const Mp3Decoder&) = delete;
    Mp3Decoder(Mp3Decoder&&) = delete;
    Mp3Decoder& operator=(Mp3Decoder&&) = delete;

    // ========================================
    // Core API
    // ========================================

    /// @brief Decode MP3 data and output PCM samples
    ///
    /// This method processes input data, parsing MP3 frames and decoding
    /// content to PCM output.
    ///
    /// @param input Pointer to input MP3 data (must not be nullptr)
    /// @param input_len Number of bytes available in input
    /// @param[out] output Pointer to output buffer for PCM samples (must not be nullptr).
    ///               The buffer should be aligned for int16_t access. Outputs
    ///               16-bit signed PCM samples (int16_t), interleaved for stereo.
    /// @param output_size Number of bytes available in output buffer.
    ///                    MP3_MIN_OUTPUT_BUFFER_BYTES (4608 bytes) is always
    ///                    sufficient: it covers the worst case (MPEG1 stereo:
    ///                    1152 * 2 * 2). A smaller buffer is accepted when it fits
    ///                    the stream's actual per-frame output (for example a mono
    ///                    or MPEG2/2.5 stream).
    /// @param[out] bytes_consumed Number of input bytes consumed. Always set,
    ///                           even on error. For MP3_STREAM_INFO_READY, the
    ///                       probe does NOT consume the frame body: this is 0
    ///                       in the common case (header parsed in place) or a
    ///                       small value (<= 3) when the header arrived
    ///                       piecemeal and bytes were copied to the internal
    ///                       buffer to complete it. Advance the input pointer
    ///                       by this amount; the frame body remains in the
    ///                       caller's buffer (so "input empty" stays a valid
    ///                       EOS signal even for single-frame files). ID3v2
    ///                       tag bytes and resync skip bytes are reported via
    ///                       separate MP3_NEED_MORE_DATA returns, not as part
    ///                       of MP3_STREAM_INFO_READY.
    ///                       For MP3_DECODE_ERROR, advance by this amount to skip
    ///                       the bad frame. For MP3_STREAM_INFO_CHANGED, advance
    ///                       by this amount (0 on the direct path, or the bytes
    ///                       buffered this call) and re-call after reconfiguring;
    ///                       the frame is retained and decoded on the next call.
    ///                       MP3_OUTPUT_BUFFER_TOO_SMALL behaves the same way:
    ///                       advance by this amount (0 unless bytes were buffered
    ///                       this call), enlarge the output buffer, and re-call;
    ///                       the frame is retained. For all other error codes
    ///                       (fatal errors), this is 0; i.e., do not advance. In
    ///                       the success case, this equals the bytes used for one
    ///                       frame. When buffering partial frame data, this
    ///                       equals the bytes copied into the internal buffer.
    /// @param[out] samples_decoded Number of valid PCM samples decoded (per channel)
    ///
    /// @return Mp3Result result code
    ///         - 0 (MP3_OK): Success (check samples_decoded)
    ///         - 1 (MP3_NEED_MORE_DATA): Incomplete frame buffered, feed more data
    ///         - -3 (MP3_OUTPUT_BUFFER_TOO_SMALL): Output buffer too small for the
    ///           frame; enlarge it and call again (recoverable)
    ///         - -4 (MP3_DECODE_ERROR): Corrupt frame skipped (recoverable)
    ///         - -5 (MP3_STREAM_INFO_CHANGED): Format changed mid-stream;
    ///           reconfigure from the accessors and call again (recoverable)
    ///         - Other negative values: Fatal error
    Mp3Result decode(const uint8_t* input, size_t input_len, uint8_t* output, size_t output_size,
                     size_t& bytes_consumed, size_t& samples_decoded);

    // ========================================
    // Accessors
    // ========================================

    /// @brief Get the bit depth of decoded samples
    /// @return Bit depth (always 16 for int16_t output samples)
    uint8_t get_bit_depth() const {
        return 16;
    }

    /// @brief Get the bitrate of the decoded audio
    /// @return Bitrate in kbps (e.g., 128, 320)
    ///         Returns 0 if not yet decoded. May vary frame to frame for VBR.
    uint32_t get_bitrate() const {
        return this->bitrate_;
    }

    /// @brief Get the number of bytes per sample
    /// @return Bytes per sample (always 2 for int16_t output samples)
    uint8_t get_bytes_per_sample() const {
        return 2;
    }

    /// @brief Get the number of output channels
    ///
    /// Reports the actual channel count from the MP3 stream: 1 for mono,
    /// 2 for stereo/joint stereo/dual channel.
    ///
    /// @return 1 or 2 (0 if not yet decoded)
    uint8_t get_channels() const {
        return this->output_channels_;
    }

    /// @brief Get the current equalizer preset
    /// @return Current Mp3Equalizer setting
    Mp3Equalizer get_equalizer() const {
        return this->equalizer_;
    }

    /// @brief Check if the decoder has been initialized
    /// @return true if decoder memory is allocated and ready
    bool is_initialized() const {
        return this->initialized_;
    }

    /// @brief Get the minimum output buffer size needed for decode()
    ///
    /// Always returns MP3_MIN_OUTPUT_BUFFER_BYTES (4608 bytes), which covers the
    /// worst case (MPEG1 stereo) and is sufficient for any stream. decode() also
    /// accepts a smaller buffer when the stream's actual frames fit (for example
    /// a mono or MPEG2/2.5 stream).
    ///
    /// @return Minimum output buffer size in bytes (always MP3_MIN_OUTPUT_BUFFER_BYTES)
    size_t get_min_output_buffer_bytes() const {
        return MP3_MIN_OUTPUT_BUFFER_BYTES;
    }

    /// @brief Get the sample rate of the decoded audio
    /// @return Sample rate in Hz (e.g., 44100, 48000, 22050)
    ///         Returns 0 if not yet decoded
    uint32_t get_sample_rate() const {
        return this->sample_rate_;
    }

    /// @brief Get the number of PCM samples per channel per frame
    ///
    /// MPEG1 produces 1152 samples per channel, MPEG2/2.5 produce 576.
    ///
    /// @return Samples per channel per frame (0 if not yet decoded)
    size_t get_samples_per_frame() const;

    /// @brief Get the MPEG version of the decoded stream
    /// @return Mp3Version (MP3_MPEG1, MP3_MPEG2, or MP3_MPEG2_5).
    ///         Returns MP3_MPEG1 if not yet decoded.
    Mp3Version get_version() const {
        return this->version_;
    }

    // ========================================
    // Mutators
    // ========================================

    /// @brief Set the equalizer preset
    ///
    /// Changes take effect on the next decode() call. Can be changed
    /// at any time, including between frames during decoding.
    ///
    /// @param eq Equalizer preset to use
    void set_equalizer(Mp3Equalizer eq) {
        this->equalizer_ = eq;
    }

    /// @brief Reset the decoder state
    ///
    /// Resets all internal state, allowing the decoder to be reused for a new
    /// stream. Frees all allocated memory. After calling reset(), the next
    /// decode() call will re-allocate resources.
    void reset();

private:
    // ========================================
    // Internal Helpers
    // ========================================

    /// @brief Initialize the decoder (lazy init on first decode)
    ///
    /// Allocates decoder state memory and initializes the OpenCore library.
    ///
    /// @return MP3_OK on success, MP3_ALLOCATION_FAILED on failure
    Mp3Result initialize();

    /// @brief Parse the first MP3 frame header to determine stream format
    ///
    /// Parses the frame header to extract sample rate, channel count, bitrate,
    /// and version without decoding audio. No scratch buffer needed.
    ///
    /// Sets sample_rate_, output_channels_, bitrate_, version_, and probe_done_.
    ///
    /// @param input     Pointer to input MP3 data
    /// @param input_len Number of bytes available in input
    /// @param[out] bytes_consumed Bytes consumed (0 if need more data)
    /// @return MP3_STREAM_INFO_READY when probe is complete,
    ///         MP3_NEED_MORE_DATA if more data is needed
    Mp3Result run_probe(const uint8_t* input, size_t input_len, size_t& bytes_consumed);

    /// @brief Parse an MP3 frame header for length and stream properties
    ///
    /// Extracts version, layer, bitrate, sample rate, channel count, and padding
    /// from the 4-byte MP3 header. Validates sync word and Layer III.
    /// Calculates frame size using standard formulas.
    ///
    /// @param data Pointer to potential MP3 frame header
    /// @param len Number of bytes available at data
    /// @return Mp3FrameInfo with frame_length:
    ///          >0: total frame bytes (including header), other fields valid
    ///           0: need more data (fewer than 4 bytes available)
    ///          -1: invalid (no sync word, unsupported layer, or bad indices)
    static Mp3FrameInfo parse_mp3_frame_header(const uint8_t* data, size_t len);

    /// @brief Parse the total size of an ID3v2 tag
    ///
    /// Detects the "ID3" signature and decodes the syncsafe size field.
    /// Accounts for the 10-byte header and optional 10-byte footer.
    ///
    /// @param data Pointer to potential ID3v2 header
    /// @param len Number of bytes available at data
    /// @return Total tag size in bytes (header + body + optional footer),
    ///         or 0 if not a valid ID3v2 header (requires at least 10 bytes)
    static size_t parse_id3v2_tag_size(const uint8_t* data, size_t len);

    /// @brief Detect a Xing/Info VBR header frame and extract gapless metadata
    ///
    /// The first frame of a LAME/Xing/Fraunhofer-encoded stream is an
    /// informational frame (silent audio payload) carrying the "Xing" or "Info"
    /// magic after the side information. When a LAME extension follows, it
    /// stores the encoder delay and padding. This frame must not be emitted as
    /// audio.
    ///
    /// @param frame         Pointer to a complete frame (header + body)
    /// @param frame_len     Length of the frame in bytes
    /// @param version       MPEG version (determines side-info size)
    /// @param channels      Channel count (1 = mono, determines side-info size)
    /// @param[out] encoder_delay   Encoder delay from the LAME tag (0 if absent)
    /// @param[out] encoder_padding Encoder padding from the LAME tag (0 if absent)
    /// @param[out] frame_count     Xing/Info frame count, excluding this header
    ///                             frame (0 if the count field is absent)
    /// @return true if the frame is a Xing/Info header frame (and must be
    ///         skipped); false if it is an ordinary audio frame
    static bool parse_vbr_header(const uint8_t* frame, size_t frame_len, Mp3Version version,
                                 uint8_t channels, uint16_t& encoder_delay,
                                 uint16_t& encoder_padding, uint32_t& frame_count);

    /// @brief Classify a complete frame and, if it is a Xing/Info header frame,
    ///        arm the gapless start and end trim state
    ///
    /// On a header frame, primes start_skip_remaining_ with the encoder delay
    /// plus the fixed decoder delay, and (when the Xing frame count is present)
    /// arms end trimming via output_samples_remaining_. The caller is
    /// responsible for consuming the frame's bytes and emitting no PCM.
    ///
    /// @param frame     Pointer to a complete frame (header + body)
    /// @param frame_len Length of the frame in bytes
    /// @return true if the frame was a Xing/Info header frame (skip it)
    bool record_vbr_header_frame(const uint8_t* frame, size_t frame_len);

    /// @brief Detect a mid-stream sample rate, channel count, or version change
    ///
    /// Compares a parsed frame header against the established stream format. On a
    /// change, updates the stored sample rate / channel count / version (so the
    /// accessors report the new values), discards the prior stream's gapless trim
    /// state, re-arms Xing/Info detection for the new stream, and returns true,
    /// letting decode() surface MP3_STREAM_INFO_CHANGED before the frame is
    /// decoded. Bitrate is excluded because it varies per frame on VBR. A header
    /// with frame_length <= 0 is ignored (returns false).
    ///
    /// @param info Parsed header of the frame about to be decoded
    /// @return true if the format changed (stored values were updated)
    bool detect_format_change(const Mp3FrameInfo& info);

    /// @brief Set up the OpenCore ext struct with common fields for decoding
    ///
    /// @param output_size Caller's output buffer capacity in bytes. Passed
    ///        through to OpenCore as the per-frame write bound, so a buffer
    ///        smaller than the worst case is rejected rather than overrun.
    void setup_decode_ext(::tPVMP3DecoderExternal& ext, uint8_t* output, size_t output_size);

    /// @brief Decode directly from user buffer (no leftover data in internal buffer)
    Mp3Result decode_direct(::tPVMP3DecoderExternal& ext, const uint8_t* input, size_t input_len,
                            size_t& bytes_consumed, bool& frame_decoded);

    /// @brief Decode from internal buffer with leftover data from a previous call
    Mp3Result decode_buffered(::tPVMP3DecoderExternal& ext, const uint8_t* input, size_t input_len,
                              size_t& bytes_consumed, bool& frame_decoded);

    /// @brief Update stream properties after a successful decode
    void finalize_decode(::tPVMP3DecoderExternal& ext, size_t& samples_decoded);

    // ========================================
    // Member Variables
    // ========================================

    /// Classification of a short unvalidated fragment held in input_buffer_.
    /// Recorded when the fragment is buffered, so tag detection never has to
    /// infer intent from buffer content alone (a mid-stream frame fragment
    /// whose bytes happen to spell a tag magic must not be treated as a tag).
    enum class PendingTag : uint8_t {
        NONE,   ///< Frame data or garbage; the decode paths resync as needed
        ID3V2,  ///< Bytes so far match a prefix of an ID3v2 "ID3" header
    };

    // Pointer fields
    void* decoder_memory_{nullptr};   // Decoder state memory (opaque to OpenCore)
    uint8_t* input_buffer_{nullptr};  // Internal input buffer for accumulating MP3 data

    // Struct fields
    Mp3FrameInfo expected_frame_info_{};  // Parsed header backing expected_frame_length_

    // size_t fields
    size_t expected_frame_length_{0};  // MP3 frame length from parsed header, 0 = unknown
    size_t id3_skip_remaining_{0};  // Remaining bytes to skip for an ID3v2 tag (0 = no active skip)
    size_t input_buffer_fill_{0};   // Number of valid bytes in the internal input buffer
    size_t output_samples_remaining_{0};  // Per-channel output samples still allowed (gapless end)
    size_t start_skip_remaining_{0};      // Leading PCM samples to drop at start (gapless)

    // 32-bit fields
    uint32_t bitrate_{0};      // Stream bitrate (set after first successful decode)
    uint32_t sample_rate_{0};  // Stream sample rate (set after first successful decode)

    // 8-bit fields
    bool end_trim_active_{false};          // True when the Xing frame count armed end trimming
    Mp3Equalizer equalizer_{MP3_EQ_FLAT};  // Equalizer preset (applied per-frame during decode)
    bool initialized_{false};              // True after decoder memory is allocated and set up
    uint8_t output_channels_{0};  // Decoded channel count (1 = mono, 2 = stereo), 0 = unknown
    PendingTag pending_tag_{PendingTag::NONE};  // What the buffered fragment was classified as
    bool probe_done_{false};          // True after first frame header parsed (probe complete)
    bool vbr_header_checked_{false};  // Set once the first frame was checked for a Xing/Info header
    Mp3Version version_{MP3_MPEG1};
};

}  // namespace micro_mp3
