#include "oreslang/runtime/NativeEtsScalarSet.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>

using oreslang::runtime::NativeEtsScalarSet;
using Table = NativeEtsScalarSet;

static void owner_death_handoff_preserves_data_and_excludes_dual_writers() {
    const Table::Principal owner{11, 22, 33, 1};
    const Table::Principal heir{11, 22, 34, 1};
    const Table::Principal peer{11, 22, 35, 1};
    const Table::Principal foreign{11, 99, 44, 1};
    Table table(owner, Table::Protection::protected_table, 4);
    const auto identity = table.handle();

    assert(table.insert(identity, owner, 7, 42) == Table::Status::ok);
    assert(table.lookup(identity, peer, 7).value == 42);
    assert(table.insert(identity, peer, 7, 500) == Table::Status::denied);
    assert(table.table_epoch() == 1);

    auto receipt = table.retire_owner_with_heir(owner, heir);
    assert(receipt.has_value());
    assert(receipt->table.incarnation == identity.incarnation);
    assert(table.table_epoch() == 1); // not yet acknowledged/published
    assert(table.lookup(identity, owner, 7).status == Table::Status::denied);
    assert(table.lookup(identity, heir, 7).status == Table::Status::denied);
    assert(table.lookup(identity, peer, 7).status == Table::Status::denied);
    assert(table.insert(identity, owner, 7, 501) == Table::Status::denied);
    assert(table.insert(identity, heir, 7, 502) == Table::Status::denied);
    assert(table.insert(identity, foreign, 7, 503) == Table::Status::denied);

    auto forgery = *receipt;
    ++forgery.receipt_nonce;
    assert(!table.acknowledge_heir(forgery, heir));
    assert(!table.acknowledge_heir(*receipt, peer));
    assert(!table.acknowledge_heir(*receipt, foreign));
    assert(!table.retire_owner(owner));
    assert(!table.retire_owner_with_heir(owner, peer));
    assert(table.lookup(identity, heir, 7).status == Table::Status::denied);
    // Even the correct heir cannot publish before the trusted OresVM has
    // separately verified old actor retirement and outstanding lease drain.
    assert(!table.acknowledge_heir(*receipt, heir));
    assert(!table.confirm_owner_retired(forgery, owner));
    assert(!table.confirm_owner_retired(*receipt, peer));
    assert(table.confirm_owner_retired(*receipt, owner));
    assert(!table.confirm_owner_retired(*receipt, owner));

    assert(table.acknowledge_heir(*receipt, heir));
    assert(table.table_epoch() == 2);
    assert(table.handle().incarnation == identity.incarnation);
    assert(!table.acknowledge_heir(*receipt, heir));
    assert(!table.abandon_heir(*receipt));
    assert(table.lookup(identity, owner, 7).status == Table::Status::denied);
    assert(table.insert(identity, owner, 7, 10) == Table::Status::denied);
    assert(table.lookup(identity, heir, 7).value == 42);
    assert(table.lookup(identity, peer, 7).value == 42);
    assert(table.insert(identity, heir, 7, 43) == Table::Status::ok);
    assert(table.lookup(identity, peer, 7).value == 43);
    assert(table.insert(identity, foreign, 7, 99) == Table::Status::denied);
    assert(!table.retire_owner(owner));
    assert(table.retire_owner(heir));
    assert(table.lookup(identity, heir, 7).status == Table::Status::retired);
}

static void invalid_heir_and_failure_quarantine_close() {
    const Table::Principal owner{1, 2, 3, 1};
    const Table::Principal heir{1, 2, 4, 1};
    const Table::Principal outside{1, 6, 4, 1};
    Table table(owner, Table::Protection::public_table, 2);
    const auto h = table.handle();
    assert(table.insert(h, owner, 1, 123) == Table::Status::ok);
    assert(!table.retire_owner_with_heir(owner, outside));
    assert(!table.retire_owner_with_heir(owner, owner));
    assert(table.lookup(h, heir, 1).value == 123);
    auto receipt = table.retire_owner_with_heir(owner, heir);
    assert(receipt.has_value());
    assert(!table.retire_owner_with_heir(owner, heir));
    assert(!table.abandon_heir(Table::HeirReceipt{
        receipt->table, receipt->deceased_owner, receipt->successor,
        receipt->table_epoch, receipt->receipt_nonce + 1
    }));
    assert(table.abandon_heir(*receipt));
    assert(table.lookup(h, owner, 1).status == Table::Status::retired);
    assert(table.lookup(h, heir, 1).status == Table::Status::retired);
    assert(!table.acknowledge_heir(*receipt, heir));
    assert(!table.retire_owner(owner));
}

static void public_table_blocks_all_previous_dead_generations() {
    Table::Principal owner{11, 22, 10, 1};
    Table table(owner, Table::Protection::public_table, 2);
    const auto h = table.handle();
    assert(table.insert(h, owner, 1, 42) == Table::Status::ok);

    // Exactly sixty-four distinct owner deaths are representable. Earlier
    // owners never regain PUBLIC read/write rights, even after many upgrades.
    for (std::uint64_t index = 0; index < 64; ++index) {
        const Table::Principal heir{11, 22, 11 + index, 1};
        auto ticket = table.retire_owner_with_heir(owner, heir);
        assert(ticket.has_value());
        assert(table.lookup(h, owner, 1).status == Table::Status::denied);
        assert(!table.acknowledge_heir(*ticket, heir));
        assert(table.confirm_owner_retired(*ticket, owner));
        assert(table.acknowledge_heir(*ticket, heir));
        assert(table.lookup(h, owner, 1).status == Table::Status::denied);
        assert(table.insert(h, owner, 1, 1000) == Table::Status::denied);
        assert(table.lookup(h, heir, 1).value == 42);
        assert(table.table_epoch() == 2 + index);
        owner = heir;
    }
    const Table::Principal after_budget{11, 22, 1000, 1};
    assert(!table.retire_owner_with_heir(owner, after_budget));
    assert(table.lookup(h, owner, 1).value == 42);
    assert(table.table_epoch() == 65);
    assert(table.retire_owner(owner));
    assert(table.lookup(h, owner, 1).status == Table::Status::retired);
}

static void stale_table_handle_and_wrong_runtime_always_reject() {
    const Table::Principal owner{22, 33, 44, 1};
    const Table::Principal heir{22, 33, 45, 1};
    Table table(owner, Table::Protection::private_table, 2);
    auto handle = table.handle();
    assert(table.insert(handle, owner, 5, 6) == Table::Status::ok);
    Table::Handle bad{handle.incarnation + 1};
    assert(table.lookup(bad, owner, 5).status == Table::Status::invalid);
    assert(table.lookup(handle, Table::Principal{55,33,45,1}, 5).status
           == Table::Status::denied);
    auto receipt = table.retire_owner_with_heir(owner, heir);
    assert(receipt.has_value());
    assert(!table.acknowledge_heir(*receipt, owner));
    assert(!table.acknowledge_heir(*receipt, heir));
    assert(table.confirm_owner_retired(*receipt, owner));
    assert(table.acknowledge_heir(*receipt, heir));
    assert(table.lookup(bad, heir, 5).status == Table::Status::invalid);
    assert(table.lookup(handle, owner, 5).status == Table::Status::denied);
    assert(table.lookup(handle, heir, 5).value == 6);
}

int main() {
    for (int round = 0; round < 1000; ++round) {
        owner_death_handoff_preserves_data_and_excludes_dual_writers();
        invalid_heir_and_failure_quarantine_close();
        stale_table_handle_and_wrong_runtime_always_reject();
    }
    public_table_blocks_all_previous_dead_generations();
    std::cout << "UHL1_NATIVE_ETS_HEIR_HOSTONLY_PASS=3001\n";
}
