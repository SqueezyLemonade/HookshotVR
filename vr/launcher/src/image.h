// PNG in and out: the art slots (art/*.png) and --screenshot. Built with libpng when the build finds it (HSL_PNG); without
// it the art slots stay on their drawn placeholders and --screenshot is refused. Windows uses its own codec (WIC).
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace img {

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;   // (top row first)
};

bool available();
bool load_png(const std::string &path, Image &out);
// alpha: kept (the icon's cut corners); else written opaque (a screenshot of the window)
bool save_png(const std::string &path, const uint8_t *rgba, int width, int height, bool alpha = false);

}  // namespace img
