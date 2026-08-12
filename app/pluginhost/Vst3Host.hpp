#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace jam2::pluginhost {

struct PluginDescription {
    std::string path;
    std::string class_id;
    std::string name;
    std::string vendor;
    std::string version;
    std::string subcategories;
};

struct FactoryClassDescription {
    std::string category;
    std::string class_id;
    std::string name;
    std::string vendor;
    std::string version;
    std::string subcategories;
};

struct MidiMessage {
    std::uint16_t sample_offset = 0;
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
};

std::vector<PluginDescription> scan_vst3(const std::string& path);
// Diagnostic enumeration includes non-audio factory classes so a malformed or
// unusual installation can be distinguished from a module-load failure.
std::vector<FactoryClassDescription> scan_vst3_factory_classes(const std::string& path);

class Vst3Instance final {
public:
    Vst3Instance();
    ~Vst3Instance();
    Vst3Instance(const Vst3Instance&) = delete;
    Vst3Instance& operator=(const Vst3Instance&) = delete;

    void load(const std::string& path, const std::string& class_id = {});
    void configure(double sample_rate, std::size_t maximum_frames,
        std::size_t source_input_channels);
    bool process(
        std::span<const float> input_left,
        std::span<const float> input_right,
        std::span<const MidiMessage> midi,
        std::span<float> output_left,
        std::span<float> output_right) noexcept;
    void reset() noexcept;
    bool open_editor() noexcept;
    void close_editor() noexcept;
    void pump_editor() noexcept;
    const std::string& editor_error() const noexcept;

    const PluginDescription& description() const noexcept;
    std::size_t input_channels() const noexcept;
    std::size_t output_channels() const noexcept;
    std::uint32_t latency_samples() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jam2::pluginhost
