#pragma once
#include <string>

namespace inst::util {

void initInstallServices();
void deinitInstallServices();
void playAudio(const std::string& audioPath);

} // namespace inst::util
