#pragma once
#include <cstdint>
#include <string>
#include "app_update.hpp"
#include <vector>

namespace nmsaves {
inline constexpr char AppVersion[] = "0.2.3";
inline constexpr char AppPath[] = "sdmc:/switch/NinMagicSaves/NinMagic-Saves.nro";
enum class UiEnvironment { Unknown, EmuMmc, SysMmc };
enum class HomeAction { Refresh, BringOriginals, Import, Backups, Export, ReturnEmuMmc };
inline std::vector<HomeAction> homeActions(UiEnvironment environment) {
    if(environment==UiEnvironment::EmuMmc)return {HomeAction::BringOriginals,HomeAction::Import,HomeAction::Backups};
    if(environment==UiEnvironment::SysMmc)return {HomeAction::Export,HomeAction::ReturnEmuMmc};
    return {HomeAction::Refresh};
}
inline bool canExportUi(UiEnvironment environment) {return environment==UiEnvironment::SysMmc;}
inline bool canImportUi(UiEnvironment environment,bool backupVerified) {return environment==UiEnvironment::EmuMmc&&backupVerified;}
inline bool canUpdateUi(UiEnvironment environment) {return environment==UiEnvironment::EmuMmc;}
enum class Screen { Home, Sources, ExportConfirm, Imports, Targets, ImportConfirm, Backups, RestoreConfirm, ConfigureReboot, RebootConfirm, Help, Update, ConfirmUpdate, ConfirmArchive, Busy, Result };
struct UiRow {std::string title,subtitle,detail;bool checked=false;};
struct View {
    Screen screen = Screen::Home;
    UiEnvironment environment=UiEnvironment::Unknown;
    std::string firmware,atmosphere,environmentDetail;
    bool offline=false;
    std::vector<UiRow> rows;
    size_t selected=0;
    std::string sourceGame,sourceProfile,sourceAccountId,sourceTitleId,destinationProfile,destinationAccountId,destinationTitleId,backupPath,transferDetail;
    bool backupVerified=false,rebootToOriginal=false,automaticReboot=false;
    std::vector<std::string> rebootDetails;
    uint64_t progressCurrent=0,progressTotal=0;
    bool progressKnown=false;
    nm::AppUpdateInfo update;
    nm::CompletedUpdateInfo updateRecord;
    std::string updateError, recoveryError, message;
    bool queryComplete = false, success = false, appletMode = false;
    double seconds = 0;
};
inline bool publishedAppOnSd(const nm::AppUpdateInfo &u) {
    return u.sdVerified&&!u.version.empty()&&u.sdVersion==u.version&&u.sdSha256==u.sha256&&u.sdSize==u.size;
}
inline bool canDownloadAppUpdate(const View &v) {
    return v.queryComplete&&v.updateError.empty()&&v.update.configured&&v.update.available&&!v.update.installed&&
        !publishedAppOnSd(v.update)&&v.updateRecord.state==nm::CompletedUpdateState::None&&v.recoveryError.empty();
}
bool uiInit(const std::string &assetRoot);
void uiDraw(uint32_t *pixels, int stride, const View &view);
void uiExit();
}
