#pragma once
// The logo as a signed distance field, for the shader that renders it as a solid 3D object.
//
// Object space: x runs -1..1 across the logo's width, y up (the height follows the logo's proportions), z toward the viewer.
// The field is negative inside the logo and positive outside, in those same units, on a canvas with a margin all round so the
// distance keeps going outside the shape.
#include <vector>

namespace wezt::ui {

struct LogoSdf {
  int w = 0, h = 0;                  // texels
  float halfX = 1.f, halfY = 0.35f;  // half extent of the whole canvas (margin included), object units
  float logoHalfY = 0.33f;           // half height of the logo itself
  std::vector<float> d;              // row 0 = top (+y); signed distance in object units
};

LogoSdf makeLogoSdf();

// Exact squared Euclidean distance transform of a grid (Felzenszwalb & Huttenlocher), in place: `f` holds 0 on feature cells and a
// large value elsewhere; on return it holds the squared distance (in cells) to the nearest feature cell. Exposed for tests.
void squaredDistanceTransform(std::vector<float>& f, int w, int h);

}  // namespace wezt::ui
