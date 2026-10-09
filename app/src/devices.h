// SPDX-License-Identifier: GPL-2.0-only
// Locating the dc30 driver's device nodes.
#pragma once

#include <string>

namespace dc30 {

// /dev/videoN of the V4L2 node with this name (sysfs "name"), or "".
std::string findVideoNode(const std::string &name);

// The driver's nodes and ALSA card.
inline std::string videoNode() { return findVideoNode("dc30"); }
inline std::string metaNode() { return findVideoNode("dc30-meta"); }
constexpr const char *alsaDevice = "hw:DC30";

} // namespace dc30
