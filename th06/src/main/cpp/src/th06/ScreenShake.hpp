#pragma once

// Screen shake: each shake is a job that runs once per frame after the bullets. It moves
// the playfield's screen origin by up to `offset` pixels (shrinking its size by the same
// amount), picking for x and y independently, with one random draw each, whether to
// shift, shrink or do nothing. Several shakes can run at once; the last one processed in
// a frame wins, and any one that ends resets the playfield.

#include <cstdint>
#include <vector>

namespace touhou::th06 {

struct World;

class ScreenShake {
  public:
    // A shake of `length` frames whose offset goes linearly from `from` to `to`.
    void add(int length, float from, float to);
    void update(World& world);
    void clear(World& world);
    int activeCount() const { return static_cast<int>(jobs_.size()); }

  private:
    struct Job {
        int length = 0;
        float from = 0, to = 0;
        int timer = 0;
    };
    std::vector<Job> jobs_;
};

}  // namespace touhou::th06
