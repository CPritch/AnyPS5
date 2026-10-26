#include "prx/libSceAgcDriver/Graphics/include/MetadataTrust.hpp"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace AgcDriver::Graphics {
namespace {

std::uint32_t AuditFromEnvironment() {
    const char* value = std::getenv("APS5_TRUST_METADATA_AUDIT");
    if (value == nullptr) return MetadataTrust::DefaultAudit;
    std::uint32_t parsed = 0;
    const auto end = value + std::strlen(value);
    const auto result = std::from_chars(value, end, parsed);
    return result.ec == std::errc{} && result.ptr == end && parsed != 0 ? parsed : MetadataTrust::DefaultAudit;
}

}

MetadataTrust::MetadataTrust(std::uint32_t auditEvery) : auditEvery(auditEvery != 0 ? auditEvery : DefaultAudit) {}

bool MetadataTrust::Enabled() {
    return Level() != 0;
}

std::uint32_t MetadataTrust::Level() {
    static const std::uint32_t level = [] {
        const char* value = std::getenv("APS5_TRUST_METADATA_WRITERS");
        if (value == nullptr) return 1u;
        if (std::strcmp(value, "0") == 0) return 0u;
        return std::strcmp(value, "2") == 0 ? 2u : 1u;
    }();
    return level;
}

MetadataTrust& MetadataTrust::Process() {
    static auto* trust = new MetadataTrust(AuditFromEnvironment());
    return *trust;
}

bool MetadataTrust::Trusted(const Key& key) const {
    std::lock_guard lock(mutex);
    const auto found = entries.find(key);
    return found != entries.end() && found->second.trusted;
}

bool MetadataTrust::Revoked(const Key& key) const {
    std::lock_guard lock(mutex);
    const auto found = entries.find(key);
    return found != entries.end() && found->second.revoked;
}

std::vector<std::uint64_t> MetadataTrust::Writers(std::span<const std::uint64_t> writers, std::uint64_t begin, std::uint64_t bytes, bool learning) const {
    std::vector<std::uint64_t> chosen(writers.begin(), writers.end());
    std::sort(chosen.begin(), chosen.end());
    chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());
    std::lock_guard lock(mutex);
    std::erase_if(chosen, [&](std::uint64_t writer) {
        if (writer == 0) return true;
        const auto found = entries.find({writer, begin, bytes});
        if (found == entries.end()) return !learning;
        return learning ? found->second.revoked : !found->second.trusted;
    });
    return chosen;
}

MetadataTrust::Use MetadataTrust::Consult(const Key& key, std::uint64_t note) {
    std::lock_guard lock(mutex);
    const auto found = entries.find(key);
    if (found == entries.end() || !found->second.trusted) return Use::Untrusted;
    auto& entry = found->second;
    if (note != 0 && note == entry.note) return entry.answer;
    entry.note = note;
    if (entry.uses + 1 < auditEvery) {
        ++entry.uses;
        ++counts.skips;
        entry.answer = Use::Skip;
    } else {
        ++counts.audits;
        entry.answer = Use::Audit;
    }
    return entry.answer;
}

bool MetadataTrust::Check(const Key& key, bool predicted) {
    if (key.writer == 0) return false;
    std::lock_guard lock(mutex);
    auto& entry = entries[key];
    if (entry.revoked) return false;
    entry.uses = 0;
    if (predicted) {
        ++counts.confirmations;
        if (++entry.confirmations >= Confirmations && !entry.trusted) {
            entry.trusted = true;
            ++counts.trusted;
        }
        return false;
    }
    if (entry.trusted) --counts.trusted;
    entry = Entry{};
    entry.revoked = true;
    ++counts.revocations;
    std::fprintf(stderr, "[trust-meta] writer 0x%llx changed metadata 0x%llx+0x%llx: revoked\n", static_cast<unsigned long long>(key.writer), static_cast<unsigned long long>(key.begin), static_cast<unsigned long long>(key.bytes));
    return true;
}

MetadataTrust::Counts MetadataTrust::Totals() const {
    std::lock_guard lock(mutex);
    return counts;
}

}
