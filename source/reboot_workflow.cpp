// SPDX-License-Identifier: GPL-2.0-only
// Hekate boot_cfg ABI: CTCaer/hekate README, bdk/utils/types.h.
// Atmosphere bpc:ams command 65001: troposphere/reboot_to_payload in Atmosphere.
// This implementation does not modify either project's binaries or source.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "reboot_workflow.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace nmsaves {
namespace {
constexpr std::size_t MaxIni = 64 * 1024;
constexpr std::size_t MaxPayload = 0x24000;
constexpr const char* OwnedIni = "bootloader/ini/ninmagic-saves.ini";
constexpr const char* SysId = "nmsvSys";
constexpr const char* EmuId = "nmsvEmu";
#ifdef NM_REBOOT_WORKFLOW_TEST
std::string testSdRoot;
RuntimeEnvironment testRuntime;
#endif

std::string sdPath(const std::string& relative) {
#ifdef NM_REBOOT_WORKFLOW_TEST
    return testSdRoot + "/" + relative;
#else
    return "sdmc:/" + relative;
#endif
}

bool validRelative(const std::string& path) {
    if (path.empty() || path.size() > 200 || path.front() == '/' ||
        path.find(':') != std::string::npos || path.find('\\') != std::string::npos)
        return false;
    std::istringstream stream(path);
    std::string part;
    while (std::getline(stream, part, '/')) {
        if (part.empty() || part == "." || part == "..") return false;
        for (unsigned char c : part) if (c < 32 || c == 127) return false;
    }
    return path.back() != '/';
}

bool plainDirectory(const std::string& path) {
    struct stat st{};
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
}

bool safeParents(const std::string& relative) {
    if (!validRelative(relative)) return false;
    std::size_t at = relative.find('/');
    while (at != std::string::npos) {
        if (!plainDirectory(sdPath(relative.substr(0, at)))) return false;
        at = relative.find('/', at + 1);
    }
    return true;
}

bool regular(const std::string& path, std::uint64_t limit, bool allowEmpty = false) {
    struct stat st{};
    return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode) &&
           st.st_size >= (allowEmpty ? 0 : 1) && static_cast<std::uint64_t>(st.st_size) <= limit;
}

bool readBytes(const std::string& relative, std::size_t limit,
               std::vector<std::uint8_t>& data, bool allowEmpty = false) {
    data.clear();
    const auto path = sdPath(relative);
    if (!safeParents(relative) || !regular(path, limit, allowEmpty)) return false;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    std::array<std::uint8_t, 4096> block{};
    std::size_t count;
    bool ok = true;
    while ((count = std::fread(block.data(), 1, block.size(), file))) {
        if (count > limit - data.size()) { ok = false; break; }
        data.insert(data.end(), block.begin(), block.begin() + count);
    }
    ok = ok && !std::ferror(file);
    std::fclose(file);
    return ok && (allowEmpty || !data.empty());
}

bool readText(const std::string& relative, std::string& text, bool allowEmpty = false) {
    std::vector<std::uint8_t> bytes;
    if (!readBytes(relative, MaxIni, bytes, allowEmpty)) return false;
    text.assign(bytes.begin(), bytes.end());
    return text.find('\0') == std::string::npos;
}

std::uint32_t le32(const std::vector<std::uint8_t>& data, std::size_t at) {
    return std::uint32_t(data[at]) | (std::uint32_t(data[at + 1]) << 8) |
           (std::uint32_t(data[at + 2]) << 16) | (std::uint32_t(data[at + 3]) << 24);
}

std::string bytesToken(const std::vector<std::uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

void tokenPart(std::string& token, const std::string& name, const std::string& value) {
    token += std::to_string(name.size()) + ":" + name + std::to_string(value.size()) + ":" + value;
}

struct Section {
    std::string name;
    std::map<std::string, std::string> fields;
    bool ambiguous = false;
    std::vector<std::string> declaredIds;
    std::size_t lineNumber = 0;
};

// Hekate's parser is NOT a general-purpose INI parser: blank/comment/caption
// lines terminate the previous section, whitespace is not stripped, and ';'
// is not a comment marker. Mirror that distinction instead of guessing.
// See CTCaer/hekate bdk/utils/ini.c:_find_section_name and ini_parse. Captions
// end at the first '}' or end of the line; a closing '}' is not mandatory.
bool parseSections(const std::string& text, std::vector<Section>& out,
                   std::string& error, const std::string& path) {
    out.clear(); error.clear();
    const auto fail = [&error, &path](std::size_t line, const char* message) {
        error = path + ":" + std::to_string(line) + ": " + message;
        return false;
    };
    if (text.size() > MaxIni) return fail(1, "INI demasiado grande.");
    const auto nul = text.find('\0');
    if (nul != std::string::npos) {
        const auto line = 1 + std::count(text.begin(), text.begin() + nul, '\n');
        return fail(static_cast<std::size_t>(line), "INI con bytes no validos.");
    }
    std::istringstream input(text);
    std::string line;
    Section* section = nullptr;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        // FatFs FF_USE_STRFUNC=2 removes every CR, not only a trailing CR.
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        if (line.size() > 500) return fail(lineNumber, "Linea INI demasiado larga para Hekate.");
        // Hekate records strlen before removing the LF. This matters for '#'
        // and other single-character lines; do not turn them into separators.
        const auto hekateLength = line.size() + (input.eof() ? 0 : 1);
        if (hekateLength < 2 || (!line.empty() &&
            (line[0] == '{' || (line[0] == '#' && hekateLength > 2)))) {
            section = nullptr; continue;
        }
        if (!line.empty() && line[0] == '[') {
            const auto close = line.find(']');
            // Hekate ignores suffixes after the first ']'. Missing or empty
            // choice names remain rejected rather than guessing a boot entry.
            if (close == std::string::npos || close < 2)
                return fail(lineNumber, "Seccion INI mal formada.");
            out.push_back({line.substr(1, close - 1), {}, false, {}, lineNumber});
            section = &out.back(); continue;
        }
        if (!section) continue; // Hekate ignores orphan key/value lines.
        const auto equals = line.find('=');
        if (equals == std::string::npos || equals == 0) {
            section->ambiguous = true; continue;
        }
        const auto key = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        if (key == "id") section->declaredIds.push_back(value);
        if (key.front() == ' ' || key.back() == ' ' || key.find('\t') != std::string::npos ||
            value.empty() || value.front() == ' ' || value.back() == ' ' ||
            !section->fields.emplace(key, value).second) section->ambiguous = true;
    }
    return true;
}

bool exactBool(const Section& section, const char* key, bool& value) {
    const auto it = section.fields.find(key);
    if (it == section.fields.end()) { value = false; return true; }
    if (it->second != "0" && it->second != "1") return false;
    value = it->second == "1"; return true;
}

bool bootIdValid(const std::string& id) {
    if (id.empty() || id.size() > 7) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

bool classify(const Section& section, RebootTarget& target) {
    if (section.ambiguous || section.fields.count("payload") || section.fields.count("l4t")) return false;
    bool stock = false, force = false, disable = false, blank = false, writes = false;
    if (!exactBool(section, "stock", stock) || stock ||
        !exactBool(section, "emummcforce", force) ||
        !exactBool(section, "emummc_force_disable", disable) || force == disable ||
        !exactBool(section, "cal0blank", blank) ||
        !exactBool(section, "cal0writesys", writes)) return false;
    const auto pkg = section.fields.find("pkg3");
    const auto fss = section.fields.find("fss0");
    if ((pkg == section.fields.end()) == (fss == section.fields.end())) return false;
    const auto& path = pkg != section.fields.end() ? pkg->second : fss->second;
    if (path != "atmosphere/package3") return false;
    // Exclude unreviewed executables/patches and emuMMC destination overrides.
    // Presentation keys do not affect NAND selection.
    static const std::set<std::string> common{
        "id", "pkg3", "fss0", "stock", "emummcforce", "emummc_force_disable",
        "cal0blank", "cal0writesys", "icon", "logopath", "bootwait", "usb3force",
        "atmosphere"
    };
    for (const auto& kv : section.fields) if (!common.count(kv.first)) return false;
    if (disable && (blank || writes)) return false;
    target = disable ? RebootTarget::CfwSysMMC : RebootTarget::EmuMMC;
    return true;
}

bool configurationDocuments(std::vector<reboot_detail::IniDocument>& documents,
                            std::string& token, std::string& error) {
    documents.clear(); token.clear();
    std::string main;
    if (!readText("bootloader/hekate_ipl.ini", main)) {
        error = "No se puede leer bootloader/hekate_ipl.ini."; return false;
    }
    documents.push_back({"bootloader/hekate_ipl.ini", main, false});
    std::vector<std::string> files;
    const auto iniDir = sdPath("bootloader/ini");
    struct stat dirStat{};
    if (lstat(iniDir.c_str(), &dirStat) == 0) {
        if (!S_ISDIR(dirStat.st_mode) || S_ISLNK(dirStat.st_mode)) {
            error = "bootloader/ini no es una carpeta normal."; return false;
        }
        const auto addFile = [&files](const std::string& name) {
            if (name.size() < 4) return;
            std::string ext = name.substr(name.size() - 4);
            for (char& c : ext) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
            if (ext == ".ini") files.push_back(name);
        };
        int readError = 0;
#ifdef __SWITCH__
        // libnx fsdev's POSIX iterator uses ENOENT at EOF. Native FS returns an
        // explicit count of zero and preserves real errors without guessing.
        FsDir dir{};
        Result rc = fsFsOpenDirectory(fsdevGetDeviceFileSystem("sdmc"),
                                     "/bootloader/ini", FsDirOpenMode_ReadFiles, &dir);
        if (R_FAILED(rc)) { error = "No se pueden revisar las entradas de Hekate."; return false; }
        while (files.size() <= 64) {
            FsDirectoryEntry item{};
            s64 count = 0;
            rc = fsDirRead(&dir, &count, 1, &item);
            if (R_FAILED(rc) || count < 0 || count > 1) { readError = 1; break; }
            if (count == 0) break;
            const auto length = strnlen(item.name, sizeof(item.name));
            if (length == sizeof(item.name)) { readError = 1; break; }
            addFile(std::string(item.name, length));
        }
        fsDirClose(&dir);
#else
        DIR* dir = opendir(iniDir.c_str());
        if (!dir) { error = "No se pueden revisar las entradas de Hekate."; return false; }
        errno = 0;
        while (auto* item = readdir(dir)) {
            addFile(item->d_name);
            if (files.size() > 64) break;
        }
        readError = errno;
        closedir(dir);
#endif
        if (readError || files.size() > 64) { error = "Lista INI incompleta o demasiado extensa."; return false; }
    } else if (errno != ENOENT) {
        error = "No se puede comprobar bootloader/ini."; return false;
    }
    std::sort(files.begin(), files.end());
    for (const auto& name : files) {
        const auto path = std::string("bootloader/ini/") + name;
        std::string text;
        if (!readText(path, text, true)) { error = "No se puede revisar " + path + "."; return false; }
        documents.push_back({path, text, true});
    }
    for (const auto& doc : documents) tokenPart(token, doc.path, doc.contents);
    return true;
}

bool package3Valid(std::string& token, std::string& error) {
    const auto path = sdPath("atmosphere/package3");
    if (!safeParents("atmosphere/package3") || !regular(path, 32 * 1024 * 1024)) {
        error = "Falta el package3 instalado de Atmosphere."; return false;
    }
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) { error = "No se puede abrir package3."; return false; }
    std::vector<std::uint8_t> header(1024);
    const auto count = std::fread(header.data(), 1, header.size(), file);
    std::fclose(file);
    if (count != header.size()) { error = "package3 incompleto."; return false; }
    const auto meta = le32(header, 4);
    if (meta > header.size() - 32 || le32(header, meta) != 0x30535346) {
        error = "package3 no contiene una cabecera FSS0 reconocida."; return false;
    }
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) { error = "package3 cambio durante la revision."; return false; }
    tokenPart(token, "package3.header", bytesToken(header));
    tokenPart(token, "package3.stat", std::to_string(st.st_size) + ":" + std::to_string(st.st_mtime));
    return true;
}

bool hexNumber(const std::string& value, std::uint32_t& out) {
    std::size_t at = value.rfind("0x", 0) == 0 ? 2 : 0;
    if (at == value.size() || value.size() - at > 8) return false;
    out = 0;
    for (; at < value.size(); ++at) {
        const char c = value[at];
        unsigned n = c >= '0' && c <= '9' ? c - '0' :
                     c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                     c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        if (n == 16) return false;
        out = (out << 4) | n;
    }
    return true;
}

bool emummcValid(std::string& token, std::string& error) {
    std::string text;
    if (!readText("emuMMC/emummc.ini", text)) { error = "Falta la configuracion de emuMMC."; return false; }
    tokenPart(token, "emuMMC/emummc.ini", text);
    std::vector<Section> sections;
    if (!parseSections(text, sections, error, "emuMMC/emummc.ini")) return false;
    const Section* emu = nullptr;
    for (const auto& s : sections) if (s.name == "emummc") {
        if (emu) { error = "Configuracion emuMMC duplicada."; return false; }
        emu = &s;
    }
    if (!emu || emu->ambiguous || !emu->fields.count("enabled") || emu->fields.at("enabled") != "1" ||
        !emu->fields.count("path")) {
        error = "emuMMC no esta habilitada o su configuracion es ambigua."; return false;
    }
    const auto& path = emu->fields.at("path");
    std::uint32_t sector = 0;
    if (!validRelative(path) || !safeParents(path + "/marker") ||
        (emu->fields.count("sector") && !hexNumber(emu->fields.at("sector"), sector))) {
        error = "La ruta o el sector de emuMMC no son validos."; return false;
    }
    if (sector) {
        // Read only the SD-side marker, never the raw partition itself.
        std::vector<std::uint8_t> marker;
        if (!readBytes(path + "/raw_based", 4, marker) || marker.size() != 4 || le32(marker, 0) != sector) {
            error = "El marcador de la emuMMC RAW no coincide con su configuracion."; return false;
        }
        tokenPart(token, path + "/raw_based", bytesToken(marker));
    } else {
        if (!regular(sdPath(path + "/file_based"), 32, true) ||
            !safeParents(path + "/eMMC/00") ||
            !regular(sdPath(path + "/eMMC/00"), 64ULL * 1024 * 1024 * 1024) ||
            !regular(sdPath(path + "/eMMC/BOOT0"), 8 * 1024 * 1024) ||
            !regular(sdPath(path + "/eMMC/BOOT1"), 8 * 1024 * 1024)) {
            error = "La emuMMC por archivos esta incompleta."; return false;
        }
        // Existence/size checks only. No eMMC file contents are read.
        for (const char* name : {"00", "BOOT0", "BOOT1"}) {
            struct stat st{};
            if (lstat(sdPath(path + "/eMMC/" + name).c_str(), &st) != 0) return false;
            tokenPart(token, path + "/eMMC/" + name, std::to_string(st.st_size));
        }
    }
    return true;
}

bool safeRuntime(const RuntimeEnvironment& runtime, std::string& error) {
    if (!runtime.atmosphere || runtime.mode == RuntimeMode::Unknown) {
        error = "No se ha comprobado la NAND activa mediante Atmosphere (SPL 65007)."; return false;
    }
    if (!runtime.gamesClosed) {
        error = "Cierra el juego antes de preparar o reiniciar. No se ha comprobado que solo este abierta NinMagic Saves.";
        return false;
    }
    return true;
}

bool sameEntry(const BootEntry& a, const BootEntry& b) {
    return a.name == b.name && a.id == b.id && a.sourcePath == b.sourcePath &&
           a.target == b.target && a.index == b.index && a.externalIni == b.externalIni &&
           a.automaticAddressable == b.automaticAddressable;
}

bool commitSd(std::string& error) {
#ifdef __SWITCH__
    const Result rc = fsFsCommit(fsdevGetDeviceFileSystem("sdmc"));
    if (R_FAILED(rc)) { error = "No se pudo confirmar la escritura de la configuracion en la microSD."; return false; }
#else
    (void)error;
#endif
    return true;
}
} // namespace

namespace reboot_detail {
bool isOffline(bool wifiKnown, bool wifiDisabled, bool networkKnown, bool connected) {
    return wifiKnown && wifiDisabled && networkKnown && !connected;
}

std::string ownedConfiguration() {
    // Exact ownership marker and contents allow idempotent setup, never replacing
    // an unknown/user-edited file. Hekate comments are outside either section.
    return "# NinMagic Saves boot workflow v1 - no persistent autoboot changes\n"
           "[NinMagic Saves - sysMMC CFW]\n"
           "id=nmsvSys\n"
           "pkg3=atmosphere/package3\n"
           "emummc_force_disable=1\n"
           "\n"
           "[NinMagic Saves - volver a emuMMC]\n"
           "id=nmsvEmu\n"
           "pkg3=atmosphere/package3\n"
           "emummcforce=1\n";
}

bool parseBootEntries(const std::vector<IniDocument>& documents,
                      std::vector<BootEntry>& entries, std::string& error) {
    entries.clear(); error.clear();
    std::multiset<std::string> ids;
    unsigned mainIndex = 0, externalIndex = 0;
    bool mainSeen = false, configSeen = false;
    for (const auto& doc : documents) {
        if (!doc.external && mainSeen) { error = doc.path + ":1: Mas de un INI principal."; return false; }
        if (!doc.external) mainSeen = true;
        std::vector<Section> sections;
        if (!parseSections(doc.contents, sections, error, doc.path)) { entries.clear(); return false; }
        for (const auto& section : sections) {
            // Hekate skips external [config] sections before ID lookup or
            // incrementing the external boot index (main.c:_auto_launch).
            if (doc.external && section.name == "config") continue;
            // Include all IDs, even those in excluded stock/payload/ambiguous
            // entries, because Hekate searches those entries as well.
            if (!doc.external && section.name == "config") {
                if (configSeen || mainIndex != 0 || section.ambiguous) {
                    entries.clear(); error = doc.path + ":" + std::to_string(section.lineNumber) +
                        ": La seccion config principal es ambigua."; return false;
                }
                configSeen = true; continue;
            }
            ids.insert(section.declaredIds.begin(), section.declaredIds.end());
            if (!doc.external && !configSeen) {
                entries.clear(); error = doc.path + ":" + std::to_string(section.lineNumber) +
                    ": Hekate requiere config antes de las entradas."; return false;
            }
            unsigned index = doc.external ? ++externalIndex : ++mainIndex;
            const auto foundId = section.fields.find("id");
            std::string id;
            if (foundId != section.fields.end()) {
                id = foundId->second;
            }
            RebootTarget target{};
            if (index > 255 || !classify(section, target)) continue;
            entries.push_back({section.name, id, doc.path, target, index, doc.external});
        }
    }
    if (!mainSeen || !configSeen) {
        entries.clear();
        const auto main = std::find_if(documents.begin(), documents.end(), [](const IniDocument& doc) { return !doc.external; });
        error = (main == documents.end() ? "bootloader/hekate_ipl.ini" : main->path) +
            std::string(":1: Falta config en el INI principal de Hekate.");
        return false;
    }
    for (auto& entry : entries)
        if (!entry.id.empty() && (!bootIdValid(entry.id) || ids.count(entry.id) != 1))
            entry.automaticAddressable = false;
    return true;
}

bool validateHekatePayload(const std::vector<std::uint8_t>& data,
                          std::string& version, std::string& error) {
    version.clear(); error.clear();
    // 0x94 relocation stub + 0x84 boot_cfg => 0x118 ipl_ver_meta.
    if (data.size() < 0x1000 || data.size() > MaxPayload ||
        le32(data, 0x118) != 0x43544349) {
        error = "El archivo no es un payload Hekate reconocido o no cabe en IRAM."; return false;
    }
    const auto major = data[0x11c], minor = data[0x11d], patch = data[0x11e];
    if (major != '6' || minor < '0' || minor > '9' || patch < '0' || patch > '9') {
        error = "Esta version de Hekate no esta validada para el arranque de un solo uso."; return false;
    }
    // Current documented ABI and pkg3 entry support: Hekate 6.x only.
    version = std::string(1, char(major)) + "." + char(minor) + "." + char(patch);
    return true;
}

bool patchHekatePayload(std::vector<std::uint8_t>& data,
                       const BootEntry& entry, std::string& error) {
    std::string version;
    if (!validateHekatePayload(data, version, error)) return false;
    if (!entry.automaticAddressable ||
        (entry.id.empty() && (entry.index == 0 || entry.index > 255)) ||
        (!entry.id.empty() && !bootIdValid(entry.id))) {
        error = "La entrada de arranque no tiene ID ni indice valido."; return false;
    }
    std::fill(data.begin() + 0x94, data.begin() + 0x118, 0);
    data[0x94] = entry.id.empty() ? 1 : 5; // FORCE_AUTOBOOT | FROM_ID
    data[0x95] = static_cast<std::uint8_t>(entry.index);
    data[0x96] = entry.externalIni ? 1 : 0;
    if (!entry.id.empty()) std::copy(entry.id.begin(), entry.id.end(), data.begin() + 0x98);
    return true;
}
} // namespace reboot_detail

RuntimeEnvironment inspectRuntimeEnvironment() {
#ifdef NM_REBOOT_WORKFLOW_TEST
    return testRuntime;
#else
    RuntimeEnvironment result;
#ifdef __SWITCH__
    result.atmosphere = hosversionIsAtmosphere();
    if (result.atmosphere && R_SUCCEEDED(splInitialize())) {
        u64 mode = 0;
        if (R_SUCCEEDED(splGetConfig(static_cast<SplConfigItem>(65007), &mode))) {
            if (mode == 0) result.mode = RuntimeMode::SysMMC;
            else if (mode == 1) result.mode = RuntimeMode::EmuMMC;
        }
        splExit();
    }
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysProductModel model{};
        if (R_SUCCEEDED(setsysGetProductModel(&model))) {
            if (model == SetSysProductModel_Nx || model == SetSysProductModel_Copper)
                result.hardware = HardwareFamily::Erista;
            else if (model == SetSysProductModel_Iowa || model == SetSysProductModel_Hoag ||
                     model == SetSysProductModel_Calcio || model == SetSysProductModel_Aula)
                result.hardware = HardwareFamily::Mariko;
        }
        setsysExit();
    }
    if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
        bool wireless = true;
        result.wifiDisabledKnown = R_SUCCEEDED(nifmIsWirelessCommunicationEnabled(&wireless));
        result.wifiDisabled = result.wifiDisabledKnown && !wireless;
        NifmInternetConnectionType type{};
        NifmInternetConnectionStatus status{};
        u32 strength = 0;
        const Result rc = nifmGetInternetConnectionStatus(&type, &strength, &status);
        // libnx documents this exact Result as neither connecting nor connected.
        // Other failures are UNKNOWN, never treated as absence of a network.
        result.networkKnown = R_SUCCEEDED(rc) || rc == 0xD46ED;
        result.networkConnected = R_SUCCEEDED(rc); // includes all connecting states
        nifmExit();
    }
    if (R_SUCCEEDED(pmdmntInitialize())) {
        constexpr Result ProcessNotFound = 15U | (1U << 9);
        u64 application = 0, self = 0, again = 0;
        const Result rc = pmdmntGetApplicationProcessId(&application);
        if (rc == ProcessNotFound) result.gamesClosed = true;
        else if (R_SUCCEEDED(rc) && application &&
                 R_SUCCEEDED(svcGetProcessId(&self, CUR_PROCESS_HANDLE)) && self == application &&
                 R_SUCCEEDED(pmdmntGetApplicationProcessId(&again)) && again == application)
            result.gamesClosed = true;
        pmdmntExit();
    }
#endif
    result.offline = reboot_detail::isOffline(result.wifiDisabledKnown, result.wifiDisabled,
                                             result.networkKnown, result.networkConnected);
    if (result.mode == RuntimeMode::Unknown) result.detail = "No se ha comprobado la NAND activa.";
    else if (!result.gamesClosed) result.detail = "Cierra el juego antes de preparar o reiniciar.";
    else if (result.networkKnown && result.networkConnected)
        result.detail = "NAND activa comprobada; la red esta conectada o intentando conectar. Consulta el aviso de red.";
    else if (!result.networkKnown || !result.wifiDisabledKnown)
        result.detail = "NAND activa comprobada; estado de red sin comprobar. Consulta el aviso de red.";
    else if (!result.offline)
        result.detail = "NAND activa comprobada; Wi-Fi habilitado, sin conexion en este momento. Consulta el aviso de red.";
    else result.detail = "NAND activa comprobada por Atmosphere; Wi-Fi apagado y sin conexion en este momento.";
    return result;
#endif
}

RebootWorkflow inspectRebootWorkflow() {
    RebootWorkflow result;
    result.runtime = inspectRuntimeEnvironment();
    std::vector<reboot_detail::IniDocument> documents;
    if (!configurationDocuments(documents, result.configurationToken, result.detail) ||
        !reboot_detail::parseBootEntries(documents, result.entries, result.detail) ||
        !package3Valid(result.configurationToken, result.detail) ||
        !emummcValid(result.configurationToken, result.detail)) return result;
    result.configurationValid = true;
    bool ownedMismatch = false, idCollision = false;
    for (const auto& doc : documents) {
        if (doc.path == OwnedIni) {
            result.ownedConfigurationPresent = doc.contents == reboot_detail::ownedConfiguration();
            ownedMismatch = !result.ownedConfigurationPresent;
        } else if (doc.contents.find("id=nmsvSys") != std::string::npos ||
                   doc.contents.find("id=nmsvEmu") != std::string::npos) idCollision = true;
    }
    result.canPrepareAccess = !ownedMismatch && !idCollision &&
        result.runtime.mode != RuntimeMode::Unknown && result.runtime.atmosphere;
    if (ownedMismatch) result.detail = "ninmagic-saves.ini ya existe con cambios; se conserva. Revisa esa configuracion manualmente.";
    else if (idCollision) result.detail = "Los IDs reservados nmsvSys/nmsvEmu ya se usan en otro INI.";
    else result.detail = "La preparacion anade dos entradas independientes; no cambia tu arranque habitual.";
    for (const char* candidate : {"bootloader/update.bin", "atmosphere/reboot_payload.bin"}) {
        std::vector<std::uint8_t> bytes;
        std::string version, error;
        if (readBytes(candidate, MaxPayload, bytes) && reboot_detail::validateHekatePayload(bytes, version, error)) {
            result.payloadPath = candidate;
            result.payloadVersion = version;
            result.payloadValid = true; break;
        }
    }
    result.automaticRebootSupported = result.runtime.hardware == HardwareFamily::Erista && result.payloadValid;
    if (!result.automaticRebootSupported)
        result.detail += " Reinicio manual: entra en Hekate > More configs y elige la entrada indicada; luego abre NinMagic Saves desde hbmenu.";
    return result;
}

bool prepareRebootAccess(bool confirmed, bool networkNoticeAcknowledged, std::string& error) {
    error.clear();
    if (!confirmed) { error = "La preparacion necesita confirmacion dentro de la aplicacion."; return false; }
    const auto state = inspectRebootWorkflow();
    if (!state.configurationValid || !state.canPrepareAccess) { error = state.detail; return false; }
    if (!safeRuntime(state.runtime, error)) return false;
    if (state.runtime.mode == RuntimeMode::EmuMMC && !networkNoticeAcknowledged) {
        error = "Lee el aviso de red y confirma para preparar el acceso al sistema original."; return false;
    }
    if (state.ownedConfigurationPresent) return true;
    const auto dir = sdPath("bootloader/ini");
    if (!plainDirectory(dir) && mkdir(dir.c_str(), 0700) != 0) {
        error = "No se puede crear bootloader/ini."; return false;
    }
    const auto target = sdPath(OwnedIni);
    const auto pending = target + ".pending";
    const std::string content = reboot_detail::ownedConfiguration();
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(pending.c_str(), flags, 0600);
    if (fd < 0) { error = "Existe una preparacion pendiente o no se puede crear. Se conserva para revision manual."; return false; }
    FILE* file = fdopen(fd, "wb");
    if (!file) { close(fd); error = "No se pudo abrir la preparacion."; return false; }
    bool ok = std::fwrite(content.data(), 1, content.size(), file) == content.size();
    if (std::fflush(file) != 0 || fsync(fd) != 0) ok = false;
    if (std::fclose(file) != 0) ok = false;
    if (!ok) { error = "No se completo la escritura; la preparacion pendiente se conserva."; return false; }
    struct stat st{};
    if (lstat(target.c_str(), &st) == 0 || errno != ENOENT) {
        error = "El destino aparecio durante la preparacion; no se reemplaza."; return false;
    }
    // This separate file is the entire change. No pre-existing file is replaced.
    if (std::rename(pending.c_str(), target.c_str()) != 0) {
        error = "No se pudo publicar la entrada; la preparacion pendiente se conserva."; return false;
    }
    if (!commitSd(error)) return false;
    std::string verify;
    if (!readText(OwnedIni, verify) || verify != content) {
        error = "No se pudo verificar la entrada creada."; return false;
    }
    return true;
}

bool prepareRebootPlan(RebootTarget target, RebootPlan& plan, std::string& error) {
    plan = {}; error.clear();
    const auto state = inspectRebootWorkflow();
    if (!state.configurationValid) { error = state.detail; return false; }
    if (!safeRuntime(state.runtime, error)) return false;
    const RuntimeMode wanted = target == RebootTarget::CfwSysMMC ? RuntimeMode::SysMMC : RuntimeMode::EmuMMC;
    if (state.runtime.mode == wanted) { error = "Ya estas en ese entorno; no hace falta reiniciar."; return false; }
    const BootEntry* entry = nullptr;
    for (const auto& candidate : state.entries) if (candidate.target == target) {
        if (!entry) entry = &candidate;
        if (candidate.sourcePath == OwnedIni && state.ownedConfigurationPresent) { entry = &candidate; break; }
    }
    if (!entry) {
        error = "No hay una entrada CFW comprobada para ese destino. Usa Preparar acceso; nunca se elige la entrada stock.";
        return false;
    }
    plan.target = target;
    plan.entry = *entry;
    plan.sourceMode = state.runtime.mode;
    plan.configurationToken = state.configurationToken;
    plan.automatic = state.automaticRebootSupported && entry->automaticAddressable;
    plan.payloadPath = state.payloadPath;
    if (plan.automatic) {
        std::vector<std::uint8_t> bytes;
        if (!readBytes(plan.payloadPath, MaxPayload, bytes)) { error = "El payload cambio durante la revision."; return false; }
        plan.payloadToken = bytesToken(bytes);
    }
    plan.detail = "Destino: " + entry->name + ". ";
    if (!plan.automatic)
        plan.detail += std::string("Reinicia manualmente a Hekate > ") +
            (entry->externalIni ? "More configs" : "Launch") + " y elige esa entrada. ";
    if (!entry->automaticAddressable)
        plan.detail += "Su ID esta duplicado o no es valido: no se selecciona automaticamente. ";
    plan.detail += "Tras arrancar, abre NinMagic Saves desde hbmenu. No se abre automaticamente. ";
    if (target == RebootTarget::CfwSysMMC)
        plan.detail += "Es CFW sobre la memoria original, no el modo stock. Activa el modo avion en el sistema original y desconecta su cable de red antes de arrancar CFW. Hacerlo despues de iniciar puede ser tarde para evitar una conexion. No existe garantia de ausencia de baneo.";
    return true;
}

bool requestReboot(const RebootPlan& reviewed, bool confirmed,
                   bool networkNoticeAcknowledged, std::string& error) {
    error.clear();
    if (!confirmed) { error = "El reinicio necesita la confirmacion final."; return false; }
    if (reviewed.target == RebootTarget::CfwSysMMC && !networkNoticeAcknowledged) {
        error = "Lee el aviso de red y confirma para reiniciar al sistema original."; return false;
    }
    RebootPlan fresh;
    if (!prepareRebootPlan(reviewed.target, fresh, error)) return false;
    if (fresh.sourceMode != reviewed.sourceMode || !sameEntry(fresh.entry, reviewed.entry) ||
        fresh.configurationToken != reviewed.configurationToken || fresh.payloadToken != reviewed.payloadToken ||
        fresh.payloadPath != reviewed.payloadPath || fresh.automatic != reviewed.automatic) {
        error = "El entorno o la configuracion cambiaron. Revisa de nuevo el reinicio."; return false;
    }
    if (!fresh.automatic) { error = fresh.detail; return false; }
    std::vector<std::uint8_t> payload(fresh.payloadToken.begin(), fresh.payloadToken.end());
    if (!reboot_detail::patchHekatePayload(payload, fresh.entry, error)) return false;
    payload.resize(MaxPayload, 0);
#ifdef __SWITCH__
    // One-shot payload lives only in RAM. Existing reboot_payload.bin is untouched.
    // Named-port access follows Atmosphere's reboot_to_payload reference client.
    Result rc = spsmInitialize();
    if (R_FAILED(rc)) { error = "No se puede preparar el servicio de reinicio."; return false; }
    smExit();
    Handle handle = INVALID_HANDLE;
    rc = svcConnectToNamedPort(&handle, "bpc:ams");
    if (R_FAILED(rc)) {
        const Result reconnect = smInitialize(); spsmExit();
        error = R_FAILED(reconnect)
            ? "No se pudo preparar el reinicio ni reconectar SM. Cierra y vuelve a abrir NinMagic Saves; usa Hekate manualmente."
            : "Atmosphere no permite preparar este reinicio. Usa Hekate manualmente.";
        return false;
    }
    Service bpc{};
    serviceCreate(&bpc, handle);
    rc = serviceDispatch(&bpc, 65001,
        .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
        .buffers = { { payload.data(), payload.size() } });
    bool resetFailed = false;
    if (R_SUCCEEDED(rc)) {
        rc = spsmShutdown(true);
        if (R_FAILED(rc)) {
            // Do not leave our one-shot target armed after a reported failure.
            // Restore the reviewed, unpatched disk payload to the RAM slot.
            std::vector<std::uint8_t> original(fresh.payloadToken.begin(), fresh.payloadToken.end());
            original.resize(MaxPayload, 0);
            const Result reset = serviceDispatch(&bpc, 65001,
                .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
                .buffers = { { original.data(), original.size() } });
            resetFailed = R_FAILED(reset);
        }
    }
    serviceClose(&bpc);
    const Result reconnect = smInitialize(); spsmExit();
    if (R_FAILED(rc)) {
        error = resetFailed
            ? "Fallo el reinicio y no se pudo restablecer el payload temporal de RAM. Apaga la consola y entra manualmente en Hekate. Los archivos de arranque no se modificaron."
            : "No se pudo reiniciar; usa Hekate manualmente y revisa la entrada.";
        if (R_FAILED(reconnect)) error += " No se pudo reconectar SM: cierra y vuelve a abrir NinMagic Saves.";
        return false;
    }
    return true;
#else
    error = "El reinicio real solo se ejecuta en Nintendo Switch; las pruebas de PC no reinician nada.";
    return false;
#endif
}
} // namespace nmsaves
