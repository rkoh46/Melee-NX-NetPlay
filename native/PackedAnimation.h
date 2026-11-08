#pragma once
#include <cstdint>
#include <vector>
namespace NativeMelee {
struct PackedTrack {
  int16_t start_frame = 0;
  uint8_t channel = 0, value_format = 0, slope_format = 0;
  std::vector<uint8_t> bytes;
};
// Re-evaluates immutable authored data at an absolute animation time.
// Returns false for an inactive track. Does not execute guest callbacks.
bool SamplePacked(const PackedTrack& track, float frame, float& value);
}
