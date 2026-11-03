#pragma once
#include <string>
namespace nm {
// Downloads only the updater-owned manifest/download temporary paths. HTTPS,
// certificate/host validation and bounded bodies remain mandatory.
bool downloadHttpsFile(const std::string &url,const std::string &path,std::string &error);
// Call only after the updater worker has ended.
void updateNetworkExit();
}
