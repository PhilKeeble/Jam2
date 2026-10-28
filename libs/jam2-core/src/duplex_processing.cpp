#include "duplex_processing.hpp"
#include "recording_calibration.hpp"
#include <stdexcept>

namespace jam2::audio::device_processing {
namespace processing = device_processing;

void prepare_duplex_processing(DuplexProcessingState& state, std::size_t frames,
    std::size_t inputs, double rate)
{
    if (frames == 0 || frames > 65536 || inputs > 1024 || rate <= 0)
        throw std::invalid_argument("invalid duplex processing capacity");
    state.buffer_size = static_cast<long>(frames);
    state.sample_rate = rate;
    state.capture_scratch.resize(frames);
    state.input_source_pointers.resize(inputs);
    state.input_peak_scratch.resize(inputs);
    state.input_downmix.configure(inputs, rate, frames);
    state.playback_scratch.resize(frames);
    state.playback_resampler_scratch.resize(frames * 2U + 2U);
    state.recorder_my_input_scratch.resize(frames);
    state.recorder_their_input_scratch.resize(frames);
    state.recorder_inputs_mix_scratch.resize(frames);
    state.recorder_metronome_scratch.resize(frames);
    state.recorder_prepared_scratch.resize(frames);
    state.metronome_wave_bank.prepare(rate);
    if (state.control) state.control->input_downmix_selected_channels.store(
        static_cast<int>(inputs), std::memory_order_relaxed);
}

void process_duplex_block(DuplexProcessingState& state,
    std::span<const std::int32_t* const> inputs, std::size_t frames,
    bool renderOutput, DeviceClockSample clock, bool measureCallback) noexcept
{
    auto* context = &state;
    if (frames == 0 || frames > state.capture_scratch.size()) return;
    context->buffer_size = static_cast<long>(frames);
    const std::uint64_t callback_start_us = processing::callback_now_us();
    processing::publish_callback_begin(
        context->control,
        context->callback_generation);
    processing::publish_device_clock(context->control,
        clock.time_us != 0 ? clock : DeviceClockSample{context->engine_frame_counter, callback_start_us});
    if (measureCallback) processing::observe_callback_interval(
        context->callback_intervals,
        callback_start_us,
        static_cast<std::size_t>(context->buffer_size),
        context->sample_rate);
    const bool network_capture_enabled = context->control != nullptr &&
        prepare_network_capture_callback(
            *context->control,
            *context->capture,
            context->engine_frame_counter);
    const bool track_take_armed = context->track_take_recorder != nullptr &&
        context->track_take_recorder->armed();
    const bool jam_recording_active = context->recorder != nullptr &&
        context->recorder->active();
    const std::int32_t track_take_options = track_take_armed && context->control != nullptr
        ? context->control->track_take_options.load(std::memory_order_relaxed)
        : 0;
    const auto track_take_source = static_cast<TrackTakeSource>(
        track_take_options & 0xff);
    const bool current_jam_take = track_take_armed &&
        track_take_source == TrackTakeSource::CurrentJam;
    const bool prepared_stem_required = current_jam_take &&
        (track_take_options & kTrackTakeIncludePrepared) != 0;
    const bool metronome_stem_required = jam_recording_active ||
        (current_jam_take &&
         (track_take_options & kTrackTakeIncludeMetronome) != 0);
    const bool recording_scratch_required = track_take_armed || jam_recording_active;
    if (recording_scratch_required &&
        context->recorder_my_input_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
        std::fill(
            context->recorder_my_input_scratch.begin(),
            context->recorder_my_input_scratch.begin() + context->buffer_size,
            0);
    }

    const int test_input_mode = context->control != nullptr ?
        context->control->test_input_mode.load(std::memory_order_relaxed) :
        0;
    if (test_input_mode != 0 &&
        context->capture_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
        auto generated = std::span<std::int32_t>(context->capture_scratch.data(), static_cast<std::size_t>(context->buffer_size));
        processing::fill_test_input(
            context->control,
            context->sample_rate,
            context->test_input_sample_counter,
            generated);
        auto* source_router = context->control != nullptr ?
            context->control->input_source_router : nullptr;
        if (source_router != nullptr) {
            std::fill(
                context->input_source_pointers.begin(),
                context->input_source_pointers.end(),
                generated.data());
            const bool routed = source_router->process(
                std::span<const std::int32_t* const>(
                    context->input_source_pointers.data(),
                    context->input_source_pointers.size()),
                generated.size(),
                context->engine_frame_counter,
                context->sample_rate,
                generated);
            if (!routed) std::fill(generated.begin(), generated.end(), 0);
        }
        if (context->control != nullptr) {
            if (source_router != nullptr) {
                processing::observe_input_peak_value(
                    context->control, source_router->last_peak_ppm());
            } else {
                processing::observe_input_peaks(context->control, generated);
            }
        }
        if (network_capture_enabled) {
            push_network_capture_callback(
                *context->control, *context->capture, generated, callback_start_us);
        }
        if (context->control != nullptr && context->pitch != nullptr) {
            push_pitch_analysis_callback(*context->control, *context->pitch, generated);
        }
        if (recording_scratch_required &&
            context->recorder_my_input_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
            std::copy(generated.begin(), generated.end(), context->recorder_my_input_scratch.begin());
        }
    } else if (!inputs.empty() &&
        inputs[0] != nullptr &&
        context->capture_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
        std::span<const std::int32_t> captured_input;
        auto* source_router = context->control != nullptr ?
            context->control->input_source_router : nullptr;
        if (source_router != nullptr) {
            for (std::size_t channel = 0; channel < inputs.size(); ++channel) {
                const std::int32_t* input_info = inputs[channel];
                context->input_source_pointers[channel] = input_info;
            }
            if (auto* calibration = context->control->recording_calibration) {
                const auto channel = calibration->input_channel();
                const auto* samples = channel < context->input_source_pointers.size()
                    ? context->input_source_pointers[channel] : nullptr;
                calibration->capture_input(samples ? std::span<const std::int32_t>(samples,
                    static_cast<std::size_t>(context->buffer_size)) : std::span<const std::int32_t>{});
            }
            const bool routed = source_router->process(
                std::span<const std::int32_t* const>(context->input_source_pointers.data(),
                    context->input_source_pointers.size()),
                static_cast<std::size_t>(context->buffer_size),
                context->engine_frame_counter,
                context->sample_rate,
                std::span<std::int32_t>(context->capture_scratch.data(),
                    static_cast<std::size_t>(context->buffer_size)));
            if (!routed) std::fill_n(context->capture_scratch.begin(),
                static_cast<std::size_t>(context->buffer_size), 0);
            captured_input = std::span<const std::int32_t>(context->capture_scratch.data(),
                static_cast<std::size_t>(context->buffer_size));
            if (network_capture_enabled) {
                push_network_capture_callback(
                    *context->control, *context->capture, captured_input, callback_start_us);
            }
        } else if (inputs.size() == 1) {
            const auto* input = inputs[0];
            captured_input = std::span<const std::int32_t>(input, static_cast<std::size_t>(context->buffer_size));
            std::copy(captured_input.begin(), captured_input.end(), context->capture_scratch.begin());
            if (network_capture_enabled) {
                push_network_capture_callback(
                    *context->control, *context->capture, captured_input, callback_start_us);
            }
        } else {
            std::fill(
                context->input_peak_scratch.begin(),
                context->input_peak_scratch.end(),
                0.0);
            for (std::size_t channel = 0; channel < inputs.size(); ++channel) {
                const std::int32_t* input_info = inputs[channel];
                if (input_info == nullptr) {
                    continue;
                }
                const auto* input = input_info;
                double peak = 0.0;
                for (long frame = 0; frame < context->buffer_size; ++frame) {
                    peak = (std::max)(
                        peak,
                        std::abs(static_cast<double>(input[frame]) / 2147483648.0));
                }
                context->input_peak_scratch[channel] = peak;
            }
            context->input_downmix.beginBlock(
                std::span<const double>(
                    context->input_peak_scratch.data(),
                    context->input_peak_scratch.size()));
            if (context->control != nullptr) {
                publish_downmix_diagnostics(
                    *context->control, context->input_downmix);
            }
            for (long i = 0; i < context->buffer_size; ++i) {
                double sum = 0.0;
                for (std::size_t channel = 0; channel < inputs.size(); ++channel) {
                    const std::int32_t* input_info = inputs[channel];
                    if (input_info == nullptr) {
                        continue;
                    }
                    const auto* input = input_info;
                    sum += static_cast<double>(input[i]) *
                        context->input_downmix.weightAt(
                            channel, static_cast<std::size_t>(i));
                }
                const double mixed = sum * context->input_downmix.normalizationAt(
                    static_cast<std::size_t>(i));
                context->capture_scratch[static_cast<std::size_t>(i)] =
                    static_cast<std::int32_t>(std::clamp(
                        mixed, -2147483648.0, 2147483647.0));
            }
            captured_input = std::span<const std::int32_t>(
                context->capture_scratch.data(),
                static_cast<std::size_t>(context->buffer_size));
            if (network_capture_enabled) {
                push_network_capture_callback(
                    *context->control, *context->capture, captured_input, callback_start_us);
            }
        }
        if (context->control != nullptr && context->pitch != nullptr) {
            push_pitch_analysis_callback(*context->control, *context->pitch, captured_input);
        }
        if (context->control != nullptr) {
            if (source_router != nullptr) {
                processing::observe_input_peak_value(
                    context->control, source_router->last_peak_ppm());
            } else {
                processing::observe_input_peaks(context->control, captured_input);
            }
        }
        if (recording_scratch_required &&
            context->recorder_my_input_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
            const bool selected = source_router != nullptr && source_router->copy_recording_source(
                static_cast<std::size_t>(context->buffer_size),
                std::span<std::int32_t>(context->recorder_my_input_scratch.data(),
                    static_cast<std::size_t>(context->buffer_size)));
            if (!selected)
                std::copy(captured_input.begin(), captured_input.end(), context->recorder_my_input_scratch.begin());
        }
    }

    if (inputs.empty() && test_input_mode == 0) {
        auto captured = std::span<std::int32_t>(context->capture_scratch.data(), frames);
        std::fill(captured.begin(), captured.end(), 0);
        auto* router = context->control ? context->control->input_source_router : nullptr;
        if (router && !router->process({}, frames, context->engine_frame_counter,
                context->sample_rate, captured)) std::fill(captured.begin(), captured.end(), 0);
        if (context->control) {
            if (router) processing::observe_input_peak_value(context->control, router->last_peak_ppm());
            else processing::observe_input_peaks(context->control, captured);
        }
        if (network_capture_enabled) push_network_capture_callback(
            *context->control, *context->capture, captured, callback_start_us);
        if (context->control && context->pitch)
            push_pitch_analysis_callback(*context->control, *context->pitch, captured);
        if (recording_scratch_required) {
            const bool selected = router && router->copy_recording_source(frames,
                std::span<std::int32_t>(context->recorder_my_input_scratch.data(), frames));
            if (!selected) std::copy(captured.begin(), captured.end(), context->recorder_my_input_scratch.begin());
        }
    }

    if (renderOutput &&
        context->playback_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
        auto* mono = context->playback_scratch.data();
        const bool network_playback_enabled = context->control != nullptr &&
            context->control->network_playback_enabled.load(std::memory_order_acquire);
        if (!network_playback_enabled) {
            context->playback_prefilled.store(false, std::memory_order_relaxed);
            context->playback->pop(std::span<std::int32_t>{}, false);
            context->playback_resampler.reset();
            context->control->playback_ratio_applied_ppm.store(1000000, std::memory_order_relaxed);
            context->control->playback_ratio_ramping.store(false, std::memory_order_relaxed);
            std::fill(mono, mono + context->buffer_size, 0);
        } else if (!context->playback_prefilled.load(std::memory_order_relaxed)) {
            if (context->playback->available_read() >= context->playback_prefill_frames) {
                context->playback_prefilled.store(true, std::memory_order_relaxed);
            } else {
                std::fill(mono, mono + context->buffer_size, 0);
            }
        }
        auto playback = std::span<std::int32_t>(mono, static_cast<std::size_t>(context->buffer_size));
        if (network_playback_enabled && context->playback_prefilled.load(std::memory_order_relaxed)) {
            processing::pop_resampled_playback(
                context->playback,
                context->control,
                context->playback_resampler,
                context->playback_resampler_scratch,
                playback);
            processing::apply_remote_level(context->control, playback);
        }
        if (context->control != nullptr) {
            context->control->network_playback_enabled_applied.store(
                network_playback_enabled,
                std::memory_order_release);
            processing::observe_shared_peak(
                context->control->remote_peak_ppm,
                context->control->gui_remote_peak_ppm,
                playback);
        }
        if (recording_scratch_required &&
            context->recorder_their_input_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
            std::copy(playback.begin(), playback.end(), context->recorder_their_input_scratch.begin());
        }
        processing::mix_local_monitor(
            context->control,
            playback,
            std::span<const std::int32_t>(
                context->capture_scratch.data(),
                std::min<std::size_t>(context->capture_scratch.size(), playback.size())));
        const std::uint64_t audio_frame_start = context->engine_frame_counter;
        processing::mix_prepared_source(
            context->control,
            playback,
            audio_frame_start,
            prepared_stem_required
                ? std::span<std::int32_t>(
                    context->recorder_prepared_scratch.data(),
                    std::min<std::size_t>(
                        context->recorder_prepared_scratch.size(), playback.size()))
                : std::span<std::int32_t>{});
        processing::mix_metronome_click(
            context->control,
            context->sample_rate,
            context->engine_frame_counter,
            context->metronome_beat_index,
            playback,
            metronome_stem_required
                ? std::span<std::int32_t>(
                    context->recorder_metronome_scratch.data(),
                    std::min<std::size_t>(
                        context->recorder_metronome_scratch.size(), playback.size()))
                : std::span<std::int32_t>{},
            &context->metronome_wave_bank);
        processing::apply_output_level(context->control, playback);
        if (context->control != nullptr) {
            processing::observe_output_peak(context->control, playback);
        }
        if (track_take_armed &&
            context->recorder_my_input_scratch.size() >= playback.size() &&
            context->recorder_their_input_scratch.size() >= playback.size() &&
            context->recorder_inputs_mix_scratch.size() >= playback.size()) {
            if (track_take_source == TrackTakeSource::CurrentJam) {
                for (std::size_t index = 0; index < playback.size(); ++index) {
                    std::int32_t sample = processing::mix_i32_samples(
                        context->recorder_my_input_scratch[index],
                        context->recorder_their_input_scratch[index]);
                    if ((track_take_options & kTrackTakeIncludePrepared) != 0 &&
                        context->recorder_prepared_scratch.size() >= playback.size()) {
                        sample = processing::mix_i32_samples(
                            sample, context->recorder_prepared_scratch[index]);
                    }
                    if ((track_take_options & kTrackTakeIncludeMetronome) != 0 &&
                        context->recorder_metronome_scratch.size() >= playback.size()) {
                        sample = processing::mix_i32_samples(
                            sample, context->recorder_metronome_scratch[index]);
                    }
                    context->recorder_inputs_mix_scratch[index] = sample;
                }
                context->track_take_recorder->record(
                    audio_frame_start,
                    std::span<const std::int32_t>(
                        context->recorder_inputs_mix_scratch.data(), playback.size()));
            } else {
                const std::uint64_t compensation = context->control != nullptr
                    ? input_take_compensation_frames(*context->control) : 0;
                const std::uint64_t capture_frame_start = audio_frame_start > compensation
                    ? audio_frame_start - compensation
                    : 0ULL;
                context->track_take_recorder->record(
                    capture_frame_start,
                    std::span<const std::int32_t>(
                        context->recorder_my_input_scratch.data(), playback.size()));
            }
        }
        if (jam_recording_active &&
            context->recorder_inputs_mix_scratch.size() >= playback.size() &&
            context->recorder_my_input_scratch.size() >= playback.size() &&
            context->recorder_their_input_scratch.size() >= playback.size() &&
            context->recorder_metronome_scratch.size() >= playback.size()) {
            for (std::size_t i = 0; i < playback.size(); ++i) {
                context->recorder_inputs_mix_scratch[i] = processing::mix_i32_samples(
                    context->recorder_my_input_scratch[i],
                    context->recorder_their_input_scratch[i]);
            }
            context->recorder->record(RecordBlock{
                audio_frame_start,
                playback,
                std::span<const std::int32_t>(context->recorder_my_input_scratch.data(), playback.size()),
                std::span<const std::int32_t>(context->recorder_their_input_scratch.data(), playback.size()),
                std::span<const std::int32_t>(context->recorder_inputs_mix_scratch.data(), playback.size()),
                std::span<const std::int32_t>(context->recorder_metronome_scratch.data(), playback.size()),
            });
        }
        if (context->control && context->control->recording_calibration) {
            context->control->recording_calibration->render(playback,
                static_cast<std::uint64_t>(context->control->input_latency_frames.load(std::memory_order_relaxed)) +
                context->control->output_latency_frames.load(std::memory_order_relaxed));
        }
    }

    context->engine_frame_counter += static_cast<std::uint64_t>(context->buffer_size);
    if (context->control != nullptr) {
        context->control->engine_frame_counter.store(context->engine_frame_counter, std::memory_order_release);
    }
    processing::publish_callback_end(
        context->control,
        context->callback_generation);
    context->callbacks.fetch_add(1, std::memory_order_relaxed);
    if (measureCallback) processing::observe_callback_work(
        context->callback_intervals,
        callback_start_us,
        processing::callback_now_us());
}

} // namespace jam2::audio::device_processing
