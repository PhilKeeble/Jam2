#include "output_recorder.hpp"

#include <QDir>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path filesystemPath(const QString& path)
{
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}

} // namespace

int main()
{
    try {
        QTemporaryDir folder;
        require(folder.isValid(), "could not create recorder test folder");

        jam2::audio::OutputRecorder recorder;
        std::string error;
        require(!recorder.active(),
            "new output recorder reports inactive without taking a stats lock");
        require(recorder.start(
                filesystemPath(folder.path()), 48000, error),
            "output recorder did not start");
        require(recorder.active(),
            "started output recorder publishes callback-side activity");

        constexpr std::int32_t audible = 2 << 16;
        std::array<std::int32_t, 4> mix{audible, 0, audible, 0};
        std::array<std::int32_t, 4> mine{0, audible, 0, 0};
        std::array<std::int32_t, 4> theirs{audible, audible, 0, 0};
        std::array<std::int32_t, 4> inputs{audible, audible, audible, 0};
        std::array<std::int32_t, 4> metronome{0, 0, 0, audible};
        recorder.record({
            100,
            mix,
            mine,
            theirs,
            inputs,
            metronome,
        });

        const auto active = recorder.snapshot();
        const std::array<std::uint64_t, 5> expected{2, 1, 2, 3, 1};
        require(active.active && active.frames_queued == 4,
            "active recorder snapshot omitted its queued block");
        for (std::size_t i = 0; i < expected.size(); ++i) {
            require(active.stem_signal_frames[i] <= expected[i],
                "live recorder signal counters exceeded queued PCM16-visible samples");
        }

        require(recorder.stop(error), "output recorder did not stop cleanly");
        require(!recorder.active(),
            "stopped output recorder clears callback-side activity");
        const auto stopped = recorder.stats();
        require(!stopped.active && stopped.frames_written == 4 &&
                stopped.stem_signal_frames == expected,
            "writer-owned final signal counters did not match PCM16-visible samples");
        for (const char* name : jam2::audio::OutputRecorder::stem_file_names) {
            require(std::filesystem::file_size(
                    filesystemPath(folder.path()) / name) > 44,
                "output recorder omitted a finalized stem");
        }

        QTemporaryDir wrappedFolder;
        require(wrappedFolder.isValid(), "could not create wrapped recorder test folder");
        jam2::audio::OutputRecorder wrappedRecorder(4096);
        require(wrappedRecorder.start(
                filesystemPath(wrappedFolder.path()), 48000, error),
            "wrapped output recorder did not start");
        std::array<std::int32_t, 128> wrappedBlock{};
        for (std::size_t frame = 0; frame < wrappedBlock.size(); ++frame) {
            wrappedBlock[frame] = static_cast<std::int32_t>((frame + 1) << 16);
        }
        for (std::uint64_t block = 0; block < 40; ++block) {
            wrappedRecorder.record({
                block * wrappedBlock.size(),
                wrappedBlock,
                wrappedBlock,
                wrappedBlock,
                wrappedBlock,
                wrappedBlock,
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(wrappedRecorder.stop(error),
            "wrapped output recorder did not stop cleanly");
        const auto wrapped = wrappedRecorder.stats();
        const std::uint64_t wrappedFrames = 40ULL * wrappedBlock.size();
        require(wrapped.frames_written == wrappedFrames &&
                wrapped.frames_queued == wrappedFrames &&
                wrapped.dropped_frames == 0 && wrapped.drop_events == 0 &&
                std::all_of(
                    wrapped.stem_signal_frames.begin(),
                    wrapped.stem_signal_frames.end(),
                    [wrappedFrames](std::uint64_t frames) {
                        return frames == wrappedFrames;
                    }),
            "recorder ring wrap changed samples or dropped queued frames");

        std::cout << "Jam2 output recorder tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Output recorder test failure: " << error.what() << '\n';
        return 1;
    }
}
