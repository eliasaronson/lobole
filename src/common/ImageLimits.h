// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// What stb_image is allowed to decode. A reference image travels inside a
// project file, so opening someone else's document decodes bytes they chose:
// only the formats the app offers are compiled in (GIF, PSD, PIC, PNM and TGA
// hold most of stb_image's known flaws), and the stbi_ macros in IBL.cpp,
// where the implementation is compiled, follow these values.
//
// The largest side is the texture limit of most GPUs, so nothing that could be
// shown is refused, and a 16K HDR environment (16384 x 8192) still loads.
constexpr int kMaxImageSide = 16384;
constexpr const char* kImageFormats = "PNG, JPEG or BMP";

// Whether an image of this size is one the app accepts; the reason when not.
inline const char* ImageSizeProblem(int w, int h) {
  if (w <= 0 || h <= 0)
    return "not an image Loböle reads";
  if (w > kMaxImageSide || h > kMaxImageSide)
    return "larger than 16384 px on a side";
  return nullptr;
}
