// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "EntitySelector.h"

#include <vector>

using fl::EntityRenderEntry;
using fl::EntitySelector;

namespace {
EntityRenderEntry ent(uint32_t idx, uint32_t gen) {
    EntityRenderEntry e;
    e.entityIdx = idx;
    e.entityGen = gen;
    return e;
}
} // namespace

TEST_CASE("EntitySelector: empty until a cycle, no selection with no entities", "[entity_selector]") {
    EntitySelector sel;
    CHECK_FALSE(sel.hasSelection());

    std::vector<EntityRenderEntry> none;
    sel.cycleNext(none);
    CHECK_FALSE(sel.hasSelection()); // nothing to select
    CHECK(sel.resolve(none) == nullptr);
}

TEST_CASE("EntitySelector: cycleNext walks ascending idx and wraps", "[entity_selector]") {
    // Deliberately out of order to prove cycling is by idx, not vector position.
    std::vector<EntityRenderEntry> es{ent(9, 1), ent(2, 1), ent(5, 1)};
    EntitySelector sel;

    sel.cycleNext(es); // no selection -> lowest idx
    CHECK(sel.selectedIdx() == 2u);
    sel.cycleNext(es);
    CHECK(sel.selectedIdx() == 5u);
    sel.cycleNext(es);
    CHECK(sel.selectedIdx() == 9u);
    sel.cycleNext(es); // wrap back to lowest
    CHECK(sel.selectedIdx() == 2u);
}

TEST_CASE("EntitySelector: cyclePrev walks descending idx and wraps", "[entity_selector]") {
    std::vector<EntityRenderEntry> es{ent(2, 1), ent(9, 1), ent(5, 1)};
    EntitySelector sel;

    sel.cyclePrev(es); // no selection -> highest idx
    CHECK(sel.selectedIdx() == 9u);
    sel.cyclePrev(es);
    CHECK(sel.selectedIdx() == 5u);
    sel.cyclePrev(es);
    CHECK(sel.selectedIdx() == 2u);
    sel.cyclePrev(es); // wrap back to highest
    CHECK(sel.selectedIdx() == 9u);
}

TEST_CASE("EntitySelector: resolve returns the matching entry, nullptr when gone", "[entity_selector]") {
    std::vector<EntityRenderEntry> es{ent(2, 7), ent(5, 3)};
    EntitySelector sel;
    sel.select(5, 3);

    const EntityRenderEntry* r = sel.resolve(es);
    REQUIRE(r != nullptr);
    CHECK(r->entityIdx == 5u);

    // The entity is destroyed: its pool slot is reused by a NEW entity (same idx, new gen). A stale
    // {idx, gen} handle must NOT resolve to the impostor -- the caller falls back to the free camera.
    std::vector<EntityRenderEntry> reused{ent(2, 7), ent(5, 4)};
    CHECK(sel.resolve(reused) == nullptr);

    // Entity simply absent from the snapshot (interest-out / despawned).
    std::vector<EntityRenderEntry> gone{ent(2, 7)};
    CHECK(sel.resolve(gone) == nullptr);
}

TEST_CASE("EntitySelector: a gen-0 (invalid) entry is never selected", "[entity_selector]") {
    std::vector<EntityRenderEntry> es{ent(1, 0), ent(4, 2)};
    EntitySelector sel;
    sel.cycleNext(es);
    CHECK(sel.selectedIdx() == 4u); // skipped the invalid idx-1 handle
    CHECK(sel.selectedGen() == 2u);
}

TEST_CASE("EntitySelector: clear drops the selection", "[entity_selector]") {
    EntitySelector sel;
    sel.select(3, 1);
    CHECK(sel.hasSelection());
    sel.clear();
    CHECK_FALSE(sel.hasSelection());
    std::vector<EntityRenderEntry> es{ent(3, 1)};
    CHECK(sel.resolve(es) == nullptr);
}
