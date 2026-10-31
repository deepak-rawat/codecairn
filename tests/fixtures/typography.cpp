// CodeCairn typography fixture. This is original sample code, not a network library.
// Compare indentation, comment density, declaration emphasis, and preview geometry.
// This comment deliberately extends beyond a narrow viewport to exercise horizontal scrolling.

#include "typography.h"

#ifndef CODECAIRN_SAMPLE
#define CODECAIRN_SAMPLE 1

namespace stream {

typedef struct {
    std::uint32_t initial_rate;
    std::uint32_t minimum_rate;
    std::uint32_t maximum_rate;
} RateConfig;

enum class LinkState {
    Stable,
    Recovering,
    Limited
};

class RateController {
public:
    explicit RateController(RateConfig config) : config_(config) {}

    std::uint32_t update(std::uint32_t observed_rate, double loss) {
        if (loss > 0.10)
            observed_rate = observed_rate * 3 / 4;
        if (observed_rate < config_.minimum_rate)
            return config_.minimum_rate;
        return observed_rate;
    }

    LinkState state() const { return LinkState::Stable; }

private:
    RateConfig config_;
};

// Tabs and spaces remain source characters, not layout substitutions.
static constexpr const char *description = "A compact source browser";

std::uint32_t choose_rate(const RateConfig &config, std::uint32_t measured_rate) {
    RateController controller(config);
    return controller.update(measured_rate, 0.025);
}

void notify_rate(RateCallback callback, std::uint32_t bitrate, void *user) {
    if (callback)
        callback(bitrate, user);
}

} // namespace stream

#endif
