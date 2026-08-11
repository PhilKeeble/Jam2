#include "demucs.hpp"
#include "dsp.hpp"
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#include <stdexcept>

bool demucsonnx::load_model(const std::filesystem::path &path,
                            struct demucsonnx::demucs_model &model,
                            Ort::SessionOptions &session_options)
{
    model.sess = std::make_unique<Ort::Session>(
        model.env, path.c_str(), session_options);
    if (model.sess->GetInputCount() != 2 || model.sess->GetOutputCount() != 2) {
        return false;
    }

    Ort::AllocatorWithDefaultOptions allocator;
    for (std::size_t index = 0; index < 2; ++index) {
        auto name = model.sess->GetInputNameAllocated(index, allocator);
        model.input_names.emplace_back(name.get());
    }
    for (std::size_t index = 0; index < 2; ++index) {
        auto name = model.sess->GetOutputNameAllocated(index, allocator);
        model.output_names.emplace_back(name.get());
    }
    for (const auto &name : model.input_names) model.input_names_ptrs.push_back(name.c_str());
    for (const auto &name : model.output_names) model.output_names_ptrs.push_back(name.c_str());

    const auto frequency_shape = model.sess->GetOutputTypeInfo(0)
        .GetTensorTypeAndShapeInfo().GetShape();
    const auto time_shape = model.sess->GetOutputTypeInfo(1)
        .GetTensorTypeAndShapeInfo().GetShape();
    if (frequency_shape.size() != 5 || time_shape.size() != 4 ||
        frequency_shape[1] <= 0 || frequency_shape[1] != time_shape[1]) {
        return false;
    }
    model.nb_sources = static_cast<int>(frequency_shape[1]);
    return true;
}

void RunONNXInference(
    struct demucsonnx::demucs_model &model,
    struct demucsonnx::demucs_segment_buffers &buffers
) {
    // Run the model
    model.sess->Run(
        Ort::RunOptions{nullptr},
        model.input_names_ptrs.data(),
        buffers.input_tensors.data(),
        buffers.input_tensors.size(),
        model.output_names_ptrs.data(),
        buffers.output_tensors.data(),
        model.output_names_ptrs.size()
    );
}

// run core demucs inference using onnx
void demucsonnx::model_inference(
    struct demucsonnx::demucs_model &model,
    struct demucsonnx::demucs_segment_buffers &buffers,
    struct demucsonnx::stft_buffers &stft_buf)
{
    // let's get a stereo complex spectrogram first
    demucsonnx::stft(stft_buf, buffers.padded_mix, buffers.z);

    // x = mag = z.abs(), but for CaC we're simply stacking the complex
    // spectrogram along the channel dimension

    // prepare frequency branch input by copying buffers.z into input_tensors[1]
    float *x_onnx_data = buffers.input_tensors[1].GetTensorMutableData<float>();

    const Eigen::Index z_loop_dim_0 = buffers.z.dimension(0);

    // limiting to j-1 because we're dropping 2049 to 2048 bins
    const Eigen::Index z_loop_dim_1 = buffers.z.dimension(1) - 1;

    // we're also dropping 2 bins from start and end i.e. the 2:2+le removal in python
    const Eigen::Index z_loop_dim_2 = buffers.z.dimension(2) - 4;

    for (Eigen::Index i = 0; i < z_loop_dim_0; ++i)
    {
        for (Eigen::Index j = 0; j < z_loop_dim_1; ++j)
        {
            for (Eigen::Index k = 0; k < z_loop_dim_2; ++k)
            {
                const Eigen::Index real_index =
                    2 * i * z_loop_dim_1 * z_loop_dim_2 + j * z_loop_dim_2 + k;
                const Eigen::Index imag_index =
                    (2 * i + 1) * z_loop_dim_1 * z_loop_dim_2 + j * z_loop_dim_2 + k;
                x_onnx_data[real_index] = buffers.z(i, j, k + 2).real();
                x_onnx_data[imag_index] = buffers.z(i, j, k + 2).imag();
            }
        }
    }

    // prepare time branch input by copying buffers.mix into  input_tensors[0]
    float *xt_onnx_data = buffers.input_tensors[0].GetTensorMutableData<float>();

    for (int i = 0; i < buffers.padded_mix.rows(); ++i)
    {
        for (int j = 0; j < buffers.segment_samples; ++j)
        {
            // calculate destination index, simple row major calculation
            // given the onnx shape of (1, 2, segment_samples)
            int dest_index = i * buffers.segment_samples + j;
            xt_onnx_data[dest_index] = buffers.padded_mix(i, j + buffers.pad);
        }
    }

    // now we have the stft, apply the core demucs inference
    // (where we removed the stft/istft to successfully convert to ONNX)
    RunONNXInference(model, buffers);

    int nb_out_sources = model.nb_sources;

    // nb_sources sources, 2 channels, N samples
    std::vector<Eigen::MatrixXf> xt_3d(
        nb_out_sources,
        Eigen::MatrixXf(2, buffers.segment_samples)
    );

    // distribute the channels of buffers.x into x_4d
    // in pytorch it's (16, 2048, 336) i.e. (chan, freq, time)
    // then apply `.view(4, -1, freq, time)

    // Map output onnx tensors
    float* xt_out_data = buffers.output_tensors[1].GetTensorMutableData<float>();
    float* x_out_data = buffers.output_tensors[0].GetTensorMutableData<float>();

    for (int s = 0; s < nb_out_sources; ++s)
    { // loop over 4 sources
        for (int i = 0; i < 2; ++i)
        {
            for (int j = 0; j < buffers.segment_samples; ++j)
            {
                int index = s * 2 * buffers.segment_samples + i * buffers.segment_samples + j;
                xt_3d[s](i, j) = xt_out_data[index];
            }
        }
    }

    // If `cac` is True, `m` is actually a full spectrogram and `z` is ignored.
    // undo complex-as-channels by splitting the 2nd dim of x_4d into (2, 2)
    for (int source = 0; source < nb_out_sources; ++source)
    {
        Eigen::Tensor3dXcf z_target = Eigen::Tensor3dXcf(
            2, buffers.z.dimension(1), buffers.z.dimension(2));

        // in the CaC case, we're simply unstacking the complex
        // spectrogram from the channel dimension
        for (Eigen::Index i = 0; i < z_loop_dim_0; ++i)
        {
            for (Eigen::Index j = 0; j < z_loop_dim_1; ++j)
            {
                for (Eigen::Index k = 0; k < z_loop_dim_2; ++k)
                {
                    const Eigen::Index real_index = source * 2 * z_loop_dim_0 * z_loop_dim_1 * z_loop_dim_2 + 2 * i * z_loop_dim_1 * z_loop_dim_2 + j * z_loop_dim_2 + k;
                    const Eigen::Index imag_index = source * 2 * z_loop_dim_0 * z_loop_dim_1 * z_loop_dim_2 +  (2 * i + 1) * z_loop_dim_1 * z_loop_dim_2 + j * z_loop_dim_2 + k;
                    z_target(i, j, k + 2) =
                        std::complex<float>(x_out_data[real_index],
                                            x_out_data[imag_index]);
                }
                // set the first two and last two bins to zero
                z_target(i, j, 0) = std::complex<float>(0.0f, 0.0f);
                z_target(i, j, 1) = std::complex<float>(0.0f, 0.0f);
                z_target(i, j, buffers.z.dimension(2) - 1) = std::complex<float>(0.0f, 0.0f);
                z_target(i, j, buffers.z.dimension(2) - 2) = std::complex<float>(0.0f, 0.0f);
            }

            // set the entire slice along the last bin to zero
            for (int k = 0; k < buffers.z.dimension(2); ++k)
            {
                z_target(i, buffers.z.dimension(1) - 1, k) = std::complex<float>(0.0f, 0.0f);
            }
        }

        Eigen::MatrixXf padded_waveform = Eigen::MatrixXf(2, buffers.padded_segment_samples);

        demucsonnx::istft(stft_buf, z_target, padded_waveform);

        // copy target waveform into all 4 dims of targets_out
        // summing with xt_3d in the process to merge the time and frequency branches
        for (int j = 0; j < 2; ++j)
        {
            for (int k = 0; k < buffers.segment_samples; ++k)
            {
                buffers.targets_out(source, j, k) = padded_waveform(j, buffers.pad + k) + xt_3d[source](j, k);
            }
        }
    }
}
