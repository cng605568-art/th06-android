#pragma once

// The launcher's display settings (Android) or their desktop defaults.

namespace touhou {

struct DisplayOptions {
    enum Scaling : int { Smooth = 0, Sharp = 1, PixelPerfect = 2 };
    bool portrait = false;  // hold the phone upright: the playfield fills the width in play
    int scaling = Smooth;   // the picture's upscale: bilinear, nearest, or whole multiples
    bool scanlines = false; // a light CRT look: every picture row darkened at its bottom
};

}  // namespace touhou
