#include "output_recorder.hpp"

#include <QDir>
#include <QTemporaryDir>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

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
        require(recorder.start(
                filesystemPath(folder.path()), 48000, error),
            "output recorder did not start");

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
        require(active.stem_signal_frames == expected,
            "recorder stem signal counters did not match PCM16-visible samples");

        require(recorder.stop(error), "output recorder did not stop cleanly");
        const auto stopped = recorder.stats();
        require(!stopped.active && stopped.frames_written == 4 &&
                stopped.stem_signal_frames == expected,
            "final recorder stats lost stem signal counters");
        for (const char* name : jam2::audio::OutputRecorder::stem_file_names) {
            require(std::filesystem::file_size(
                    filesystemPath(folder.path()) / name) > 44,
                "output recorder omitted a finalized stem");
        }

        std::cout << "Jam2 output recorder tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Output recorder test failure: " << error.what() << '\n';
        return 1;
    }
}
