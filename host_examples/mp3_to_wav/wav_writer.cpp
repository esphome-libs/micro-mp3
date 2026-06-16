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

#include "wav_writer.h"

#include <cstring>

// ============================================================================
// Constants and Helpers
// ============================================================================

// WAV file format constants
// RIFF chunk_size = total_file_size - 8 (excludes "RIFF" tag and chunk_size field itself).
// Total header on disk is 44 bytes (12 RIFF + 24 fmt + 8 data), so chunk_size = data_size + 36.
constexpr uint32_t WAV_HEADER_SIZE = 36;

// WAV file header structures (little-endian)
#pragma pack(push, 1)

/// @brief RIFF container header that precedes the WAVE chunks
struct RIFFHeader {
    char chunk_id[4];     // "RIFF"
    uint32_t chunk_size;  // File size - 8
    char format[4];       // "WAVE"
};

/// @brief Format chunk describing the PCM sample layout
struct FmtChunk {
    char chunk_id[4];       // "fmt "
    uint32_t chunk_size;    // 16 for PCM
    uint16_t audio_format;  // 1 for PCM
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;    // sample_rate * num_channels * bits_per_sample/8
    uint16_t block_align;  // num_channels * bits_per_sample/8
    uint16_t bits_per_sample;
};

/// @brief Data chunk header that precedes the raw PCM bytes
struct DataChunk {
    char chunk_id[4];     // "data"
    uint32_t chunk_size;  // Number of bytes in data
};
#pragma pack(pop)

// ============================================================================
// Lifecycle
// ============================================================================

WavWriter::WavWriter(const std::string& filename, uint32_t sample_rate, uint16_t num_channels,
                     uint16_t bits_per_sample)
    : file_(fopen(filename.c_str(), "wb")),
      sample_rate_(sample_rate),
      bits_per_sample_(bits_per_sample),
      num_channels_(num_channels) {
    if (this->file_) {
        this->write_header();
    }
}

WavWriter::~WavWriter() {
    if (this->file_) {
        this->update_header();
        fclose(this->file_);
        this->file_ = nullptr;
    }
}

// ============================================================================
// Core API
// ============================================================================

bool WavWriter::write_samples(const int16_t* samples, size_t num_samples) {
    if (!this->file_ || !samples || num_samples == 0) {
        return false;
    }

    size_t bytes_to_write = num_samples * this->num_channels_ * sizeof(int16_t);
    size_t bytes_written = fwrite(samples, 1, bytes_to_write, this->file_);

    if (bytes_written == bytes_to_write) {
        this->samples_written_ += static_cast<uint32_t>(num_samples);
        return true;
    }

    return false;
}

// ============================================================================
// Internal Helpers
// ============================================================================

void WavWriter::write_header() {
    RIFFHeader riff{};
    memcpy(riff.chunk_id, "RIFF", 4);
    riff.chunk_size = 0;  // Placeholder, finalized by update_header()
    memcpy(riff.format, "WAVE", 4);

    FmtChunk fmt{};
    memcpy(fmt.chunk_id, "fmt ", 4);
    fmt.chunk_size = 16;
    fmt.audio_format = 1;  // PCM
    fmt.num_channels = this->num_channels_;
    fmt.sample_rate = this->sample_rate_;
    fmt.byte_rate = this->sample_rate_ * this->num_channels_ * (this->bits_per_sample_ / 8);
    fmt.block_align = this->num_channels_ * (this->bits_per_sample_ / 8);
    fmt.bits_per_sample = this->bits_per_sample_;

    DataChunk data{};
    memcpy(data.chunk_id, "data", 4);
    data.chunk_size = 0;  // Placeholder, finalized by update_header()

    fwrite(&riff, sizeof(riff), 1, this->file_);
    fwrite(&fmt, sizeof(fmt), 1, this->file_);
    fwrite(&data, sizeof(data), 1, this->file_);
}

void WavWriter::update_header() {
    if (!this->file_) {
        return;
    }

    uint32_t data_size =
        this->samples_written_ * this->num_channels_ * (this->bits_per_sample_ / 8);
    uint32_t file_size = data_size + WAV_HEADER_SIZE;

    // Update RIFF chunk size
    fseek(this->file_, 4, SEEK_SET);
    fwrite(&file_size, 4, 1, this->file_);

    // Update data chunk size
    fseek(this->file_, 40, SEEK_SET);
    fwrite(&data_size, 4, 1, this->file_);

    // Return to end of file
    fseek(this->file_, 0, SEEK_END);
}
