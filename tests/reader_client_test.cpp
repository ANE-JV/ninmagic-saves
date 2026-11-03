#include "reader_client.hpp"
#include "../reader-prototype/include/nmsv_reader_protocol.h"

#include <cstdio>
#include <cstdlib>

namespace {
int checks = 0;
void check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(1);
    }
}
#define CHECK(expr) check((expr), #expr, __LINE__)

using nmsaves::ReaderDiagnostics;
using nmsaves::ReaderState;
using nmsaves::detail::ReaderHandshakeBuffer;

void field(ReaderHandshakeBuffer& bytes, std::size_t index, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[index * 4 + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

struct Transport final : nmsaves::detail::ReaderTransport {
    bool present = true;
    std::uint32_t presenceResult = 0, openResult = 0, handshakeResult = 0;
    int queried = 0, opened = 0, requested = 0, closed = 0;
    std::size_t received = 32;
    ReaderHandshakeBuffer packet{};

    Transport() {
        field(packet, 0, NMSV_MAGIC);
        field(packet, 1, NMSV_PROTOCOL_VERSION);
        field(packet, 2, 32);
        field(packet, 3, NMSV_CAP_DIAGNOSTIC_ONLY);
    }
    std::uint32_t queryPresence(bool& out) override {
        ++queried; out = present; return presenceResult;
    }
    std::uint32_t open() override { ++opened; return openResult; }
    std::uint32_t getHandshake(ReaderHandshakeBuffer& bytes, std::size_t& count) override {
        ++requested; bytes = packet; count = received; return handshakeResult;
    }
    void close() override { ++closed; }
};

ReaderDiagnostics inspect(Transport& transport) {
    ReaderDiagnostics diagnostic;
    nmsaves::detail::inspectReaderService(transport, diagnostic);
    CHECK(!diagnostic.physicalAccessVerified);
    CHECK(transport.queried == 1);
    return diagnostic;
}

void expectRejected(Transport transport) {
    const auto diagnostic = inspect(transport);
    CHECK(diagnostic.state == ReaderState::Rejected);
    CHECK(diagnostic.servicePresent);
    CHECK(!diagnostic.protocolValid);
    CHECK(transport.opened == 1);
    CHECK(transport.requested == 1);
    CHECK(transport.closed == 1);
}

void checkTransportPaths() {
    Transport absent;
    absent.present = false;
    const auto missing = inspect(absent);
    CHECK(missing.state == ReaderState::Unavailable);
    CHECK(!missing.servicePresent && !missing.protocolValid);
    CHECK(absent.opened == 0 && absent.requested == 0 && absent.closed == 0);

    Transport queryError;
    queryError.presenceResult = 0x1234;
    const auto query = inspect(queryError);
    CHECK(query.state == ReaderState::Unavailable);
    CHECK(!query.servicePresent);
    CHECK(queryError.opened == 0 && queryError.requested == 0 && queryError.closed == 0);

    Transport openError;
    openError.openResult = 0x4321;
    const auto open = inspect(openError);
    CHECK(open.state == ReaderState::Unavailable);
    CHECK(open.servicePresent && !open.protocolValid);
    CHECK(openError.opened == 1 && openError.requested == 0 && openError.closed == 0);

    Transport ipcError;
    ipcError.handshakeResult = 0x1111;
    expectRejected(ipcError);

    Transport valid;
    const auto available = inspect(valid);
    CHECK(available.state == ReaderState::PresentUnverified);
    CHECK(available.servicePresent && available.protocolValid);
    CHECK(valid.opened == 1 && valid.requested == 1 && valid.closed == 1);

    Transport previouslyValidThenAbsent;
    previouslyValidThenAbsent.present = false;
    ReaderDiagnostics reused = available;
    reused.physicalAccessVerified = true; // stale state must not survive a refresh.
    nmsaves::detail::inspectReaderService(previouslyValidThenAbsent, reused);
    CHECK(reused.state == ReaderState::Unavailable);
    CHECK(!reused.servicePresent && !reused.protocolValid && !reused.physicalAccessVerified);
}

void checkMalformedHandshake() {
    for (const auto length : {std::size_t(0), std::size_t(1), std::size_t(31),
                              std::size_t(33), std::size_t(0xFFFFFFFF)}) {
        Transport transport;
        transport.received = length;
        expectRejected(transport);
    }
    for (const auto pair : {std::pair<std::size_t, std::uint32_t>{0, 0},
                            {1, 0}, {1, 2}, {2, 28}, {2, 36}, {3, 0},
                            {3, 0x80000001}, {5, 3}, {6, 1}, {7, 1}}) {
        Transport transport;
        field(transport.packet, pair.first, pair.second);
        expectRejected(transport);
    }
    Transport incoherent;
    field(incoherent.packet, 4, 512); // cannot advertise a read limit without the capability.
    expectRejected(incoherent);

    for (const auto backend : {NMSV_BACKEND_NONE, NMSV_BACKEND_HOST_FIXTURE}) {
        Transport transport;
        field(transport.packet, 3, NMSV_CAP_DIAGNOSTIC_ONLY | NMSV_CAP_PHYSICAL_RAW_READ_AVAILABLE);
        field(transport.packet, 4, 512);
        field(transport.packet, 5, backend);
        expectRejected(transport);
    }
    for (const auto limit : {0u, 511u, 513u, 65537u, 0xFFFFFFFFu}) {
        Transport transport;
        field(transport.packet, 3, NMSV_CAP_DIAGNOSTIC_ONLY | NMSV_CAP_PHYSICAL_RAW_READ_AVAILABLE);
        field(transport.packet, 4, limit);
        field(transport.packet, 5, NMSV_BACKEND_FS_ORIGINAL_ACCESSOR);
        expectRejected(transport);
    }
    for (const auto limit : {512u, 65536u}) {
        Transport transport;
        field(transport.packet, 3, NMSV_CAP_DIAGNOSTIC_ONLY | NMSV_CAP_PHYSICAL_RAW_READ_AVAILABLE);
        field(transport.packet, 4, limit);
        field(transport.packet, 5, NMSV_BACKEND_FS_ORIGINAL_ACCESSOR);
        const auto diagnostic = inspect(transport);
        CHECK(diagnostic.state == ReaderState::PresentUnverified);
        CHECK(diagnostic.protocolValid);
        CHECK(!diagnostic.physicalAccessVerified); // claimed capability is never validation.
        CHECK(transport.closed == 1);
    }
}

void checkConfiguration() {
    using Status = nmsaves::detail::EmummcConfiguration::Status;
    using nmsaves::detail::parseEmummcConfiguration;
    CHECK(parseEmummcConfiguration("[emummc]\nenabled=1\nsector=0x1234\n").status == Status::Enabled);
    CHECK(parseEmummcConfiguration("[emummc]\r\n enabled = 0 # preference\r\n").status == Status::Disabled);
    CHECK(parseEmummcConfiguration("enabled=1\n[unrelated]\nenabled=1\n").status == Status::NotSpecified);
    CHECK(parseEmummcConfiguration("; enabled=0\n[emummc]\n# enabled=0\nenabled=1; enabled\n").status == Status::Enabled);
    CHECK(parseEmummcConfiguration("[emummc]\nenabled=1\n[other]\nenabled=0\n").status == Status::Enabled);
    CHECK(parseEmummcConfiguration("[emummc]\nenabled=1\nenabled=0\n").status == Status::Invalid);
    CHECK(parseEmummcConfiguration("[emummc]\nenabled=1\nenabled=1\n").status == Status::Invalid);
    CHECK(parseEmummcConfiguration("[emummc]\nenabled=yes\n").status == Status::Invalid);
    CHECK(parseEmummcConfiguration(std::string(8193, 'x')).status == Status::Invalid);
    CHECK(parseEmummcConfiguration(std::string("[emummc]\nenabled=1\0", 20)).status == Status::Invalid);

}
} // namespace

int main() {
    checkTransportPaths();
    checkMalformedHandshake();
    checkConfiguration();
    std::printf("reader_client: %d checks passed\n", checks);
    return 0;
}
