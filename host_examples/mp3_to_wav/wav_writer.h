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
/// @brief Simple WAV file writer for PCM audio in RIFF/WAVE format

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

/// @brief Writes int16_t PCM samples to a standard RIFF/WAVE file
///
/// The header is written on construction with placeholder sizes and rewritten
/// with the final byte counts on destruction, so the writer can stream samples
/// without knowing the total length in advance.
class WavWriter {
public:
    // ========================================
    // Lifecycle
    // ========================================

    /// @brief Construct a new WAV writer and open the output file
    /// @param filename Output WAV file path
    /// @param sample_rate Sample rate in Hz
    /// @param num_channels Number of channels (1 = mono, 2 = stereo)
    /// @param bits_per_sample Bits per sample (typically 16)
    WavWriter(const std::string& filename, uint32_t sample_rate, uint16_t num_channels,
              uint16_t bits_per_sample = 16);

    /// @brief Destroy the WAV writer and finalize the file
    ~WavWriter();

    // Non-copyable
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    // ========================================
    // Core API
    // ========================================

    /// @brief Write PCM samples to the WAV file
    /// @param samples Pointer to interleaved PCM samples (int16_t array)
    /// @param num_samples Number of samples per channel
    /// @return true on success, false on write error
    bool write_samples(const int16_t* samples, size_t num_samples);

    // ========================================
    // Accessors
    // ========================================

    /// @brief Check whether the output file is open and ready
    /// @return true if the file is open
    bool is_open() const {
        return this->file_ != nullptr;
    }

    /// @brief Get the total number of samples written
    /// @return Total samples written per channel
    uint32_t get_samples_written() const {
        return this->samples_written_;
    }

private:
    // ========================================
    // Internal Helpers
    // ========================================

    /// @brief Write the RIFF/WAVE header with placeholder sizes
    void write_header();

    /// @brief Rewrite the header with the final RIFF and data chunk sizes
    void update_header();

    // ========================================
    // Member Variables
    // ========================================

    // Pointer fields
    FILE* file_;  // Output file handle, nullptr when not open

    // 32-bit fields
    uint32_t sample_rate_;         // Sample rate in Hz
    uint32_t samples_written_{0};  // Total samples written per channel

    // 16-bit fields
    uint16_t bits_per_sample_;  // Bits per sample
    uint16_t num_channels_;     // Number of channels
};
