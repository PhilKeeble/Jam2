#include "audio_device.hpp"

namespace jam2::audio {

std::vector<DeviceInfo> list_devices()
{
    return {};
}

DeviceProbe probe_device(int, double)
{
    return {};
}

DeviceMeterResult meter_device(int, double, long, int)
{
    return {};
}

DeviceRingResult ring_device(int, double, long, int, std::size_t)
{
    return {};
}

std::unique_ptr<DeviceStream> start_duplex_stream(int, double, long, MonoRingBuffer&, MonoRingBuffer&, std::size_t, StreamControl&)
{
    return {};
}

} // namespace jam2::audio
