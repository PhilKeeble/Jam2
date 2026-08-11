#include "demucs.hpp"
#include "dsp.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <unsupported/Eigen/CXX11/Tensor>

static void reflect_padding(
    Eigen::MatrixXf &padded_mix,
    int left_padding,
    int right_padding,
    int N)
{
    // Match torch.nn.functional.pad(..., mode="reflect"): reflection excludes
    // the boundary sample itself.
    for (int i = 0; i < left_padding; ++i)
    {
        padded_mix.block(0, left_padding - 1 - i, 2, 1) =
            padded_mix.block(0, left_padding + 1 + i, 2, 1);
    }

    // Reflect from the last 'right_padding' samples of the original data
    for (int i = 0; i < right_padding; ++i)
    {
        int last_elem = N + left_padding - 1 ;
        padded_mix.block(0, last_elem + i + 1, 2, 1) =
            padded_mix.block(0, last_elem - 1 - i, 2, 1);
    }
}

Eigen::Tensor3dXf demucsonnx::demucs_inference(
    struct demucsonnx::demucs_model &model,
    const Eigen::MatrixXf &audio,
    demucsonnx::ProgressCallback cb,
    std::uint32_t shift_seed,
    int &used_shift_offset)
{
    if (audio.rows() != 2 || audio.cols() < 2) {
        throw std::invalid_argument("Demucs requires at least two stereo frames");
    }
    if (audio.cols() > std::numeric_limits<int>::max()) {
        throw std::length_error("Demucs input exceeds the native adapter's sample limit");
    }
    const int length = static_cast<int>(audio.cols());
    int max_shift = static_cast<int>(demucsonnx::MAX_SHIFT_SECS * demucsonnx::SUPPORTED_SAMPLE_RATE);

    // Calculate the overall mean and standard deviation
    Eigen::VectorXf ref_mean_0 = audio.colwise().mean();
    float ref_mean = ref_mean_0.mean();
    float ref_std = std::sqrt((ref_mean_0.array() - ref_mean).square().sum() /
                              (ref_mean_0.size() - 1)) + 1.0e-8F;

    // Create padded_mix with symmetric zero padding
    int padded_length = length + 2 * max_shift;
    Eigen::MatrixXf padded_mix(2, padded_length);
    padded_mix.setZero();

    // Normalize and copy audio into padded_mix starting at column max_shift
    padded_mix.block(0, max_shift, 2, length) = (audio.array() - ref_mean) / ref_std;

    // Generate random shift offset for time invariance
    std::mt19937 generator(shift_seed);
    std::uniform_int_distribution<> dist(0, max_shift);
    int shift_offset = dist(generator);
    used_shift_offset = shift_offset;

    int shifted_length = length + max_shift - shift_offset;

    // --- Begin merged split_inference and segment_inference logic ---

    // Calculate segment size in samples
    int segment_samples = static_cast<int>(demucsonnx::SEGMENT_LEN_SECS * demucsonnx::SUPPORTED_SAMPLE_RATE);
    int nb_out_sources = model.nb_sources;

    // Create reusable buffers with padded sizes
    demucsonnx::demucs_segment_buffers buffers(2, segment_samples, nb_out_sources);
    demucsonnx::stft_buffers stft_buf(buffers.padded_segment_samples);

    // Calculate stride for overlapping segments
    int stride_samples = static_cast<int>((1.0f - demucsonnx::OVERLAP) * segment_samples);

    // Create an output tensor initialized to zero
    Eigen::Tensor3dXf out(nb_out_sources, 2, shifted_length);
    out.setZero();

    // Create weight vector for overlapping segments
    Eigen::VectorXf weight(segment_samples);
    int half_segment = segment_samples / 2;
    weight.head(half_segment) = Eigen::VectorXf::LinSpaced(
        half_segment, 1.0F, static_cast<float>(half_segment));
    weight.tail(half_segment) = weight.head(half_segment).reverse();
    weight /= weight.maxCoeff();
    weight = weight.array().pow(demucsonnx::TRANSITION_POWER);

    // Initialize sum_weight as Eigen::VectorXf
    Eigen::VectorXf sum_weight(shifted_length);
    sum_weight.setZero();

    // Calculate total number of chunks
    int total_chunks = static_cast<int>(std::ceil(static_cast<float>(shifted_length) / stride_samples));
    float increment_per_chunk = 1.0f / static_cast<float>(total_chunks);
    float inference_progress = 0.0f;

    // Preallocate a tensor for chunk_out to avoid repeated allocations
    Eigen::Tensor3dXf chunk_out(nb_out_sources, 2, segment_samples);
    chunk_out.setZero();

    // Process each segment
    for (int segment_offset = 0; segment_offset < shifted_length; segment_offset += stride_samples)
    {
        int chunk_length = std::min(segment_samples, shifted_length - segment_offset);

        // HTDemucs evaluates every split at its fixed training length. Match
        // nested TensorChunk.padded(): a short final chunk receives real context
        // from before its logical start (and any available outer shift padding),
        // then its returned output is centre-trimmed back to chunk_length.
        const int model_padding = segment_samples - chunk_length;
        const int model_padding_start = model_padding / 2;
        const int logical_start = shift_offset + segment_offset;
        const int requested_start = logical_start - model_padding_start;
        const int requested_end = requested_start + segment_samples;
        const int copy_start = std::max(0, requested_start);
        const int copy_end = std::min(padded_length, requested_end);
        const int copy_length = std::max(0, copy_end - copy_start);
        const int destination_start = copy_start - requested_start;
        buffers.padded_mix.setZero();
        if (copy_length > 0) {
            buffers.padded_mix.block(
                0, buffers.pad + destination_start, 2, copy_length) =
                padded_mix.block(0, copy_start, 2, copy_length);
        }

        // then, reflect padding on the left and right for
        // the stft boundary effects
        reflect_padding(buffers.padded_mix, buffers.pad, buffers.pad_end, segment_samples);

        // Run model inference
        demucsonnx::model_inference(model, buffers, stft_buf);

        // Update progress
        if (cb) cb(inference_progress + increment_per_chunk, "Segment inference complete");

        // Undo the model-input centring while copying the real chunk output.
        // Preallocate chunk_out and set to zero
        chunk_out.setZero();

        for (int i = 0; i < nb_out_sources; ++i)
        {
            for (int j = 0; j < 2; ++j)
            {
                for (int k = 0; k < chunk_length; ++k)
                {
                    chunk_out(i, j, k) =
                        buffers.targets_out(i, j, k + model_padding_start);
                }
            }
        }

        // Accumulate the weighted chunk output using Eigen::Map for vectorization
        for (int i = 0; i < nb_out_sources; ++i)
        {
            for (int j = 0; j < 2; ++j)
            {
                // JamTaster's tensor aliases are explicitly row-major, making
                // the final time dimension contiguous for vectorized overlap-add.
                Eigen::Map<Eigen::ArrayXf> out_slice(
                    &out(i, j, segment_offset), chunk_length);
                Eigen::Map<const Eigen::ArrayXf> chunk_out_slice(
                    &chunk_out(i, j, 0), chunk_length);
                out_slice += weight.head(chunk_length).array() * chunk_out_slice;
            }
        }

        // Accumulate the weights using Eigen's segment operation
        sum_weight.segment(segment_offset, chunk_length) += weight.head(chunk_length);

        inference_progress += increment_per_chunk;
    }

    // Normalize the output by sum_weight using vectorized operations
    for (int i = 0; i < nb_out_sources; ++i)
    {
        for (int j = 0; j < 2; ++j)
        {
            Eigen::Map<Eigen::ArrayXf> out_slice(&out(i, j, 0), shifted_length);
            out_slice /= sum_weight.array();
        }
    }

    // Inverse the normalization directly on 'out' using vectorized operations
    for (int i = 0; i < nb_out_sources; ++i)
    {
        for (int j = 0; j < 2; ++j)
        {
            Eigen::Map<Eigen::ArrayXf> out_slice(&out(i, j, 0), shifted_length);
            out_slice = out_slice * ref_std + ref_mean;
        }
    }

    // Create a view (slice) of 'out' without allocating new memory
    int trim_start = max_shift - shift_offset;
    Eigen::array<Eigen::Index, 3> offset_array = {0, 0, trim_start};
    Eigen::array<Eigen::Index, 3> extent_array = {nb_out_sources, 2, length};
    auto result = out.slice(offset_array, extent_array);

    // Materialize the slice into a new tensor to return
    Eigen::Tensor3dXf trimmed_waveform_outputs = result.eval();

    return trimmed_waveform_outputs;
}
