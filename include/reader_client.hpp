#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nmsaves {

enum class ReaderState { Unavailable, Rejected, PresentUnverified };

struct ReaderDiagnostics {
    std::string firmware = "No disponible";
    std::string atmosphere = "Sin comprobar";
    std::string runtimeMode = "Entorno sin comprobar";
    std::string readerState = "Sin comprobar";
    std::string summary;
    std::string detail;
    std::vector<std::string> checks;
    ReaderState state = ReaderState::Unavailable;
    bool appletMode = false;
    bool servicePresent = false;
    bool protocolValid = false;
    // This diagnostic build cannot independently verify physical NAND provenance.
    bool physicalAccessVerified = false;
};

// Runtime/configuration and command-0 checks only. Does not open BIS, enumerate
// savedata, read keys, mount the original system, or write any data.
ReaderDiagnostics inspectReader();

namespace detail {

constexpr std::size_t ReaderHandshakeBytes = 32;
using ReaderHandshakeBuffer = std::array<std::uint8_t, ReaderHandshakeBytes>;

// Small transport seam for exercising the same fail-closed path on a host PC.
// Result zero means success; nonzero values remain diagnostic-only result codes.
class ReaderTransport {
public:
    virtual ~ReaderTransport() = default;
    virtual std::uint32_t queryPresence(bool& present) = 0;
    virtual std::uint32_t open() = 0;
    virtual std::uint32_t getHandshake(ReaderHandshakeBuffer& bytes,
                                     std::size_t& received) = 0;
    virtual void close() = 0;
};

void inspectReaderService(ReaderTransport& transport, ReaderDiagnostics& result);

struct EmummcConfiguration {
    enum class Status { NotSpecified, Enabled, Disabled, Invalid };
    Status status = Status::NotSpecified;
};

// Configuration is only a boot preference, never evidence of the running NAND.
EmummcConfiguration parseEmummcConfiguration(const std::string& contents);

} // namespace detail
} // namespace nmsaves
