// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "common/Logger.h"

#include <cstdint>

// How many sets of per-frame GPU resources a renderer keeps, and how to index
// them. See docs/rendering/frame-pacing.md#frames-in-flight.
//
// The app hands every renderer the SWAPCHAIN IMAGE INDEX (`wd->FrameIndex`,
// 0..ImageCount-1), because that is the granularity the app's fences protect:
// it waits on the acquired image's fence before recording. Per-frame
// resources must therefore have one slot per swapchain image.
//
// They used to have exactly two, and the index was wrapped to fit. That
// stopped an out-of-bounds write (ASan caught it on an Intel Arc) but made
// images 2 and 0 share a slot on a 3-image swapchain — so the CPU could
// overwrite a uniform buffer the GPU was still reading, and the viewport
// visibly shook while the camera moved. It never showed on a 2-image
// swapchain, which is what this machine's NVIDIA reports.
namespace render {

// Capacity, not a count: sized with margin over any real swapchain (2-4 in
// practice, 5-6 with MAILBOX) so no plumbing or resize path is needed. A slot
// is a few hundred bytes of uniform buffer plus a descriptor set; the outline
// renderer's per-mesh buffers are allocated lazily per slot, so unused slots
// cost nothing.
inline constexpr uint32_t kMaxFramesInFlight = 8;

// The slot for a swapchain image index. Beyond capacity — which no driver
// should do — it warns once and wraps, which is the old behaviour: degraded
// (that is the shake) rather than a crash or an out-of-bounds write.
inline uint32_t FrameSlot(uint32_t imageIndex, const char* who) {
  if (imageIndex < kMaxFramesInFlight)
    return imageIndex;
  static bool warned = false;
  if (!warned) {
    warned = true;
    LOG_WARN("{}: swapchain image {} exceeds {} per-frame slots; frames will "
             "alias (expect flicker while the camera moves)",
             who, imageIndex, kMaxFramesInFlight);
  }
  return imageIndex % kMaxFramesInFlight;
}

} // namespace render
