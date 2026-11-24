#include <coffeecnc/core/fixed_setter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace coffeecnc {
namespace {

using Point = std::array<double, 2>;

// Symmetric two-parameter least-squares system.
struct Normal {
  double xx{};
  double xy{};
  double yy{};
  Point rhs{};

  void add(Point row, double value) {
    xx += row[0] * row[0];
    xy += row[0] * row[1];
    yy += row[1] * row[1];
    rhs[0] += row[0] * value;
    rhs[1] += row[1] * value;
  }

  [[nodiscard]] auto solve() const -> std::optional<Point> {
    const double det = xx * yy - xy * xy;
    const double trace = xx + yy;
    if (!std::isfinite(det) || !(det > 1e-12 * trace * trace))
      return std::nullopt;
    return Point{(yy * rhs[0] - xy * rhs[1]) / det, (xx * rhs[1] - xy * rhs[0]) / det};
  }
};

struct RadialFit {
  double radius{};
  double sum_squares{};
  double max_residual{};
  Normal update;
};

// For a given centre the least-squares radius is the mean distance. Eliminating the radius
// leaves two unknowns. Subtract the mean of the unit vectors in the centre's derivatives too.
auto radial_fit(std::span<const Point> points, Point centre) -> std::optional<RadialFit> {
  const double count = static_cast<double>(points.size());
  std::array<double, 5> distances{};
  std::array<Point, 5> units{};
  Point mean{};
  RadialFit fit;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const auto& point = points[i];
    const Point radial{point[0] - centre[0], point[1] - centre[1]};
    const double distance = std::hypot(radial[0], radial[1]);
    if (!std::isfinite(distance) || distance == 0)
      return std::nullopt;
    distances.at(i) = distance;
    units.at(i) = {radial[0] / distance, radial[1] / distance};
    fit.radius += distance / count;
    mean[0] += units.at(i)[0] / count;
    mean[1] += units.at(i)[1] / count;
  }
  for (std::size_t i = 0; i < points.size(); ++i) {
    const double error = distances.at(i) - fit.radius;
    fit.sum_squares += error * error;
    fit.max_residual = std::max(fit.max_residual, std::abs(error));
    fit.update.add({units.at(i)[0] - mean[0], units.at(i)[1] - mean[1]}, error);
  }
  return fit;
}

} // namespace

auto fit_probe_circle(std::span<const std::array<double, 2>> points) -> std::optional<CircleFit> {
  if (points.size() < 4 || points.size() > 5)
    return std::nullopt;
  // The algebraic fit in coordinates about the centroid seeds the orthogonal fit.
  Point mean{};
  const double count = static_cast<double>(points.size());
  for (const auto& point : points) {
    mean[0] += point[0] / count;
    mean[1] += point[1] / count;
  }
  Normal seed;
  for (const auto& point : points) {
    const Point from_mean{point[0] - mean[0], point[1] - mean[1]};
    seed.add(from_mean, (from_mean[0] * from_mean[0] + from_mean[1] * from_mean[1]) / 2);
  }
  const auto initial = seed.solve();
  if (!initial)
    return std::nullopt;
  Point centre{mean[0] + (*initial)[0], mean[1] + (*initial)[1]};
  auto fit = radial_fit(points, centre);
  for (int iteration = 0; fit && iteration < 30; ++iteration) {
    const auto step = fit->update.solve();
    if (!step)
      return std::nullopt;
    if (std::hypot((*step)[0], (*step)[1]) <= 1e-8)
      return fit->radius <= 5000
                 ? std::optional{CircleFit{{centre, fit->radius}, fit->max_residual}}
                 : std::nullopt;
    double fraction = 1;
    while (fraction >= 1.0 / 128) {
      const Point candidate{centre[0] + fraction * (*step)[0], centre[1] + fraction * (*step)[1]};
      auto next = radial_fit(points, candidate);
      if (next && next->sum_squares < fit->sum_squares) {
        centre = candidate;
        fit = next;
        break;
      }
      fraction /= 2;
    }
    if (fraction < 1.0 / 128)
      return std::nullopt;
  }
  return std::nullopt;
}

} // namespace coffeecnc
