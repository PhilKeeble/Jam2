#include "dsp.hpp"
#include <unsupported/Eigen/FFT>

namespace {

void stft_inner(struct demucsonnx::stft_buffers &stft_buf,
                Eigen::FFT<float> &cfg);
void istft_inner(struct demucsonnx::stft_buffers &stft_buf,
                 Eigen::FFT<float> &cfg);

// reflect padding
void pad_signal(struct demucsonnx::stft_buffers &stft_buf)
{
    const auto sample_count = static_cast<int>(
        stft_buf.padded_waveform_mono_in.size()) - 2 * stft_buf.pad;
    const int first = stft_buf.pad;
    const int last = first + sample_count - 1;
    for (int index = 0; index < stft_buf.pad; ++index) {
        stft_buf.padded_waveform_mono_in[first - 1 - index] =
            stft_buf.padded_waveform_mono_in[first + 1 + index];
        stft_buf.padded_waveform_mono_in[last + 1 + index] =
            stft_buf.padded_waveform_mono_in[last - 1 - index];
    }
}

Eigen::FFT<float> get_fft_cfg()
{
    Eigen::FFT<float> cfg;

    cfg.SetFlag(Eigen::FFT<float>::Speedy);
    // Demucs and PyTorch exchange the positive-frequency half-spectrum.
    // Let Eigen reconstruct the conjugate half during the real inverse FFT;
    // otherwise stale full-spectrum bins survive between model outputs.
    cfg.SetFlag(Eigen::FFT<float>::HalfSpectrum);

    return cfg;
}

} // namespace

void demucsonnx::stft(
    struct stft_buffers &stft_buf,
    const Eigen::MatrixXf &waveform,
    Eigen::Tensor3dXcf &spec)
{
    // get the fft config
    Eigen::FFT<float> cfg = get_fft_cfg();

    /*****************************************/
    /*  operate on each channel sequentially */
    /*****************************************/

    for (int channel = 0; channel < 2; ++channel)
    {
        Eigen::VectorXf row_vec = waveform.row(channel);

        std::copy_n(row_vec.data(), row_vec.size(),
                    stft_buf.padded_waveform_mono_in.begin() + stft_buf.pad);

        // apply padding equivalent to center padding with center=True
        // in torch.stft:
        // https://pytorch.org/docs/stable/generated/torch.stft.html

        // reflect pads stft_buf.padded_waveform_mono in-place
        pad_signal(stft_buf);

        // does forward fft on stft_buf.padded_waveform_mono, stores spectrum in
        // complex_spec_mono
        stft_inner(stft_buf, cfg);

        for (int i = 0; i < stft_buf.nb_bins; ++i)
        {
            for (int j = 0; j < stft_buf.nb_frames; ++j)
            {
                spec(channel, i, j) = stft_buf.complex_spec_mono[j][i];
            }
        }
    }
}

void demucsonnx::istft(
    struct stft_buffers &stft_buf,
    const Eigen::Tensor3dXcf &spec,
    Eigen::MatrixXf &waveform)
{
    // get the fft config
    Eigen::FFT<float> cfg = get_fft_cfg();

    /*****************************************/
    /*  operate on each channel sequentially */
    /*****************************************/

    for (int channel = 0; channel < 2; ++channel)
    {
        // Populate the nested vectors
        for (int i = 0; i < stft_buf.nb_bins; ++i)
        {
            for (int j = 0; j < stft_buf.nb_frames; ++j)
            {
                stft_buf.complex_spec_mono[j][i] = spec(channel, i, j);
            }
        }

        // does inverse fft on stft_buf.complex_spec_mono, stores waveform in
        // padded_waveform_mono
        istft_inner(stft_buf, cfg);

        // copies waveform_mono into stft_buf.waveform past first pad samples
        waveform.row(channel) = Eigen::Map<Eigen::MatrixXf>(
            stft_buf.padded_waveform_mono_out.data() + stft_buf.pad, 1,
            stft_buf.padded_waveform_mono_out.size() - FFT_WINDOW_SIZE);
    }
}

namespace {

void stft_inner(struct demucsonnx::stft_buffers &stft_buf,
                Eigen::FFT<float> &cfg)
{
    int frame_idx = 0;

    // Loop over the waveform with a stride of hop_size
    for (std::size_t start = 0;
         start <=
         stft_buf.padded_waveform_mono_in.size() - demucsonnx::FFT_WINDOW_SIZE;
         start += demucsonnx::FFT_HOP_SIZE)
    {
        // Apply window and run FFT
        for (int i = 0; i < demucsonnx::FFT_WINDOW_SIZE; ++i)
        {
            stft_buf.windowed_waveform_mono[i] =
                stft_buf.padded_waveform_mono_in[start + i] *
                stft_buf.window[i];
        }
        cfg.fwd(stft_buf.complex_spec_mono[frame_idx],
                stft_buf.windowed_waveform_mono);
        // now scale stft_buf.complex_spec_mono[frame_idx] by 1.0f /
        // sqrt(float(FFT_WINDOW_SIZE)))

        for (int i = 0; i < demucsonnx::FFT_WINDOW_SIZE / 2 + 1; ++i)
        {
            stft_buf.complex_spec_mono[frame_idx][i] *=
                1.0f / sqrt(float(demucsonnx::FFT_WINDOW_SIZE));
        }
        frame_idx++;
    }
}

void istft_inner(struct demucsonnx::stft_buffers &stft_buf,
                 Eigen::FFT<float> &cfg)
{
    // clear padded_waveform_mono
    std::fill(stft_buf.padded_waveform_mono_out.begin(),
              stft_buf.padded_waveform_mono_out.end(), 0.0f);

    // Loop over the input with a stride of (hop_size)
    for (int start = 0; start < stft_buf.nb_frames * demucsonnx::FFT_HOP_SIZE;
         start += demucsonnx::FFT_HOP_SIZE)
    {
        int frame_idx = start / demucsonnx::FFT_HOP_SIZE;
        // undo sqrt(nfft) scaling
        for (int i = 0; i < demucsonnx::FFT_WINDOW_SIZE / 2 + 1; ++i)
        {
            stft_buf.complex_spec_mono[frame_idx][i] *=
                sqrt(float(demucsonnx::FFT_WINDOW_SIZE));
        }
        // Run iFFT
        cfg.inv(stft_buf.windowed_waveform_mono,
                stft_buf.complex_spec_mono[frame_idx]);

        // Apply window and add to output
        for (int i = 0; i < demucsonnx::FFT_WINDOW_SIZE; ++i)
        {
            // This Eigen backend's std::vector real inverse is unscaled. The
            // explicit 1/N below is verified by the native STFT roundtrip test;
            // multiplying the spectrum by sqrt(N) gives PyTorch normalization.
            stft_buf.padded_waveform_mono_out[start + i] +=
                stft_buf.windowed_waveform_mono[i] * stft_buf.window[i] /
                static_cast<float>(demucsonnx::FFT_WINDOW_SIZE) /
                (stft_buf.normalized_window[start + i] + 1e-8f);
        }
    }
}

} // namespace
