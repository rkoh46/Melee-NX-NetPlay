// Host-side evaluation of authored animation curves. No guest memory or callbacks.
// Equations follow doldecomp/melee baselib FObjUpdateAnim and splGetHelmite.
#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace NativeMelee {
enum class Curve { Constant, Linear, Hermite };
struct Segment {
  double start, end;
  float first, last;
  float first_slope, last_slope; // Value per animation frame, not normalized tangents.
  Curve curve;
};

// This consumes decoded authored tracks, not consecutive rendered/simulation poses.
// Sampling never fires HSD KEY events or advances AObj callbacks/gameplay state.
class AnimationTrack {
public:
  explicit AnimationTrack(std::vector<Segment> segments) : m_segments(std::move(segments)) {
    if (m_segments.empty()) throw std::invalid_argument("empty animation track");
    for (size_t i = 0; i < m_segments.size(); ++i) {
      const auto& s = m_segments[i];
      if (!std::isfinite(s.start) || !std::isfinite(s.end) || s.end <= s.start ||
          !std::isfinite(s.end - s.start) ||
          !std::isfinite(s.first) || !std::isfinite(s.last) ||
          !std::isfinite(s.first_slope) || !std::isfinite(s.last_slope) ||
          (i && s.start != m_segments[i-1].end))
        throw std::invalid_argument("invalid or noncontiguous animation segments");
    }
  }

  float Sample(double frame) const {
    if (!std::isfinite(frame)) throw std::invalid_argument("nonfinite animation time");
    if (frame < m_segments.front().start) return m_segments.front().first;
    if (frame >= m_segments.back().end) return m_segments.back().last;
    const auto next = std::upper_bound(m_segments.begin(), m_segments.end(), frame,
        [](double time, const Segment& s) { return time < s.start; });
    const auto& s = *(next - 1);
    const double duration = s.end - s.start;
    const double t = frame - s.start;
    if (s.curve == Curve::Constant) return s.first;
    if (s.curve == Curve::Linear)
      return static_cast<float>(s.first + (s.last - s.first) * (t / duration));
    const double u = t / duration;
    const double u2 = u*u, u3 = u2*u;
    return static_cast<float>((2*u3 - 3*u2 + 1)*s.first +
        (u3 - 2*u2 + u)*duration*s.first_slope +
        (-2*u3 + 3*u2)*s.last + (u3 - u2)*duration*s.last_slope);
  }
private:
  const std::vector<Segment> m_segments;
};
}
