#pragma once
// The Wezt logo as a picture. The shape is the owner's logo (assets/logo_original.jpg) stored as an 8-bit coverage mask
// (logo_mask.inc); the gold metal look (bevel, highlights, a darker rim) is computed here, so it can be re-tuned in code.
#include <vector>

namespace wezt::ui {

struct LogoImage {
  int w = 0, h = 0;
  std::vector<unsigned char> rgba;   // straight (not premultiplied) alpha, w*h*4
};

enum class LogoStyle { Gold, Black };
LogoImage makeLogo(LogoStyle style);   // polished gold, or polished black with a fine gold rim
LogoImage makeWhiteLogo();   // same shape in white: drawn over the gold for the moving glint
// Half-size copies down to 1x1 (for textures that are drawn much smaller than their size). [0] is the input.
std::vector<LogoImage> mipChain(const LogoImage& base);

}  // namespace wezt::ui
