// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "math/Table1D.h"
#include "math/Table2D.h"

using Catch::Matchers::WithinAbs;
using namespace fl;

// 2x2 grid: rows={0,1}, cols={0,1}, values={1,2,3,4} (row-major)
//   (0,0)=1  (0,1)=2
//   (1,0)=3  (1,1)=4
static Table2D makeSimple2x2() {
    Table2D t;
    t.rows = {0.f, 1.f};
    t.cols = {0.f, 1.f};
    t.values = {1.f, 2.f, 3.f, 4.f};
    return t;
}

TEST_CASE("Table2D exact corner values", "[table2d]") {
    auto t = makeSimple2x2();
    CHECK_THAT(t.lookup(0.f, 0.f), WithinAbs(1.f, 1e-5f));
    CHECK_THAT(t.lookup(0.f, 1.f), WithinAbs(2.f, 1e-5f));
    CHECK_THAT(t.lookup(1.f, 0.f), WithinAbs(3.f, 1e-5f));
    CHECK_THAT(t.lookup(1.f, 1.f), WithinAbs(4.f, 1e-5f));
}

TEST_CASE("Table2D bilinear midpoint", "[table2d]") {
    auto t = makeSimple2x2();
    // At (0.5, 0.5): lerp all four corners -> (1+2+3+4)/4 = 2.5
    CHECK_THAT(t.lookup(0.5f, 0.5f), WithinAbs(2.5f, 1e-5f));
}

TEST_CASE("Table2D edge clamping below range", "[table2d]") {
    auto t = makeSimple2x2();
    CHECK_THAT(t.lookup(-1.f, -1.f), WithinAbs(1.f, 1e-5f));
}

TEST_CASE("Table2D edge clamping above range", "[table2d]") {
    auto t = makeSimple2x2();
    CHECK_THAT(t.lookup(5.f, 5.f), WithinAbs(4.f, 1e-5f));
}

TEST_CASE("Table2D row-axis interpolation only", "[table2d]") {
    auto t = makeSimple2x2();
    // At col=0, row=0.5: lerp between row0 (1) and row1 (3) -> 2
    CHECK_THAT(t.lookup(0.5f, 0.f), WithinAbs(2.f, 1e-5f));
}

TEST_CASE("Table2D col-axis interpolation only", "[table2d]") {
    auto t = makeSimple2x2();
    // At row=0, col=0.5: lerp between (0,0)=1 and (0,1)=2 -> 1.5
    CHECK_THAT(t.lookup(0.f, 0.5f), WithinAbs(1.5f, 1e-5f));
}

// Table1D tests

TEST_CASE("Table1D exact endpoints", "[table1d]") {
    Table1D t;
    t.keys = {0.f, 1.f, 2.f};
    t.values = {10.f, 20.f, 40.f};
    CHECK_THAT(t.lookup(0.f), WithinAbs(10.f, 1e-5f));
    CHECK_THAT(t.lookup(1.f), WithinAbs(20.f, 1e-5f));
    CHECK_THAT(t.lookup(2.f), WithinAbs(40.f, 1e-5f));
}

TEST_CASE("Table1D linear midpoint", "[table1d]") {
    Table1D t;
    t.keys = {0.f, 2.f};
    t.values = {0.f, 4.f};
    CHECK_THAT(t.lookup(1.f), WithinAbs(2.f, 1e-5f));
}

TEST_CASE("Table1D clamping below range", "[table1d]") {
    Table1D t;
    t.keys = {1.f, 2.f};
    t.values = {5.f, 10.f};
    CHECK_THAT(t.lookup(0.f), WithinAbs(5.f, 1e-5f));
}

TEST_CASE("Table1D clamping above range", "[table1d]") {
    Table1D t;
    t.keys = {1.f, 2.f};
    t.values = {5.f, 10.f};
    CHECK_THAT(t.lookup(10.f), WithinAbs(10.f, 1e-5f));
}
