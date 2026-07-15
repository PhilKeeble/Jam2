#include "track_take_recorder.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>

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

struct TrackTakeRecorder::SampleQueue {
    explicit SampleQueue(std::size_t capacity) : samples(capacity) {}

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

struct TrackTakeRecorder::WavWriter {
    WavWriter(const std::filesystem::path& path, int sample_rate)
    {
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }
        out.open(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("failed to open take WAV: " + path.string());
        }
        write_header(0, sample_rate);
    }

    void write_samples(std::span<const std::int16_t> samples_in)
    {
        for (const std::int16_t sample : samples_in) {
            write_u16(out, static_cast<std::uint16_t>(sample));
        }
        frames += static_cast<std::uint64_t>(samples_in.size());
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

TrackTakeRecorder::TrackTakeRecorder(std::size_t queue_capacity_frames)
    : queue_capacity_frames_(std::max<std::size_t>(queue_capacity_frames, 4096))
    , queue_(std::make_unique<SampleQueue>(queue_capacity_frames_))
{
}

TrackTakeRecorder::~TrackTakeRecorder()
{
    cancel();
    join_writer_if_needed();
    close_writer();
}

bool TrackTakeRecorder::arm(
    std::string take_id,
    const std::filesystem::path& output_path,
    int sample_rate,
    std::string& error)
{
    if (armed_.load(std::memory_order_acquire) || recording_.load(std::memory_order_acquire)) {
        error = "track take recorder is already active";
        return false;
    }
    join_writer_if_needed();
    close_writer();
    if (take_id.empty()) {
        error = "track take id is required";
        return false;
    }
    if (output_path.empty()) {
        error = "track take output WAV is required";
        return false;
    }
    if (sample_rate <= 0) {
        error = "track take sample rate must be positive";
        return false;
    }

    reset_queue();
    try {
        writer_ = std::make_unique<WavWriter>(output_path, sample_rate);
    } catch (const std::exception& ex) {
        error = ex.what();
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = error;
        writer_errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        take_id_ = std::move(take_id);
        output_path_ = output_path;
        sample_rate_ = sample_rate;
        last_error_.clear();
    }
    frames_queued_.store(0, std::memory_order_relaxed);
    frames_written_.store(0, std::memory_order_relaxed);
    dropped_frames_.store(0, std::memory_order_relaxed);
    drop_events_.store(0, std::memory_order_relaxed);
    writer_errors_.store(0, std::memory_order_relaxed);
    start_frame_.store(0, std::memory_order_relaxed);
    stop_frame_.store(0, std::memory_order_relaxed);
    start_requested_.store(false, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);
    writer_stop_.store(false, std::memory_order_release);
    finalized_.store(false, std::memory_order_release);
    completion_pending_.store(false, std::memory_order_release);
    canceled_.store(false, std::memory_order_release);
    armed_.store(true, std::memory_order_release);
    writer_thread_ = std::thread([this] { writer_loop(); });
    return true;
}

bool TrackTakeRecorder::start_at(std::uint64_t frame, std::string& error)
{
    if (!armed_.load(std::memory_order_acquire)) {
        error = "track take recorder is not armed";
        return false;
    }
    start_frame_.store(frame, std::memory_order_release);
    start_requested_.store(true, std::memory_order_release);
    return true;
}

bool TrackTakeRecorder::stop_at(std::uint64_t frame, std::string& error)
{
    if (!armed_.load(std::memory_order_acquire) && !recording_.load(std::memory_order_acquire)) {
        error = "track take recorder is not active";
        return false;
    }
    stop_frame_.store(frame, std::memory_order_release);
    stop_requested_.store(true, std::memory_order_release);
    return true;
}

void TrackTakeRecorder::cancel() noexcept
{
    armed_.store(false, std::memory_order_release);
    recording_.store(false, std::memory_order_release);
    canceled_.store(true, std::memory_order_release);
    finalized_.store(true, std::memory_order_release);
    completion_pending_.store(true, std::memory_order_release);
    writer_stop_.store(true, std::memory_order_release);
}

void TrackTakeRecorder::record(std::uint64_t audio_frame_start, std::span<const std::int32_t> input) noexcept
{
    if (!armed_.load(std::memory_order_acquire) || input.empty() || !start_requested_.load(std::memory_order_acquire)) {
        return;
    }

    const std::uint64_t block_start = audio_frame_start;
    const std::uint64_t block_end = block_start + static_cast<std::uint64_t>(input.size());
    const std::uint64_t start_frame = start_frame_.load(std::memory_order_acquire);
    if (!recording_.load(std::memory_order_acquire) && block_end <= start_frame) {
        return;
    }

    std::uint64_t copy_start = block_start;
    if (!recording_.load(std::memory_order_acquire)) {
        recording_.store(true, std::memory_order_release);
        copy_start = std::max(copy_start, start_frame);
    }

    std::uint64_t copy_end = block_end;
    const bool stopping = stop_requested_.load(std::memory_order_acquire);
    const std::uint64_t stop_frame = stop_frame_.load(std::memory_order_acquire);
    if (stopping) {
        if (stop_frame <= block_start) {
            finalize_from_audio_thread(stop_frame);
            return;
        }
        copy_end = std::min(copy_end, stop_frame);
    }
    if (copy_end > copy_start) {
        const std::size_t offset = static_cast<std::size_t>(copy_start - block_start);
        const std::size_t count = static_cast<std::size_t>(copy_end - copy_start);
        if (queue_->available_write() < count) {
            dropped_frames_.fetch_add(count, std::memory_order_relaxed);
            drop_events_.fetch_add(1, std::memory_order_relaxed);
        } else {
            const std::size_t pushed = queue_->push(input.subspan(offset, count));
            frames_queued_.fetch_add(pushed, std::memory_order_relaxed);
            if (pushed < count) {
                dropped_frames_.fetch_add(count - pushed, std::memory_order_relaxed);
                drop_events_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (stopping && block_end >= stop_frame) {
        finalize_from_audio_thread(stop_frame);
    }
}

TrackTakeRecorderSnapshot TrackTakeRecorder::snapshot() const noexcept
{
    TrackTakeRecorderSnapshot out;
    out.armed = armed_.load(std::memory_order_acquire);
    out.recording = recording_.load(std::memory_order_acquire);
    out.finalized = finalized_.load(std::memory_order_acquire);
    out.start_frame = start_frame_.load(std::memory_order_acquire);
    out.stop_frame = stop_frame_.load(std::memory_order_acquire);
    out.frames_queued = frames_queued_.load(std::memory_order_relaxed);
    out.frames_written = frames_written_.load(std::memory_order_relaxed);
    out.dropped_frames = dropped_frames_.load(std::memory_order_relaxed);
    out.drop_events = drop_events_.load(std::memory_order_relaxed);
    out.writer_errors = writer_errors_.load(std::memory_order_relaxed);
    out.queue_depth_frames = queue_ ? queue_->depth() : 0;
    out.queue_capacity_frames = queue_capacity_frames_;
    return out;
}

TrackTakeRecorderStats TrackTakeRecorder::stats() const
{
    const auto fixed = snapshot();
    TrackTakeRecorderStats out;
    out.armed = fixed.armed;
    out.recording = fixed.recording;
    out.finalized = fixed.finalized;
    out.start_frame = fixed.start_frame;
    out.stop_frame = fixed.stop_frame;
    out.frames_queued = fixed.frames_queued;
    out.frames_written = fixed.frames_written;
    out.dropped_frames = fixed.dropped_frames;
    out.drop_events = fixed.drop_events;
    out.writer_errors = fixed.writer_errors;
    out.queue_depth_frames = fixed.queue_depth_frames;
    out.queue_capacity_frames = fixed.queue_capacity_frames;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        out.take_id = take_id_;
        out.output_path = output_path_.string();
        out.last_error = last_error_;
        out.sample_rate = sample_rate_;
    }
    return out;
}

TrackTakeRecorderCompletion TrackTakeRecorder::consume_completion()
{
    if (!completion_pending_.load(std::memory_order_acquire)) {
        return {};
    }
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
    close_writer();
    if (!completion_pending_.exchange(false, std::memory_order_acq_rel)) {
        return {};
    }
    TrackTakeRecorderCompletion out;
    out.available = true;
    out.ok = !canceled_.load(std::memory_order_acquire) && writer_errors_.load(std::memory_order_relaxed) == 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        out.take_id = take_id_;
        out.output_path = output_path_.string();
        out.error = last_error_;
        out.sample_rate = sample_rate_;
    }
    out.start_frame = start_frame_.load(std::memory_order_acquire);
    out.stop_frame = stop_frame_.load(std::memory_order_acquire);
    out.frames_written = frames_written_.load(std::memory_order_relaxed);
    out.dropped_frames = dropped_frames_.load(std::memory_order_relaxed);
    out.writer_errors = writer_errors_.load(std::memory_order_relaxed);
    armed_.store(false, std::memory_order_release);
    recording_.store(false, std::memory_order_release);
    finalized_.store(false, std::memory_order_release);
    return out;
}

void TrackTakeRecorder::writer_loop() noexcept
{
    std::vector<std::int16_t> buffer(4096);
    while (true) {
        const std::size_t count = queue_->pop(std::span<std::int16_t>(buffer.data(), buffer.size()));
        if (count > 0) {
            try {
                writer_->write_samples(std::span<const std::int16_t>(buffer.data(), count));
                frames_written_.fetch_add(count, std::memory_order_relaxed);
            } catch (const std::exception& ex) {
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    last_error_ = ex.what();
                }
                writer_errors_.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        if (writer_stop_.load(std::memory_order_acquire) && queue_->depth() == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    close_writer();
    finalized_.store(true, std::memory_order_release);
    completion_pending_.store(true, std::memory_order_release);
}

void TrackTakeRecorder::reset_queue() noexcept
{
    if (queue_) {
        queue_->reset();
    }
}

void TrackTakeRecorder::close_writer() noexcept
{
    if (writer_) {
        writer_->close(sample_rate_);
        writer_.reset();
    }
}

void TrackTakeRecorder::finalize_from_audio_thread(std::uint64_t frame) noexcept
{
    stop_frame_.store(frame, std::memory_order_release);
    armed_.store(false, std::memory_order_release);
    recording_.store(false, std::memory_order_release);
    writer_stop_.store(true, std::memory_order_release);
}

void TrackTakeRecorder::join_writer_if_needed()
{
    if (writer_thread_.joinable()) {
        writer_stop_.store(true, std::memory_order_release);
        writer_thread_.join();
    }
}

} // namespace jam2::audio
