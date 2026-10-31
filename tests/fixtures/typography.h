#pragma once
#include <cstdint>

namespace stream {
using RateCallback = void (*)(std::uint32_t bitrate, void *user);
void notify_rate(RateCallback callback, std::uint32_t bitrate, void *user);
}
