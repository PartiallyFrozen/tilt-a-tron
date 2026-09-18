#include "engine/polar.h"

#include <cmath>

#include "engine/gfx.h"
#include "esp_heap_caps.h"

namespace wc {

bool Polar::init()
{
    if (ang_) return true;
    const int n = Gfx::W * Gfx::H;
    ang_ = static_cast<uint16_t *>(heap_caps_malloc(n * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    rad_ = static_cast<uint16_t *>(heap_caps_malloc(n * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    if (!ang_ || !rad_) return false;

    const float c = Gfx::W / 2.0f;
    const float k = 65536.0f / (2.0f * float(M_PI));
    for (int y = 0; y < Gfx::H; y++) {
        const float dy = y + 0.5f - c;
        for (int x = 0; x < Gfx::W; x++) {
            const float dx = x + 0.5f - c;
            float a = std::atan2(dy, dx);
            if (a < 0) a += 2.0f * float(M_PI);
            const int i = y * Gfx::W + x;
            ang_[i] = uint16_t(int(a * k) & 0xFFFF);
            rad_[i] = uint16_t(std::sqrt(dx * dx + dy * dy) * 16.0f);
        }
    }
    return true;
}

}  // namespace wc
