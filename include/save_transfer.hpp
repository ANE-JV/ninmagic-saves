#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nmsaves {

enum class SaveEnvironment { Unknown, SysMMC, EmuMMC };
enum class SaveKind : uint8_t { Account = 1, Device = 3 };
struct TransferEnvironment {
    SaveEnvironment mode = SaveEnvironment::Unknown;
    bool offlineVerified = false;
    bool gamesClosed = false;
    // Opaque local fingerprint of this destination instance. Never a NAND selector.
    std::string instanceId;
    std::string modeLabel, networkLabel, processLabel, detail;
};
struct SaveEntry {
    uint64_t titleId = 0, saveDataId = 0;
    uint64_t userLow = 0, userHigh = 0;
    SaveKind kind = SaveKind::Account;
    uint16_t index = 0;
    uint8_t rank = 0;
    std::string gameName, userName;
};
struct ExportEntry {
    std::string id;
    SaveEntry source;
    uint64_t bytes = 0;
    uint32_t files = 0;
    std::string manifestSha256;
};
struct RestorePlan {
    std::string id;
    ExportEntry exported;
    SaveEntry destination;
    std::string backupId, backupSha256;
    uint64_t backupBytes = 0;
    bool backupVerified = false;
};
struct RecoveryEntry {
    std::string id, state, backupId;
    SaveEntry destination;
    bool needsRecovery = false;
};
struct TransferResult {
    bool ok = false;
    bool recoveryRequired = false;
    std::string message, id;
};

// Blocking calls: invoke on one worker. No function initializes or changes a NAND.
TransferEnvironment inspectTransferEnvironment();
bool listTransferSaves(std::vector<SaveEntry>& saves, std::string& error);
bool listTransferExports(std::vector<ExportEntry>& exports, std::string& error);
TransferResult exportTransferSave(const SaveEntry& source);
// Explicitly selected destination account; never infer it from the source user.
// Creates and verifies the destination backup BEFORE the UI asks to replace it.
TransferResult prepareTransferRestore(const ExportEntry& exported, const SaveEntry& destination, RestorePlan& plan);
// Call ONLY after displaying game, source user, destination user and backup, and
// obtaining replacement confirmation. Reloads the journal and checks for changes.
TransferResult applyTransferRestore(const RestorePlan& confirmedPlan);
bool listTransferRecoveries(std::vector<RecoveryEntry>& recoveries, std::string& error);
// Call after explicit recovery confirmation. Restores only this app's verified backup.
TransferResult recoverTransferRestore(const RecoveryEntry& confirmedRecovery);
bool compatibleTransferTarget(const SaveEntry& source, const SaveEntry& destination);
std::string transferTitleId(uint64_t titleId);

// Portable transfer engine. Native backend mounts the source read-only and uses
// fsFsCommit for destination commits; host tests use isolated fixture directories.
namespace transfer_detail {
// Local anti-mix-up fingerprint; never use config text as runtime NAND proof.
std::string destinationConfigFingerprint(uint64_t deviceId, const std::string& config, std::string& profilePath);
class Backend {
public:
    virtual ~Backend() = default;
    virtual TransferEnvironment environment() = 0;
    virtual bool enumerate(std::vector<SaveEntry>& saves, std::string& error) = 0;
    virtual bool mount(const SaveEntry& save, bool writable, std::string& path, std::string& error) = 0;
    virtual void unmount() = 0;
    virtual bool saveCapacity(uint64_t& bytes, std::string& error) = 0;
    virtual bool commitSave(std::string& error) = 0;
    virtual bool commitSd(std::string& error) = 0;
    // Test-only fault hook. Native backend always returns true.
    virtual bool checkpoint(const char*, const std::string&) { return true; }
};
class Engine {
    Backend& backend_;
    std::string root_;
public:
    Engine(Backend& backend, std::string root);
    bool listExports(std::vector<ExportEntry>& exports, std::string& error);
    TransferResult exportSave(const SaveEntry& source);
    TransferResult prepareRestore(const ExportEntry& exported, const SaveEntry& destination, RestorePlan& plan);
    TransferResult applyRestore(const RestorePlan& confirmedPlan);
    bool listRecoveries(std::vector<RecoveryEntry>& entries, std::string& error);
    TransferResult recover(const RecoveryEntry& confirmedRecovery);
};
}
}
