#include "output_recorder.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace jam2::audio {
namespace {

void write_u16(std::ofstream& out, std::uint16_t value)
{
    const std::array<char, 2> bytes{
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8) & 0xffU),
    };
    out.write(bytes.data(), bytes.size());
}

void write_u32(std::ofstream& out, std::uint32_t value)
{
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8) & 0xffU),
        static_cast<char>((value >> 16) & 0xffU),
        static_cast<char>((value >> 24) & 0xffU),
    };
    out.write(bytes.data(), bytes.size());
}

std::int16_t to_i16(std::int32_t sample)
{
    const std::int32_t shifted = sample >> 16;
    return static_cast<std::int16_t>(std::clamp(shifted, -32768, 32767));
}

} // namespace

struct OutputRecorder::StemQueue {
    explicit StemQueue(std::size_t capacity) : samples(capacity) {}

    std::size_t push(std::span<const std::int32_t> input) noexcept
    {
        const std::size_t read = read_index.load(std::memory_order_acquire);
        const std::size_t write = write_index.load(std::memory_order_relaxed);
        const std::size_t used = write - read;
        const std::size_t available = samples.size() > used ? samples.size() - used : 0;
        const std::size_t count = std::min(available, input.size());
        for (std::size_t i = 0; i < count; ++i) {
            samples[(write + i) % samples.size()] = to_i16(input[i]);
        }
        write_index.store(write + count, std::memory_order_release);
        return count;
    }

    std::size_t pop(std::span<std::int16_t> output) noexcept
    {
        const std::size_t write = write_index.load(std::memory_order_acquire);
        const std::size_t read = read_index.load(std::memory_order_relaxed);
        const std::size_t count = std::min(write - read, output.size());
        for (std::size_t i = 0; i < count; ++i) {
            output[i] = samples[(read + i) % samples.size()];
        }
        read_index.store(read + count, std::memory_order_release);
        return count;
    }

    std::size_t depth() const noexcept
    {
        const std::size_t write = write_index.load(std::memory_order_acquire);
        const std::size_t read = read_index.load(std::memory_order_acquire);
        return write >= read ? write - read : 0;
    }

    std::size_t available_write() const noexcept
    {
        const std::size_t used = depth();
        return samples.size() > used ? samples.size() - used : 0;
    }

    void reset() noexcept
    {
        read_index.store(0, std::memory_order_relaxed);
        write_index.store(0, std::memory_order_relaxed);
    }

    std::vector<std::int16_t> samples;
    std::atomic<std::size_t> read_index{0};
    std::atomic<std::size_t> write_index{0};
};

struct OutputRecorder::WavWriter {
    WavWriter(const std::filesystem::path& path, int sample_rate)
    {
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }
        out.open(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("failed to open WAV: " + path.string());
        }
        write_header(0, sample_rate);
    }

    void write_samples(std::span<const std::int16_t> samples)
    {
        for (const std::int16_t sample : samples) {
            write_u16(out, static_cast<std::uint16_t>(sample));
        }
        frames += static_cast<std::uint64_t>(samples.size());
    }

    void close(int sample_rate) noexcept
    {
        if (!out) {
            return;
        }
        const std::uint64_t clamped = std::min<std::uint64_t>(frames, std::numeric_limits<std::uint32_t>::max() / 2U);
        out.seekp(0, std::ios::beg);
        write_header(static_cast<std::uint32_t>(clamped), sample_rate);
        out.close();
    }

    void write_header(std::uint32_t frames_in_file, int sample_rate)
    {
        constexpr std::uint16_t channels = 1;
        constexpr std::uint16_t bits = 16;
        const std::uint32_t data_bytes = frames_in_file * channels * (bits / 8);
        out.write("RIFF", 4);
        write_u32(out, 36U + data_bytes);
        out.write("WAVE", 4);
        out.write("fmt ", 4);
        write_u32(out, 16);
        write_u16(out, 1);
        write_u16(out, channels);
        write_u32(out, static_cast<std::uint32_t>(sample_rate));
        write_u32(out, static_cast<std::uint32_t>(sample_rate * channels * (bits / 8)));
        write_u16(out, channels * (bits / 8));
        write_u16(out, bits);
        out.write("data", 4);
        write_u32(out, data_bytes);
    }

    std::ofstream out;
    std::uint64_t frames = 0;
};

OutputRecorder::OutputRecorder(std::size_t queue_capacity_frames)
    : queue_capacity_frames_(std::max<std::size_t>(queue_capacity_frames, 4096))
{
    for (auto& queue : queues_) {
        queue = std::make_unique<StemQueue>(queue_capacity_frames_);
    }
}

OutputRecorder::~OutputRecorder()
{
    std::string ignored;
    (void)stop(ignored);
}

bool OutputRecorder::start(const std::filesystem::path& folder, int sample_rate, std::string& error)
{
    if (active_.load(std::memory_order_acquire)) {
        error = "recording is already active";
        return false;
    }
    if (writer_thread_.joinable()) {
        writer_stop_.store(true, std::memory_order_release);
        writer_thread_.join();
    }
    if (sample_rate <= 0) {
        error = "recording sample rate must be positive";
        return false;
    }

    close_writers();
    reset_queues();
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        folder_ = folder;
        sample_rate_ = sample_rate;
        last_error_.clear();
    }
    first_frame_set_.store(false, std::memory_order_relaxed);
    start_audio_frame_.store(0, std::memory_order_relaxed);
    stop_audio_frame_.store(0, std::memory_order_relaxed);
    frames_queued_.store(0, std::memory_order_relaxed);
    frames_written_.store(0, std::memory_order_relaxed);
    dropped_frames_.store(0, std::memory_order_relaxed);
    drop_events_.store(0, std::memory_order_relaxed);
    writer_errors_.store(0, std::memory_order_relaxed);

    try {
        for (std::size_t i = 0; i < writers_.size(); ++i) {
            writers_[i] = std::make_unique<WavWriter>(folder / stem_file_names[i], sample_rate);
        }
    } catch (const std::exception& ex) {
        close_writers();
        error = ex.what();
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            last_error_ = error;
        }
        writer_errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    writer_stop_.store(false, std::memory_order_release);
    writer_thread_ = std::thread([this] { writer_loop(); });
    active_.store(true, std::memory_order_release);
    return true;
}

bool OutputRecorder::stop(std::string& error)
{
    active_.store(false, std::memory_order_release);
    writer_stop_.store(true, std::memory_order_release);
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
    close_writers();
    std::lock_guard<std::mutex> lock(error_mutex_);
    if (!last_error_.empty()) {
        error = last_error_;
        return false;
    }
    return true;
}

void OutputRecorder::record(const RecordBlock& block) noexcept
{
    if (!active_.load(std::memory_order_acquire) || block.mix.empty()) {
        return;
    }
    bool expected = false;
    if (first_frame_set_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        start_audio_frame_.store(block.audio_frame_start, std::memory_order_release);
    }
    stop_audio_frame_.store(block.audio_frame_start + static_cast<std::uint64_t>(block.mix.size()), std::memory_order_release);

    const std::array<std::span<const std::int32_t>, static_cast<std::size_t>(RecordStem::Count)> stems{
        block.mix,
        block.my_input,
        block.their_input,
        block.inputs_mix,
        block.metronome,
    };
    std::size_t pushed = block.mix.size();
    for (std::size_t i = 0; i < stems.size(); ++i) {
        if (stems[i].size() != block.mix.size()) {
            dropped_frames_.fetch_add(block.mix.size(), std::memory_order_relaxed);
            drop_events_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (queues_[i]->available_write() < block.mix.size()) {
            dropped_frames_.fetch_add(block.mix.size(), std::memory_order_relaxed);
            drop_events_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    for (std::size_t i = 0; i < stems.size(); ++i) {
        pushed = std::min(pushed, queues_[i]->push(stems[i]));
    }
    if (pushed < block.mix.size()) {
        dropped_frames_.fetch_add(block.mix.size() - pushed, std::memory_order_relaxed);
        drop_events_.fetch_add(1, std::memory_order_relaxed);
    }
    frames_queued_.fetch_add(pushed, std::memory_order_relaxed);
}

OutputRecorderStats OutputRecorder::stats() const
{
    OutputRecorderStats out;
    out.active = active_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        out.folder = folder_.string();
        out.sample_rate = sample_rate_;
        out.last_error = last_error_;
    }
    out.queue_capacity_frames = queue_capacity_frames_;
    out.start_audio_frame = start_audio_frame_.load(std::memory_order_acquire);
    out.stop_audio_frame = stop_audio_frame_.load(std::memory_order_acquire);
    out.frames_queued = frames_queued_.load(std::memory_order_relaxed);
    out.frames_written = frames_written_.load(std::memory_order_relaxed);
    out.dropped_frames = dropped_frames_.load(std::memory_order_relaxed);
    out.drop_events = drop_events_.load(std::memory_order_relaxed);
    out.writer_errors = writer_errors_.load(std::memory_order_relaxed);
    out.queue_depth_frames = queues_[0] ? queues_[0]->depth() : 0;
    return out;
}

void OutputRecorder::writer_loop() noexcept
{
    std::array<std::vector<std::int16_t>, static_cast<std::size_t>(RecordStem::Count)> buffers;
    for (auto& buffer : buffers) {
        buffer.resize(4096);
    }

    while (true) {
        bool wrote_any = false;
        for (std::size_t i = 0; i < queues_.size(); ++i) {
            const std::size_t count = queues_[i]->pop(std::span<std::int16_t>(buffers[i].data(), buffers[i].size()));
            if (count == 0) {
                continue;
            }
            wrote_any = true;
            try {
                writers_[i]->write_samples(std::span<const std::int16_t>(buffers[i].data(), count));
            } catch (const std::exception& ex) {
                {
                    std::lock_guard<std::mutex> lock(error_mutex_);
                    last_error_ = ex.what();
                }
                writer_errors_.fetch_add(1, std::memory_order_relaxed);
            }
            if (i == 0) {
                frames_written_.fetch_add(count, std::memory_order_relaxed);
            }
        }
        if (writer_stop_.load(std::memory_order_acquire)) {
            bool empty = true;
            for (const auto& queue : queues_) {
                empty = empty && queue->depth() == 0;
            }
            if (empty) {
                break;
            }
        }
        if (!wrote_any) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

void OutputRecorder::close_writers() noexcept
{
    for (auto& writer : writers_) {
        if (writer) {
            writer->close(sample_rate_);
            writer.reset();
        }
    }
}

void OutputRecorder::reset_queues()
{
    for (auto& queue : queues_) {
        queue->reset();
    }
}

} // namespace jam2::audio
