#include "reader_client.hpp"
#include "../reader-prototype/include/nmsv_reader_protocol.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace nmsaves {
namespace {

std::string resultText(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(value));
    return text;
}

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r");
    return value.substr(first, last - first + 1);
}

std::uint32_t readLe32(const detail::ReaderHandshakeBuffer& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::string validateHandshake(const detail::ReaderHandshakeBuffer& bytes,
                              std::size_t received) {
    if (received != detail::ReaderHandshakeBytes) return "Tamano de respuesta incorrecto.";
    if (readLe32(bytes, 0) != NMSV_MAGIC) return "Identificador de protocolo incorrecto.";
    if (readLe32(bytes, 4) != NMSV_PROTOCOL_VERSION) return "Version de protocolo no admitida.";
    if (readLe32(bytes, 8) != detail::ReaderHandshakeBytes) return "Estructura de respuesta incompatible.";
    const auto flags = readLe32(bytes, 12);
    const auto maxRead = readLe32(bytes, 16);
    const auto backend = readLe32(bytes, 20);
    constexpr auto knownFlags = NMSV_CAP_DIAGNOSTIC_ONLY | NMSV_CAP_PHYSICAL_RAW_READ_AVAILABLE;
    if ((flags & ~knownFlags) || !(flags & NMSV_CAP_DIAGNOSTIC_ONLY))
        return "Capacidades no admitidas por esta version diagnostica.";
    if (backend > NMSV_BACKEND_FS_ORIGINAL_ACCESSOR) return "Tipo de lector desconocido.";
    if (flags & NMSV_CAP_PHYSICAL_RAW_READ_AVAILABLE) {
        if (backend != NMSV_BACKEND_FS_ORIGINAL_ACCESSOR)
            return "El lector no identifica un origen fisico admitido.";
        if (maxRead == 0 || maxRead > 65536 || maxRead % 512 != 0)
            return "Limite de lectura invalido.";
    } else if (maxRead != 0) {
        return "Limite de lectura incoherente con las capacidades.";
    }
    // A service cannot grant itself hardware validation through a response flag.
    if (readLe32(bytes, 24) != NMSV_VALIDATION_NOT_PERFORMED)
        return "La validacion declarada no es reconocida por este prototipo.";
    if (readLe32(bytes, 28) != 0) return "Campo reservado no valido.";
    return {};
}

void unavailable(ReaderDiagnostics& result, const std::string& reason) {
    result.state = ReaderState::Unavailable;
    result.readerState = "Lector no disponible";
    result.summary = "Las partidas del sistema original aun no estan accesibles.";
    result.detail = reason;
}

#ifdef __SWITCH__

class SwitchReaderTransport final : public detail::ReaderTransport {
public:
    std::uint32_t queryPresence(bool& present) override {
        // Atmosphere SM extension 65100 is a presence query, unlike GetService
        // which can wait indefinitely for a service that was never installed.
        const auto name = smEncodeName(NMSV_SERVICE_NAME);
        u8 found = 0;
        const Result rc = tipcDispatchInOut(smGetServiceSessionTipc(), 65100, name, found);
        present = R_SUCCEEDED(rc) && found == 1;
        if (R_SUCCEEDED(rc) && found > 1)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return rc;
    }

    std::uint32_t open() override {
        return smGetService(&service_, NMSV_SERVICE_NAME);
    }

    std::uint32_t getHandshake(detail::ReaderHandshakeBuffer& bytes,
                               std::size_t& received) override {
        received = 0;
        // Only command 0 exists in this client. In particular, no raw block,
        // key, savedata, write, or restore request can be issued here.
        CmifRequestFormat request{};
        request.request_id = NMSV_CMD_GET_HANDSHAKE;
        cmifMakeRequest(armGetTls(), request);
        const Result rc = svcSendSyncRequest(service_.session);
        if (R_FAILED(rc)) return rc;

        // libnx's generic response copier assumes a correctly sized CMIF body.
        // Check IPC bounds before reading a prototype service's reply instead.
        const auto base = reinterpret_cast<std::uintptr_t>(armGetTls());
        const auto response = hipcParseResponse(armGetTls());
        const auto dataStart = reinterpret_cast<std::uintptr_t>(response.data_words);
        const auto dataEnd = dataStart + response.num_data_words * sizeof(u32);
        const auto headerStart = reinterpret_cast<std::uintptr_t>(
            cmifGetAlignedDataStart(response.data_words, armGetTls()));
        constexpr std::size_t tlsIpcBytes = 0x100;
        if (response.num_copy_handles || response.num_move_handles) {
            // Unexpected handles are not part of this protocol. Close only
            // handle entries that fit inside the kernel-delivered IPC buffer.
            const auto closeHandles = [base](const Handle* handles, std::size_t count) {
                const auto start = reinterpret_cast<std::uintptr_t>(handles);
                if (start < base || start > base + tlsIpcBytes ||
                    count > (base + tlsIpcBytes - start) / sizeof(Handle)) return;
                for (std::size_t i = 0; i < count; ++i) svcCloseHandle(handles[i]);
            };
            closeHandles(response.copy_handles, response.num_copy_handles);
            closeHandles(response.move_handles, response.num_move_handles);
            return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);
        }
        if (dataStart < base || dataEnd > base + tlsIpcBytes ||
            headerStart < dataStart || headerStart + sizeof(CmifOutHeader) > dataEnd)
            return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);

        CmifOutHeader header{};
        std::memcpy(&header, reinterpret_cast<const void*>(headerStart), sizeof(header));
        if (header.magic != CMIF_OUT_HEADER_MAGIC)
            return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);
        if (R_FAILED(header.result)) return header.result;
        const auto payloadStart = headerStart + sizeof(header);
        const auto available = dataEnd - payloadStart;
        if (available > bytes.size() + 15)
            return MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader);
        // CMIF can include alignment padding, so consume the fixed 32-byte ABI.
        // A short packet is returned as short, then rejected by the common path.
        received = std::min<std::size_t>(available, bytes.size());
        std::memcpy(bytes.data(), reinterpret_cast<const void*>(payloadStart), received);
        return 0;
    }

    void close() override { serviceClose(&service_); }

private:
    Service service_{};
};

void inspectConfiguration(ReaderDiagnostics& result) {
    FILE* file = std::fopen("sdmc:/emuMMC/emummc.ini", "rb");
    if (!file) {
        result.checks.emplace_back("Configuracion emuMMC en SD: no disponible; no determina el entorno activo.");
        return;
    }
    std::array<char, 8193> buffer{};
    const auto count = std::fread(buffer.data(), 1, buffer.size(), file);
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed || count > 8192) {
        result.checks.emplace_back("Configuracion emuMMC en SD: lectura no valida; no se interpreta.");
        return;
    }
    const auto config = detail::parseEmummcConfiguration(std::string(buffer.data(), count));
    using Status = detail::EmummcConfiguration::Status;
    if (config.status == Status::Enabled)
        result.checks.emplace_back("SD: emuMMC configurada como habilitada; esto no prueba el arranque actual.");
    else if (config.status == Status::Disabled)
        result.checks.emplace_back("SD: emuMMC configurada como deshabilitada; esto no prueba el arranque actual.");
    else
        result.checks.emplace_back("SD: configuracion emuMMC ausente o ambigua; no se infiere el entorno.");
}

#endif
} // namespace

namespace detail {

EmummcConfiguration parseEmummcConfiguration(const std::string& contents) {
    EmummcConfiguration result;
    using Status = EmummcConfiguration::Status;
    if (contents.size() > 8192 || contents.find('\0') != std::string::npos) {
        result.status = Status::Invalid;
        return result;
    }
    bool emummcSection = false;
    bool sawEnabled = false;
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const auto end = contents.find('\n', offset);
        const auto length = end == std::string::npos ? contents.size() - offset : end - offset;
        std::string line = trim(contents.substr(offset, length));
        offset = end == std::string::npos ? contents.size() : end + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            emummcSection = line == "[emummc]";
            continue;
        }
        if (!emummcSection) continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos || trim(line.substr(0, equals)) != "enabled") continue;
        std::string value = line.substr(equals + 1);
        const auto comment = value.find_first_of(";#");
        if (comment != std::string::npos) value.resize(comment);
        value = trim(value);
        if (sawEnabled || (value != "0" && value != "1")) {
            result.status = Status::Invalid;
            return result;
        }
        sawEnabled = true;
        result.status = value == "1" ? Status::Enabled : Status::Disabled;
    }
    return result;
}

void inspectReaderService(ReaderTransport& transport, ReaderDiagnostics& result) {
    // Refresh every field that could otherwise survive a previous successful probe.
    result.servicePresent = false;
    result.protocolValid = false;
    result.physicalAccessVerified = false;
    unavailable(result, "El servicio de lectura original no esta instalado o no esta iniciado.");
    bool present = false;
    auto rc = transport.queryPresence(present);
    if (rc != 0) {
        result.detail = "No se pudo consultar nmsv:r (" + resultText(rc) + "). No se ha intentado acceder a la memoria original.";
        return;
    }
    if (!present) return;
    result.servicePresent = true;
    rc = transport.open();
    if (rc != 0) {
        result.detail = "El servicio esta registrado, pero no se pudo abrir (" + resultText(rc) + ").";
        return;
    }
    ReaderHandshakeBuffer bytes{};
    std::size_t received = 0;
    rc = transport.getHandshake(bytes, received);
    transport.close();
    const auto error = rc != 0 ? "Fallo en la consulta del protocolo (" + resultText(rc) + ")."
                              : validateHandshake(bytes, received);
    if (!error.empty()) {
        result.state = ReaderState::Rejected;
        result.readerState = "Lector incompatible";
        result.summary = "No se puede validar el protocolo del lector.";
        result.detail = error + " No se han leido partidas ni memoria original.";
        return;
    }
    result.protocolValid = true;
    result.state = ReaderState::PresentUnverified;
    result.readerState = "Lector presente, sin validar";
    result.summary = "El lector responde; el acceso a sysMMC fisica sigue sin comprobar.";
    result.detail = "Una respuesta del servicio no demuestra el origen de los datos. Esta version no lee ni transfiere partidas.";
    const auto backend = readLe32(bytes, 20);
    result.checks.emplace_back(backend == NMSV_BACKEND_NONE ? "Lector: no anuncia un acceso a datos." :
                              backend == NMSV_BACKEND_HOST_FIXTURE ? "Lector: anuncia datos de prueba de PC, no partidas originales." :
                              "Lector: anuncia acceso experimental; origen fisico sin validar.");
}

} // namespace detail

ReaderDiagnostics inspectReader() {
    ReaderDiagnostics result;
#ifdef __SWITCH__
    result.appletMode = appletGetAppletType() != AppletType_Application;
    SetSysFirmwareVersion firmware{};
    Result rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = setsysGetFirmwareVersion(&firmware);
        setsysExit();
        if (R_SUCCEEDED(rc)) {
            std::size_t length = 0;
            while (length < sizeof(firmware.display_version) && firmware.display_version[length]) ++length;
            result.firmware.assign(firmware.display_version, length);
        }
    }
    if (R_FAILED(rc)) result.firmware = "No disponible (" + resultText(rc) + ")";

    bool atmosphere = hosversionIsAtmosphere();
    result.atmosphere = atmosphere ? "Atmosphere detectado" : "Atmosphere no confirmado";
    rc = splInitialize();
    if (R_SUCCEEDED(rc)) {
        u64 value = 0;
        if (R_SUCCEEDED(splGetConfig(static_cast<SplConfigItem>(65000), &value))) {
            atmosphere = true;
            char text[64];
            std::snprintf(text, sizeof(text), "Atmosphere %u.%u.%u", unsigned((value >> 56) & 255),
                          unsigned((value >> 48) & 255), unsigned((value >> 40) & 255));
            result.atmosphere = text;
        }
        // Extension 65007 reports the active runtime, not the SD boot preference.
        const Result modeRc = splGetConfig(static_cast<SplConfigItem>(65007), &value);
        if (R_SUCCEEDED(modeRc) && value <= 1)
            result.runtimeMode = value == 1 ? "emuMMC activa" : "sysMMC activa";
        else
            result.checks.emplace_back("Entorno activo: respuesta no disponible o no reconocida.");
        splExit();
    } else {
        result.checks.emplace_back("Entorno activo: no se pudo abrir el servicio de configuracion.");
    }
    inspectConfiguration(result);
    if (atmosphere) {
        SwitchReaderTransport transport;
        detail::inspectReaderService(transport, result);
    } else {
        unavailable(result, "No se ha confirmado Atmosphere; no se consulta el lector experimental.");
    }
#else
    result.runtimeMode = "PC: sin consola conectada a este proceso";
    unavailable(result, "Vista de escritorio: no se simula acceso a sysMMC ni a las partidas.");
#endif
    result.checks.emplace_back("Solo diagnostico: no se abren BIS, partidas, claves ni memoria original.");
    result.checks.emplace_back("Las partidas originales y de destino permanecen sin cambios.");
    return result;
}

} // namespace nmsaves
