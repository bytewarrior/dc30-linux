// SPDX-License-Identifier: GPL-2.0-only
#include "devices.h"

#include <filesystem>
#include <fstream>

namespace dc30 {

std::string findVideoNode(const std::string &name)
{
	namespace fs = std::filesystem;
	std::error_code ec;

	for (const auto &e : fs::directory_iterator("/sys/class/video4linux", ec)) {
		std::ifstream f(e.path() / "name");
		std::string n;

		if (std::getline(f, n) && n == name)
			return "/dev/" + e.path().filename().string();
	}
	return {};
}

} // namespace dc30
