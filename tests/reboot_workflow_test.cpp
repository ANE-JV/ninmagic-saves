// Synthetic fixtures only: no NAND, console, account, or credential access.
#define NM_REBOOT_WORKFLOW_TEST 1
#include "../source/reboot_workflow.cpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace nmsaves;
using namespace nmsaves::reboot_detail;

static void put(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    assert(file.good());
}
static std::string get(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}
static void set32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
static std::vector<std::uint8_t> fakePayload() {
    std::vector<std::uint8_t> bytes(0x1000, 0xA7);
    set32(bytes, 0x118, 0x43544349);
    bytes[0x11c] = '6'; bytes[0x11d] = '4'; bytes[0x11e] = '2';
    return bytes;
}
static const char* MainIni =
    "[config]\nautoboot=1\nautoboot_list=0\n\n"
    "{My console}\n"
    "[Nintendo original]\npkg3=atmosphere/package3\nstock=1\nemummc_force_disable=1\n\n"
    "[Any name]\npkg3=atmosphere/package3\nemummcforce=1\ncal0blank=1\n";

int main() {
    std::string error, version;
    std::vector<BootEntry> entries;
    std::vector<IniDocument> docs{{"bootloader/hekate_ipl.ini", MainIni, false}};
    assert(parseBootEntries(docs, entries, error));
    assert(entries.size() == 1 && entries[0].target == RebootTarget::EmuMMC && entries[0].index == 2);
    docs.push_back({OwnedIni, ownedConfiguration(), true});
    assert(parseBootEntries(docs, entries, error));
    assert(entries.size() == 3);
    assert(entries[1].id == "nmsvSys" && entries[1].target == RebootTarget::CfwSysMMC);
    assert(entries[2].id == "nmsvEmu" && entries[2].index == 2 && entries[2].externalIni);
    assert(ownedConfiguration().find("cal0") == std::string::npos);
    assert(ownedConfiguration().find("kip1patch") == std::string::npos);
    assert(ownedConfiguration().find("autoboot=") == std::string::npos);

    const std::string normal = "[config]\nautoboot=0\n\n[CFW]\npkg3=atmosphere/package3\nemummc_force_disable=1\n";
    for (const auto& overrideLine : {
        "stock=1\n", "payload=bootloader/payloads/fusee.bin\n", "l4t=1\n",
        "emupath=emuMMC/OTHER\n", "kip1patch=nosigchk\n", "cal0blank=1\n",
        "cal0writesys=1\n", "emummcforce=1\n", "stock=0x1\n",
        "emummc_force_disable=1\n", "kernel=custom.bin\n", "unknown_override=1\n"
    }) {
        assert(parseBootEntries({{"main", normal + overrideLine, false}}, entries, error));
        assert(entries.empty());
    }
    assert(!parseBootEntries({{"main", "[config\nautoboot=0\n", false}}, entries, error));
    assert(!parseBootEntries({{"main", "[CFW]\npkg3=atmosphere/package3\nemummcforce=1\n", false}}, entries, error));
    assert(!parseBootEntries({{"main", normal + "\n[config]\nautoboot=0\n", false}}, entries, error));
    assert(parseBootEntries({{"main", normal + "\nstock=1\n", false}}, entries, error));
    assert(entries.size() == 1); // Hekate ignores the orphan key after a blank.
    assert(parseBootEntries({{"main", "[config]\nautoboot=0\n\n[X]\npkg3=atmosphere/package3\n# note\nemummc_force_disable=1\n", false}}, entries, error));
    assert(entries.empty()); // Comment terminates the entry in Hekate.
    assert(parseBootEntries({{"main", normal + "id=equal\n\n[stock]\nstock=1\nid=equal\n", false}}, entries, error));
    assert(entries.size() == 1 && !entries[0].automaticAddressable);
    assert(parseBootEntries({{"main", normal + "id=longerthan7\n", false}}, entries, error));
    assert(entries.size() == 1 && !entries[0].automaticAddressable);
    assert(parseBootEntries({{"main", normal + "id=equal\n\n[payload]\npayload=x.bin\nid=other\nid=equal\n", false}}, entries, error));
    assert(entries.size() == 1 && !entries[0].automaticAddressable);

    auto payload = fakePayload();
    assert(validateHekatePayload(payload, version, error) && version == "6.4.2");
    auto before = payload;
    BootEntry boot{"Sys", "nmsvSys", OwnedIni, RebootTarget::CfwSysMMC, 1, true};
    assert(patchHekatePayload(payload, boot, error));
    assert(payload[0x94] == 5 && payload[0x95] == 1 && payload[0x96] == 1 && payload[0x97] == 0);
    assert(std::memcmp(payload.data() + 0x98, "nmsvSys\0", 8) == 0);
    for (std::size_t i = 0; i < payload.size(); ++i)
        if (i < 0x94 || i >= 0x118) assert(payload[i] == before[i]);
    boot.id.clear(); boot.index = 7;
    assert(patchHekatePayload(payload, boot, error) && payload[0x94] == 1 && payload[0x95] == 7);
    boot.index = 0; assert(!patchHekatePayload(payload, boot, error));
    payload[0x118] = 0; assert(!validateHekatePayload(payload, version, error));
    payload = fakePayload(); payload[0x11c] = '7'; assert(!validateHekatePayload(payload, version, error));
    payload.resize(MaxPayload + 1); assert(!validateHekatePayload(payload, version, error));
    for (unsigned mask = 0; mask < 16; ++mask)
        assert(isOffline(mask & 1, mask & 2, mask & 4, mask & 8) == (mask == 7));

    char temp[] = "/tmp/ninmagic-reboot-XXXXXX";
    const char* made = mkdtemp(temp); assert(made);
    testSdRoot = made;
    const fs::path root(testSdRoot);
    put(root / "bootloader/hekate_ipl.ini", MainIni);
    put(root / "emuMMC/emummc.ini", "[emummc]\nenabled=1\nsector=0x123400\npath=emuMMC/RAW1\nid=0x0000\nnintendo_path=emuMMC/RAW1/Nintendo\n");
    std::vector<std::uint8_t> marker(4); set32(marker, 0, 0x123400);
    put(root / "emuMMC/RAW1/raw_based", bytesToken(marker));
    std::vector<std::uint8_t> package(1024, 0); set32(package, 4, 0x20); set32(package, 0x20, 0x30535346);
    put(root / "atmosphere/package3", bytesToken(package));
    put(root / "bootloader/update.bin", bytesToken(fakePayload()));
    put(root / "exosphere.ini", "[exosphere]\nblank_prodinfo_sysmmc=1\n");
    const auto originalMain = get(root / "bootloader/hekate_ipl.ini");
    const auto originalExo = get(root / "exosphere.ini");
    testRuntime.mode = RuntimeMode::EmuMMC;
    testRuntime.hardware = HardwareFamily::Erista;
    testRuntime.atmosphere = true;
    testRuntime.offline = false; // Unknown network is informational, not a gate.
    testRuntime.gamesClosed = true;
    auto inspection = inspectRebootWorkflow();
    assert(inspection.configurationValid && inspection.payloadValid && inspection.canPrepareAccess);
    assert(!prepareRebootAccess(false, true, error));
    assert(!prepareRebootAccess(true, false, error));
    assert(error.find("aviso de red") != std::string::npos);
    assert(!fs::exists(root / OwnedIni));
    assert(prepareRebootAccess(true, true, error));
    assert(get(root / OwnedIni) == ownedConfiguration());
    assert(get(root / "bootloader/hekate_ipl.ini") == originalMain);
    assert(get(root / "exosphere.ini") == originalExo);
    assert(prepareRebootAccess(true, true, error)); // idempotent; no overwrite.
    RebootPlan plan;
    assert(prepareRebootPlan(RebootTarget::CfwSysMMC, plan, error));
    assert(plan.automatic && plan.entry.id == "nmsvSys");
    assert(plan.detail.find("despues de iniciar puede ser tarde") != std::string::npos);
    assert(!requestReboot(plan, false, true, error));
    assert(!requestReboot(plan, true, false, error));
    assert(error.find("aviso de red") != std::string::npos);
    testRuntime.gamesClosed = false;
    assert(!prepareRebootAccess(true, true, error));
    assert(!requestReboot(plan, true, true, error));
    assert(error.find("Cierra el juego") != std::string::npos);
    testRuntime.gamesClosed = true;
    assert(!requestReboot(plan, true, true, error));
    assert(error.find("PC") != std::string::npos); // never a host reboot
    assert(get(root / "bootloader/update.bin") == bytesToken(fakePayload()));
    // Connected, connecting, disconnected, unknown and inconsistent cached
    // network observations do not prevent setup or review. A network change
    // after review must also reach the host-only reboot boundary, not block.
    for (unsigned mask = 0; mask < 32; ++mask) {
        testRuntime.wifiDisabledKnown = mask & 1;
        testRuntime.wifiDisabled = mask & 2;
        testRuntime.networkKnown = mask & 4;
        testRuntime.networkConnected = mask & 8;
        testRuntime.offline = mask & 16;
        assert(prepareRebootAccess(true, true, error));
        RebootPlan networkPlan;
        assert(prepareRebootPlan(RebootTarget::CfwSysMMC, networkPlan, error));
        assert(networkPlan.automatic && networkPlan.entry.id == "nmsvSys");
        assert(!requestReboot(plan, true, true, error));
        assert(error.find("PC") != std::string::npos);
    }
    testRuntime.atmosphere = false;
    assert(!prepareRebootAccess(true, true, error));
    assert(!requestReboot(plan, true, true, error));
    assert(error.find("NAND activa") != std::string::npos);
    testRuntime.atmosphere = true;
    put(root / "bootloader/hekate_ipl.ini", originalMain + "\n# changed since review\n");
    assert(!requestReboot(plan, true, true, error) && error.find("cambiaron") != std::string::npos);
    testRuntime.hardware = HardwareFamily::Mariko;
    assert(prepareRebootPlan(RebootTarget::CfwSysMMC, plan, error) && !plan.automatic);
    assert(!requestReboot(plan, true, true, error) && error.find("manualmente") != std::string::npos);
    testRuntime.mode = RuntimeMode::Unknown;
    assert(!prepareRebootPlan(RebootTarget::CfwSysMMC, plan, error));
    testRuntime.mode = RuntimeMode::SysMMC;
    assert(prepareRebootPlan(RebootTarget::EmuMMC, plan, error) && plan.entry.id == "nmsvEmu");
    assert(!prepareRebootPlan(RebootTarget::CfwSysMMC, plan, error));
    assert(prepareRebootAccess(true, false, error)); // original already running
    testRuntime.hardware = HardwareFamily::Unknown;
    assert(prepareRebootPlan(RebootTarget::EmuMMC, plan, error) && !plan.automatic);
    put(root / "bootloader/ini/duplicate.ini", "[Stock duplicate]\nstock=1\nid=nmsvEmu\n");
    assert(prepareRebootPlan(RebootTarget::EmuMMC, plan, error) && !plan.automatic);
    assert(!plan.entry.automaticAddressable);
    fs::remove(root / "bootloader/ini/duplicate.ini");
    put(root / OwnedIni, "[user configuration]\npayload=other.bin\n");
    assert(!prepareRebootAccess(true, true, error));
    assert(get(root / OwnedIni) == "[user configuration]\npayload=other.bin\n");

    // File-based emuMMC: configuration/metadata-only checks, no NAND reads.
    put(root / "emuMMC/emummc.ini", "[emummc]\nenabled=1\nsector=0\npath=emuMMC/SD00\n");
    put(root / "emuMMC/SD00/file_based", "");
    for (const char* name : {"BOOT0", "BOOT1", "00"}) put(root / "emuMMC/SD00/eMMC" / name, "fixture");
    inspection = inspectRebootWorkflow(); assert(inspection.configurationValid);
    put(root / "emuMMC/emummc.ini", "[emummc]\nenabled=1\npath=emuMMC/SD00\n");
    inspection = inspectRebootWorkflow(); assert(inspection.configurationValid); // sector defaults 0
    put(root / "emuMMC/emummc.ini", "[emummc]\nenabled=1\nsector=0\npath=../outside\n");
    inspection = inspectRebootWorkflow(); assert(!inspection.configurationValid);
    fs::remove_all(root); // Exactly the mkdtemp fixture created above.
    std::cout << "Reboot workflow tests passed (synthetic fixtures only; hardware reboot untested).\n";
}
