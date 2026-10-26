#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_METADATATRUST_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_METADATATRUST_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <vector>

namespace AgcDriver::Graphics {

class MetadataTrust {
public:
    static constexpr std::uint32_t Confirmations = 3;
    static constexpr std::uint32_t DefaultAudit = 64;
    struct Key {
        std::uint64_t writer = 0;
        std::uint64_t begin = 0;
        std::uint64_t bytes = 0;
        auto operator<=>(const Key&) const = default;
    };
    enum class Use : std::uint8_t { Untrusted, Skip, Audit };
    struct Counts {
        std::uint64_t skips = 0;
        std::uint64_t audits = 0;
        std::uint64_t confirmations = 0;
        std::uint64_t revocations = 0;
        std::uint64_t trusted = 0;
    };

    explicit MetadataTrust(std::uint32_t auditEvery = DefaultAudit);
    MetadataTrust(const MetadataTrust&) = delete;
    MetadataTrust& operator=(const MetadataTrust&) = delete;
    static bool Enabled();
    static std::uint32_t Level();
    static MetadataTrust& Process();
    std::uint32_t AuditEvery() const { return auditEvery; }
    bool Trusted(const Key& key) const;
    bool Revoked(const Key& key) const;
    std::vector<std::uint64_t> Writers(std::span<const std::uint64_t> writers, std::uint64_t begin, std::uint64_t bytes, bool learning) const;
    Use Consult(const Key& key, std::uint64_t note);
    bool Check(const Key& key, bool predicted);
    Counts Totals() const;

private:
    struct Entry {
        std::uint32_t confirmations = 0;
        std::uint32_t uses = 0;
        bool trusted = false;
        bool revoked = false;
        std::uint64_t note = 0;
        Use answer = Use::Untrusted;
    };

    std::uint32_t auditEvery;
    mutable std::mutex mutex;
    std::map<Key, Entry> entries;
    Counts counts;
};

}

#endif
