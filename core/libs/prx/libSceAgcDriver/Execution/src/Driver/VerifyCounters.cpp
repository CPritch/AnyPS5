#include "prx/libSceAgcDriver/Execution/include/Driver/VerifyCounters.hpp"
#include <atomic>
#include <cstdlib>

namespace AgcDriver::DriverDetail {

namespace {

struct Entry {
    const char* field;
    const char* name;
};

constexpr std::array<Entry, VerifyCountSize> Entries{{
    {"vf_packet_overlap_checked", "APS5_VERIFY_PACKET_OVERLAP_MEMO"},
    {"vf_packet_overlap_skipped", "APS5_VERIFY_PACKET_OVERLAP_MEMO"},
    {"vf_held_writes_checked", "APS5_VERIFY_HELD_WRITES"},
    {"vf_held_writes_skipped", "APS5_VERIFY_HELD_WRITES"},
    {"vf_pending_write_index_checked", "APS5_VERIFY_PENDING_WRITE_INDEX"},
    {"vf_pending_write_index_skipped", "APS5_VERIFY_PENDING_WRITE_INDEX"},
    {"vf_register_key_memo_checked", "APS5_VERIFY_REGISTER_KEY"},
    {"vf_register_key_hash_checked", "APS5_VERIFY_REGISTER_KEY"},
    {"vf_register_key_skipped", "APS5_VERIFY_REGISTER_KEY"},
    {"vf_holder_note_checked", "APS5_VERIFY_HOLDER_NOTE"},
    {"vf_holder_note_unnamed", "APS5_VERIFY_HOLDER_NOTE"},
    {"vf_unlocked_import_checked", "APS5_VERIFY_UNLOCKED_IMPORT"},
    {"vf_unlocked_import_skipped", "APS5_VERIFY_UNLOCKED_IMPORT"},
    {"vf_early_import_barrier_checked", "APS5_VERIFY_EARLY_IMPORT_BARRIER"},
    {"vf_early_import_barrier_skipped", "APS5_VERIFY_EARLY_IMPORT_BARRIER"},
    {"vf_journal_reconcile_decisions", "APS5_VERIFY_JOURNAL_RECONCILE"},
    {"vf_journal_reconcile_covers", "APS5_VERIFY_JOURNAL_RECONCILE"},
    {"vf_journal_reconcile_invariants", "APS5_VERIFY_JOURNAL_RECONCILE"},
    {"vf_journal_reconcile_unsettled", "APS5_VERIFY_JOURNAL_RECONCILE"},
    {"vf_journal_reconcile_survivals", "APS5_VERIFY_JOURNAL_RECONCILE"},
    {"vf_journal_reconcile_skipped", "APS5_VERIFY_JOURNAL_RECONCILE"},
    {"vf_region_check_checked", "APS5_VERIFY_REGION_CHECK"},
    {"vf_region_check_skipped", "APS5_VERIFY_REGION_CHECK"},
}};

std::array<std::atomic<std::uint64_t>, VerifyCountSize> totals{};

}

void CountVerify(VerifyCount which, std::uint64_t amount) {
    totals[static_cast<std::size_t>(which)].fetch_add(amount, std::memory_order_relaxed);
}

VerifyCounts VerifyTotals() {
    VerifyCounts counts{};
    for (std::size_t index = 0; index < counts.size(); ++index) counts[index] = totals[index].load(std::memory_order_relaxed);
    return counts;
}

bool VerifyRequested(VerifyCount which) {
    static const auto requested = [] {
        std::array<bool, VerifyCountSize> set{};
        for (std::size_t index = 0; index < set.size(); ++index) set[index] = std::getenv(Entries[index].name) != nullptr;
        return set;
    }();
    return requested[static_cast<std::size_t>(which)];
}

bool AnyVerifyRequested() {
    for (std::size_t index = 0; index < VerifyCountSize; ++index) {
        if (VerifyRequested(static_cast<VerifyCount>(index))) return true;
    }
    return false;
}

const char* VerifyFieldName(VerifyCount which) {
    return Entries[static_cast<std::size_t>(which)].field;
}

const char* VerifySwitchName(VerifyCount which) {
    return Entries[static_cast<std::size_t>(which)].name;
}

}
