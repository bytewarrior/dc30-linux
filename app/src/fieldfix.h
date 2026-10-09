// SPDX-License-Identifier: GPL-2.0-only
// dc30-capture --fix: re-pairs the fields of a finished recording by
// their true parity (FieldAligner) and writes a new file.
//
// Two passes: the first decodes the fields (luma) and decides the
// pairing, the second writes the new file. MJPEG: every packet holds one
// JPEG per field, so the fields are only put together anew, nothing is
// re-encoded. FFV1: the fields' lines are put together and encoded again,
// lossless, with the settings dc30-capture records with.
//
// Time line: every frame keeps the time of its first field (Matroska
// time stamps on the 20 ms field grid). Where the field grid of the
// signal slipped, one field stands alone and becomes a frame of 20 ms;
// frames the recording repeated for lost ones are left out, the frame
// before lasts longer. The sound is copied unchanged - nothing is
// inserted, picture and sound keep their times.
#pragma once

#include <string>

namespace dc30 {

// Returns 0, or prints why not and returns 1. 'log': a text file with the
// summary, next to the output.
int fixRecording(const std::string &in, const std::string &out);

} // namespace dc30
