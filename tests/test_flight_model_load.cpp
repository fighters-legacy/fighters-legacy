// SPDX-License-Identifier: GPL-3.0-or-later
//
// THE REGRESSION TEST for #1232: loadAndParseFlightModel is the one load-and-parse step behind both
// the client resolver and the server spawn path, and its contract is that malformed content NEVER
// throws — the server used to call parseFlightModel uncaught and terminate on the first spawn of an
// entity type whose pack shipped a bad flight-model TOML, while the client fell back gracefully.

#include "mock_log.h"
#include <ILogger.h>
#include <content/AssetManager.h>
#include <content/IContentPack.h>
#include <flight/FlightModelData.h>
#include <flight/FlightModelLoad.h>
#include <mock_content.h>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace fl;

namespace {

// Serves whatever flight-model bytes the case puts in, byte-for-byte.
struct BytesPack : public NullContentPack {
    std::map<std::string, std::string> flightModels;

    bool hasAsset(const char* n, AssetType t) const override {
        return t == AssetType::FlightModel && flightModels.count(n) != 0;
    }
    std::optional<FlightModel> loadFlightModel(const char* n) override {
        auto it = flightModels.find(n);
        if (it == flightModels.end())
            return std::nullopt;
        FlightModel d;
        d.name = n;
        d.bytes.assign(it->second.begin(), it->second.end());
        return d;
    }
};

std::unique_ptr<AssetManager> makeAssets(BytesPack pack, ILogger& log) {
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::make_unique<BytesPack>(std::move(pack)));
    auto am = std::make_unique<AssetManager>(std::move(packs), log);
    am->initialize(nullptr);
    return am;
}

// Same minimal-valid model as test_flight_model_parser.cpp's kMinimalToml (a private copy each —
// candidate for the #1276 shared-fixture fold).
const char* kMinimalToml = R"(
[aircraft]
name         = "Test Fighter"
type         = "fighter"
engine_type  = "turbofan"
has_fbw      = false
cruise_alt_m = 10000.0

[flight_model]
mass_kg      = 10000.0
wing_area_m2 = 35.0
wingspan_m   = 10.0
mac_m        = 3.5
fuel_kg      = 4000.0
ixx_kg_m2    = 10000.0
iyy_kg_m2    = 70000.0
izz_kg_m2    = 78000.0

[aero.cl_table]
alpha  = [-5.0, 0.0, 5.0, 10.0, 15.0]
mach   = [0.3, 0.9]
values = [
    -0.2, -0.2,
     0.05, 0.05,
     0.4,  0.4,
     0.75, 0.75,
     1.05, 1.05,
]

[aero.drag_polar]
cd0           = 0.018
k             = 0.14
speedbrake_cd = 0.07
gear_cd       = 0.03

[aero.moments]
cm_alpha = -0.7
cm_q     = -10.0
cm_de    = -1.0
cl_beta  = -0.08
cl_p     = -0.40
cl_da    =  0.07
cn_beta  =  0.10
cn_r     = -0.12
cn_dr    = -0.05

[aero.limits]
alpha_stall_deg  = 15.0
max_g_structural =  8.0
min_g_structural = -3.0
max_mach         =  1.6

[aero.controls]
max_elevator_deg = 25.0
max_aileron_deg  = 20.0
max_rudder_deg   = 30.0

[engine]
fuel_flow_idle_kg_s = 0.1
fuel_flow_mil_kg_s  = 1.0
fuel_flow_ab_kg_s   = 3.0
spool_time_s        = 5.0

[engine.mil_thrust]
mach   = [0.0, 0.9]
alt_km = [0.0, 12.0]
values = [60.0, 30.0,
          65.0, 33.0]
)";

} // namespace

TEST_CASE("loadAndParseFlightModel: a valid model parses to a non-null model and no error") {
    NullLogger log;
    BytesPack pack;
    pack.flightModels["good"] = kMinimalToml;
    auto assets = makeAssets(std::move(pack), log);

    FlightModelLoadResult res;
    REQUIRE_NOTHROW(res = loadAndParseFlightModel(*assets, "good"));
    REQUIRE(res.model != nullptr);
    CHECK(res.error.empty());
}

TEST_CASE("loadAndParseFlightModel: missing asset is a null model with a reason, not a throw") {
    NullLogger log;
    auto assets = makeAssets(BytesPack{}, log);

    FlightModelLoadResult res;
    REQUIRE_NOTHROW(res = loadAndParseFlightModel(*assets, "does-not-exist"));
    CHECK(res.model == nullptr);
    CHECK(res.error.find("does-not-exist") != std::string::npos);
    CHECK(res.error.find("no loaded content pack") != std::string::npos);
}

TEST_CASE("loadAndParseFlightModel: empty asset bytes are a null model, not a throw") {
    NullLogger log;
    BytesPack pack;
    pack.flightModels["empty"] = "";
    auto assets = makeAssets(std::move(pack), log);

    FlightModelLoadResult res;
    REQUIRE_NOTHROW(res = loadAndParseFlightModel(*assets, "empty"));
    CHECK(res.model == nullptr);
    CHECK(!res.error.empty());
}

TEST_CASE("loadAndParseFlightModel: TOML syntax errors are a null model with the parser's reason") {
    NullLogger log;
    BytesPack pack;
    pack.flightModels["broken"] = "[aircraft\nname = "; // unterminated table header
    auto assets = makeAssets(std::move(pack), log);

    FlightModelLoadResult res;
    REQUIRE_NOTHROW(res = loadAndParseFlightModel(*assets, "broken"));
    CHECK(res.model == nullptr);
    CHECK(res.error.find("broken") != std::string::npos);
    CHECK(res.error.find("failed to parse") != std::string::npos);
}

TEST_CASE("loadAndParseFlightModel: valid TOML missing required fields is a null model, not a throw") {
    NullLogger log;
    BytesPack pack;
    pack.flightModels["hollow"] = "[aircraft]\nname = \"Hollow\"\n"; // parses as TOML, fails the schema
    auto assets = makeAssets(std::move(pack), log);

    FlightModelLoadResult res;
    REQUIRE_NOTHROW(res = loadAndParseFlightModel(*assets, "hollow"));
    CHECK(res.model == nullptr);
    CHECK(res.error.find("failed to parse") != std::string::npos);
}
