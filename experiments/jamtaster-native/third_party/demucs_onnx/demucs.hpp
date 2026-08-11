#ifndef MODEL_HPP
#define MODEL_HPP

#include "dsp.hpp"
#include "tensor.hpp"
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>

namespace demucsonnx
{
using ProgressCallback = std::function<void(float, const std::string &)>;

struct demucs_model {
    std::unique_ptr<Ort::Session> sess;
    int nb_sources = 0;
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "jamtaster_demucs"};
    std::vector<std::string> input_names;
    std::vector<std::string> output_names;

    std::vector<const char*> input_names_ptrs;
    std::vector<const char*> output_names_ptrs;
};

bool load_model(const std::filesystem::path &path,
                struct demucs_model &model,
                Ort::SessionOptions &session_options);

struct demucs_segment_buffers
{
    int segment_samples;
    int le;
    int pad;
    int pad_end;
    int padded_segment_samples;
    int nb_stft_frames;
    int nb_stft_bins;

    Eigen::Tensor3dXf targets_out;
    Eigen::MatrixXf padded_mix;
    Eigen::Tensor3dXcf z;

    std::vector<int64_t> x_onnx_in_shape;
    std::vector<int64_t> xt_onnx_in_shape;

    std::vector<int64_t> x_onnx_out_shape;
    std::vector<int64_t> xt_onnx_out_shape;

    Ort::AllocatorWithDefaultOptions allocator;
    std::vector<Ort::Value> input_tensors;
    std::vector<Ort::Value> output_tensors;

    // constructor for demucs_segment_buffers that takes int parameters

    // let's do pesky precomputing of the signal repadding to 1/4 hop
    // for time and frequency alignment
    demucs_segment_buffers(int nb_channels, int segment_samples, int nb_sources)
        : segment_samples(segment_samples),
          le(int(std::ceil((float)segment_samples / (float)FFT_HOP_SIZE))),
          pad(3 * FFT_HOP_SIZE / 2),
          pad_end(pad + le * FFT_HOP_SIZE - segment_samples),
          padded_segment_samples(segment_samples + pad + pad_end),
          nb_stft_frames(segment_samples / demucsonnx::FFT_HOP_SIZE + 1),
          nb_stft_bins(demucsonnx::FFT_WINDOW_SIZE / 2 + 1),
          targets_out(nb_sources, nb_channels, segment_samples),
          padded_mix(nb_channels, padded_segment_samples),
          z(nb_channels, nb_stft_bins, nb_stft_frames+4),
          // complex-as-channels implies 2*nb_channels for real+imag
          x_onnx_in_shape({1, 2 * nb_channels, nb_stft_bins - 1, nb_stft_frames}),
          xt_onnx_in_shape({1, nb_channels, segment_samples}),
          x_onnx_out_shape({1, nb_sources, 2 * nb_channels, nb_stft_bins - 1, nb_stft_frames}),
          xt_onnx_out_shape({1, nb_sources, nb_channels, segment_samples})
    {
        // precompute the input tensors
        // inputs in form (xt, x)
        input_tensors.push_back(Ort::Value::CreateTensor<float>(
            allocator,
            xt_onnx_in_shape.data(),
            xt_onnx_in_shape.size()));

        input_tensors.push_back(Ort::Value::CreateTensor<float>(
            allocator,
            x_onnx_in_shape.data(),
            x_onnx_in_shape.size()));

        // precompute the output tensors
        // outputs in form (x_out, xt_out)
        output_tensors.push_back(Ort::Value::CreateTensor<float>(
            allocator,
            x_onnx_out_shape.data(),
            x_onnx_out_shape.size()));

        output_tensors.push_back(Ort::Value::CreateTensor<float>(
            allocator,
            xt_onnx_out_shape.data(),
            xt_onnx_out_shape.size()));
    };
};

constexpr float SEGMENT_LEN_SECS = 7.8F;
constexpr float MAX_SHIFT_SECS = 0.5F;
constexpr float OVERLAP = 0.25F;
constexpr float TRANSITION_POWER = 1.0F;

Eigen::Tensor3dXf demucs_inference(struct demucs_model &model,
                                   const Eigen::MatrixXf &audio,
                                   ProgressCallback cb,
                                   std::uint32_t shift_seed,
                                   int &used_shift_offset);

void model_inference(struct demucs_model &model,
                     struct demucsonnx::demucs_segment_buffers &buffers,
                     struct demucsonnx::stft_buffers &stft_buf);
} // namespace demucsonnx

#endif // MODEL_HPP
