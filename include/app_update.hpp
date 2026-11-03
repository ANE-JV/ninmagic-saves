#pragma once
#include <cstdint>
#include <string>
namespace nm {
// Fresh installations can consult the official channel without an SD-side
// configuration file. A present update-url.txt explicitly overrides this URL.
inline constexpr char OfficialUpdateManifest[]="https://raw.githubusercontent.com/ANE-JV/ninmagic-saves/main/update.txt";
struct AppUpdateInfo {
    bool configured=false,available=false,prepared=false,installed=false;
    std::string version,url,sha256,detail;
    uint64_t size=0;
    // The executing binary and the inspected file on SD may differ until reopen.
    std::string runningVersion,sdVersion,sdSha256;
    uint64_t sdSize=0;
    bool sdVerified=false;
};
enum class CompletedUpdateState { None, CanArchive, Blocked };
struct CompletedUpdateInfo {
    CompletedUpdateState state=CompletedUpdateState::None;
    std::string token,journalState,oldVersion,newVersion,currentVersion;
    std::string currentSha256,reviewSha256,detail;
    uint64_t currentSize=0;
};
// Missing update-url.txt uses OfficialUpdateManifest without creating the file.
// A present but invalid or unreadable override is an error, never a fallback.
bool checkAppUpdate(AppUpdateInfo&,std::string &error);
// target must be exactly sdmc:/switch/NinMagicSaves/NinMagic-Saves.nro.
bool prepareAppUpdate(AppUpdateInfo&,const std::string &target,std::string &error);
// Main must obtain explicit confirmation and call romfsExit before replacing
// its NRO. Success requires closing/reopening; no automatic relaunch occurs.
bool installPreparedAppUpdate(AppUpdateInfo&,const std::string &target,std::string &error);
// Call on startup before a new update. Retains immutable numbered backups.
bool recoverAppUpdate(const std::string &target,std::string &error);
// Read-only review. Returns true only for CanArchive. None returns false with
// no error; Blocked returns false and a reason. SD fields are populated whenever
// that file can be validated, even when no record can be archived.
bool canArchiveCompletedUpdate(const std::string &target,CompletedUpdateInfo&,std::string &error);
// Explicit confirmation of the exact review snapshot is required. Revalidates
// every active record and the NRO/backup, then archives only the record TXT files.
bool archiveCompletedUpdate(const std::string &target,const CompletedUpdateInfo &review,std::string &error);
}
