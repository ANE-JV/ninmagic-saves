#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nmsaves {

enum class RuntimeMode { Unknown, EmuMMC, SysMMC };
enum class HardwareFamily { Unknown, Erista, Mariko };
enum class RebootTarget { CfwSysMMC, EmuMMC };

struct RuntimeEnvironment {
    RuntimeMode mode = RuntimeMode::Unknown;
    HardwareFamily hardware = HardwareFamily::Unknown;
    bool atmosphere = false;
    bool wifiDisabledKnown = false;
    bool wifiDisabled = false;
    bool networkKnown = false;
    bool networkConnected = false;
    // Informational only. Network state never authorizes or blocks a reboot.
    bool offline = false;
    bool gamesClosed = false;
    std::string detail;
};

struct BootEntry {
    std::string name, id, sourcePath;
    RebootTarget target = RebootTarget::CfwSysMMC;
    unsigned index = 0;
    bool externalIni = false;
    bool automaticAddressable = true;
};

struct RebootWorkflow {
    RuntimeEnvironment runtime;
    std::vector<BootEntry> entries;
    std::string payloadPath, payloadVersion, detail;
    std::string configurationToken;
    bool configurationValid = false;
    bool payloadValid = false;
    bool ownedConfigurationPresent = false;
    bool canPrepareAccess = false;
    bool automaticRebootSupported = false;
};

struct RebootPlan {
    RebootTarget target = RebootTarget::CfwSysMMC;
    BootEntry entry;
    std::string payloadPath, payloadToken, configurationToken, detail;
    RuntimeMode sourceMode = RuntimeMode::Unknown;
    bool automatic = false;
};

// Reads runtime services and Hekate files. Never writes configuration or reboots.
RuntimeEnvironment inspectRuntimeEnvironment();
RebootWorkflow inspectRebootWorkflow();

// Writes ONLY bootloader/ini/ninmagic-saves.ini after an explicit confirmation.
// Existing different contents are preserved and rejected, never overwritten.
// The caller must show the network warning before confirming. Acknowledgment
// means the warning was read, NOT that airplane mode is enabled. Explain that
// airplane mode should be enabled in the original system before its CFW boot;
// enabling it after startup may be too late to prevent a connection. There is
// no guarantee against a console ban. The original setting cannot be read here.
bool prepareRebootAccess(bool userConfirmed,
                         bool networkNoticeAcknowledged,
                         std::string& error);

// Read-only review for a subsequent confirmation screen. Deterministically
// chooses a valid matching entry, preferring NinMagic's own entry.
bool prepareRebootPlan(RebootTarget target, RebootPlan& plan,
                       std::string& error);

// Call only on the dedicated final confirmation. Rechecks runtime, game state,
// configuration and payload. Network detection remains informational; the bool
// acknowledges reading the warning above, not an asserted airplane setting.
// No persistent autoboot or payload writes.
// Mariko returns actionable manual instructions; it never performs Erista RTP.
bool requestReboot(const RebootPlan& plan, bool userConfirmed,
                   bool networkNoticeAcknowledged, std::string& error);

namespace reboot_detail {
struct IniDocument { std::string path, contents; bool external = false; };
bool parseBootEntries(const std::vector<IniDocument>& documents,
                      std::vector<BootEntry>& entries, std::string& error);
bool validateHekatePayload(const std::vector<std::uint8_t>& bytes,
                          std::string& version, std::string& error);
bool patchHekatePayload(std::vector<std::uint8_t>& bytes,
                       const BootEntry& entry, std::string& error);
// Pure generation, also used by host tests. No NAND data is part of this INI.
std::string ownedConfiguration();
bool isOffline(bool wifiDisabledKnown, bool wifiDisabled, bool networkKnown,
               bool connected);
} // namespace reboot_detail
} // namespace nmsaves
