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

/* MP3 to WAV Converter
 * Converts .mp3 files to .wav format using micro_mp3
 */

#include "micro_mp3/mp3_decoder.h"
#include "wav_writer.h"

#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

struct DecodeStats {
    size_t total_packets = 0;
    size_t total_bytes_read = 0;
    size_t total_bytes_consumed = 0;
    size_t decode_calls = 0;
    size_t decode_errors = 0;
};

void print_usage(const char* program_name) {
    std::cerr << "Usage: " << program_name << " <input.mp3> <output.wav>\n";
    std::cerr << "\nConverts an MP3 file to WAV format.\n";
}

void print_error_description(micro_mp3::Mp3Result result) {
    switch (result) {
        case micro_mp3::MP3_INPUT_INVALID:
            std::cerr << " (MP3_INPUT_INVALID - Invalid input data)";
            break;
        case micro_mp3::MP3_ALLOCATION_FAILED:
            std::cerr << " (MP3_ALLOCATION_FAILED - Memory allocation failed)";
            break;
        case micro_mp3::MP3_OUTPUT_BUFFER_TOO_SMALL:
            std::cerr << " (MP3_OUTPUT_BUFFER_TOO_SMALL - Output buffer too small)";
            break;
        case micro_mp3::MP3_DECODE_ERROR:
            std::cerr << " (MP3_DECODE_ERROR - Decode failed)";
            break;
        default:
            break;
    }
}

/// Create the WavWriter once stream format is known (on MP3_STREAM_INFO_READY).
/// Returns false if the output file could not be opened.
bool try_initialize_wav_writer(std::unique_ptr<WavWriter>& wav_writer,
                               micro_mp3::Mp3Decoder& decoder, const char* output_file) {
    if (wav_writer) {
        return true;
    }

    uint32_t sample_rate = decoder.get_sample_rate();
    uint8_t channels = decoder.get_channels();

    std::cout << "MP3 stream info:\n";
    std::cout << "  Sample rate: " << sample_rate << " Hz\n";
    std::cout << "  Channels: " << static_cast<int>(channels) << "\n";
    std::cout << "  Bit depth: " << static_cast<int>(decoder.get_bit_depth()) << " bits\n";
    std::cout << "  Bitrate: " << decoder.get_bitrate() << " kbps\n";

    wav_writer = std::make_unique<WavWriter>(output_file, sample_rate, channels, 16);

    if (!wav_writer->isOpen()) {
        std::cerr << "Error: Could not create output file: " << output_file << "\n";
        return false;
    }

    return true;
}

/// Decode all complete MP3 frames from a chunk of input data.
/// Returns false on fatal error.
bool decode_chunk(micro_mp3::Mp3Decoder& decoder, const uint8_t* data, size_t data_size,
                  std::vector<int16_t>& pcm_buffer, std::unique_ptr<WavWriter>& wav_writer,
                  const char* output_file, DecodeStats& stats) {
    size_t chunk_offset = 0;

    while (chunk_offset < data_size) {
        size_t consumed = 0;
        size_t samples = 0;

        stats.decode_calls++;

        micro_mp3::Mp3Result result =
            decoder.decode(data + chunk_offset, data_size - chunk_offset,
                           reinterpret_cast<uint8_t*>(pcm_buffer.data()),
                           pcm_buffer.size() * sizeof(int16_t), consumed, samples);

        stats.total_bytes_consumed += consumed;
        chunk_offset += consumed;

        if (result == micro_mp3::MP3_STREAM_INFO_READY) {
            if (!try_initialize_wav_writer(wav_writer, decoder, output_file)) {
                return false;
            }
            continue;  // samples_decoded is 0; nothing to write
        }

        if (result < 0) {
            if (result == micro_mp3::MP3_OUTPUT_BUFFER_TOO_SMALL) {
                size_t larger_size = pcm_buffer.size() * 2;
                std::cout << "Resizing PCM buffer from " << pcm_buffer.size() << " to "
                          << larger_size << " samples\n";
                pcm_buffer.resize(larger_size);
                continue;
            }

            if (result == micro_mp3::MP3_DECODE_ERROR) {
                // Recoverable — the decoder has already skipped the bad frame
                stats.decode_errors++;
                if (stats.decode_errors == 1) {
                    std::cerr << "Warning: skipping corrupt frame at byte position "
                              << stats.total_bytes_consumed << "\n";
                }
                continue;
            }

            // Fatal errors (allocation failure, invalid input, etc.)
            std::cerr << "Error at byte position " << stats.total_bytes_consumed << " in file\n";
            std::cerr << "Decode call #" << stats.decode_calls << ", consumed=" << consumed
                      << ", samples=" << samples << "\n";
            std::cerr << "Error: Decoding failed with error code: " << static_cast<int>(result);
            print_error_description(result);
            std::cerr << "\n";
            return false;
        }

        if (samples > 0) {
            stats.total_packets++;

            if (wav_writer) {
                if (!wav_writer->writeSamples(pcm_buffer.data(), samples)) {
                    std::cerr << "Error: Failed to write samples to WAV file\n";
                    return false;
                }
            }
        }

        // Decoder needs more data
        if (consumed == 0 && samples == 0) {
            break;
        }
    }

    return true;
}

void print_stats(const DecodeStats& stats, const WavWriter& wav_writer, uint32_t sample_rate) {
    std::cout << "\nConversion complete!\n";
    std::cout << "Total decode() calls: " << stats.decode_calls << "\n";
    std::cout << "Total bytes read from file: " << stats.total_bytes_read << "\n";
    std::cout << "Total bytes consumed by decoder: " << stats.total_bytes_consumed << "\n";
    std::cout << "Average bytes per packet: "
              << (stats.total_bytes_consumed / (stats.total_packets > 0 ? stats.total_packets : 1))
              << "\n";
    std::cout << "Total packets decoded: " << stats.total_packets << "\n";
    if (stats.decode_errors > 0) {
        std::cout << "Corrupt frames skipped: " << stats.decode_errors << "\n";
    }
    std::cout << "Total samples written: " << wav_writer.getSamplesWritten() << "\n";
    std::cout << "Duration: " << (wav_writer.getSamplesWritten() / static_cast<double>(sample_rate))
              << " seconds\n";
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 3) {
            print_usage(argv[0]);
            return 1;
        }

        const char* input_file = argv[1];
        const char* output_file = argv[2];

        // Open input file
        std::ifstream input(input_file, std::ios::binary);
        if (!input) {
            std::cerr << "Error: Could not open input file: " << input_file << "\n";
            return 1;
        }

        micro_mp3::Mp3Decoder decoder;
        std::unique_ptr<WavWriter> wav_writer;
        DecodeStats stats;

        // Input buffer - read chunks sequentially, decoder handles internal buffering
        const size_t chunk_size = 4096;
        std::vector<uint8_t> input_buffer(chunk_size);

        // Output PCM buffer - MP3 frame: up to 1152 samples per channel
        // Worst case: 1152 samples * 2 channels * 2 bytes = 4608 bytes
        // Use MP3_MIN_OUTPUT_BUFFER_BYTES to ensure sufficient size
        const size_t pcm_buffer_size = micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES / sizeof(int16_t);
        std::vector<int16_t> pcm_buffer(pcm_buffer_size);

        // Process file - read chunks and feed to decoder
        while (input) {
            input.read(reinterpret_cast<char*>(input_buffer.data()), chunk_size);
            std::streamsize bytes_read = input.gcount();

            if (bytes_read == 0) {
                break;
            }

            stats.total_bytes_read += bytes_read;

            if (!decode_chunk(decoder, input_buffer.data(), static_cast<size_t>(bytes_read),
                              pcm_buffer, wav_writer, output_file, stats)) {
                return 1;
            }
        }

        if (!wav_writer) {
            std::cerr << "Error: No MP3 stream found in input file\n";
            return 1;
        }

        print_stats(stats, *wav_writer, decoder.get_sample_rate());
        std::cout << "Output file: " << output_file << "\n";

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
