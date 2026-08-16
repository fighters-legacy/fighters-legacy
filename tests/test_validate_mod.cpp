// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "crypto/Sha256.h"
#include "mod_validator.h"
#include "temp_path.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace fl;
namespace fs = std::filesystem;

namespace {
void writeText(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
bool hasSubstr(const std::vector<std::string>& v, const std::string& needle) {
    for (const auto& s : v)
        if (s.find(needle) != std::string::npos)
            return true;
    return false;
}
// A minimal pack with a valid manifest and no assets.
void writeManifest(const fs::path& root, const std::string& body) {
    writeText(root / "manifest.toml", body);
}
const char* kGoodManifest = "[mod]\nid = \"test\"\nname = \"test\"\nversion = \"1.0\"\n"
                            "engine-api = \"1.0\"\npriority = 50\nnamespace = \"test\"\n";

// A complete, valid light-fighter flight model — enough for the trim math to converge, which the
// expectation gate needs in order to have an opinion at all.
const char* kFighterToml = R"(
[aircraft]
name         = "Test Fighter"
type         = "fighter"
engine_type  = "turbojet"
has_fbw      = false
cruise_alt_m = 11000.0

[flight_model]
mass_kg      = 4349.0
wing_area_m2 = 17.28
wingspan_m   = 8.13
mac_m        = 2.44
fuel_kg      = 2000.0
ixx_kg_m2    = 3800.0
iyy_kg_m2    = 25000.0
izz_kg_m2    = 27000.0

[aero.cl_table]
alpha  = [-5.0, 0.0, 5.0, 10.0, 15.0, 18.0, 22.0, 26.0]
mach   = [0.3, 0.6, 0.9, 1.2]
values = [
    -0.25,-0.26,-0.28,-0.20,
     0.05, 0.05, 0.06, 0.04,
     0.40, 0.43, 0.48, 0.35,
     0.78, 0.84, 0.92, 0.66,
     1.10, 1.18, 1.28, 0.92,
     1.24, 1.32, 1.42, 1.02,
     1.15, 1.22, 1.30, 0.94,
     0.95, 1.00, 1.08, 0.78,
]

[aero.drag_polar]
cd0           = 0.0200
k             = 0.115
speedbrake_cd = 0.06
gear_cd       = 0.025

[aero.moments]
cm_alpha = -0.6
cm_q     = -9.0
cm_de    = -0.9
cl_beta  = -0.09
cl_p     = -0.38
cl_da    =  0.08
cn_beta  =  0.12
cn_r     = -0.14
cn_dr    = -0.06

[aero.limits]
alpha_stall_deg  = 18.0
max_g_structural =  7.33
min_g_structural = -3.0
max_mach         =  1.63

[aero.controls]
max_elevator_deg = 25.0
max_aileron_deg  = 20.0
max_rudder_deg   = 30.0

[engine]
fuel_flow_idle_kg_s = 0.05
fuel_flow_mil_kg_s  = 0.52
fuel_flow_ab_kg_s   = 1.55
spool_time_s        = 4.0

[engine.mil_thrust]
mach   = [0.0, 0.6, 0.9, 1.2, 1.6]
alt_km = [0.0, 6.0, 11.0, 15.0]
values = [
    22.2, 12.0,  6.4, 3.4,
    21.0, 12.6,  7.0, 3.8,
    20.4, 13.0,  7.6, 4.2,
    19.0, 12.6,  7.6, 4.3,
    16.6, 11.4,  7.0, 4.0,
]

[engine.ab_thrust]
mach   = [0.0, 0.6, 0.9, 1.2, 1.6]
alt_km = [0.0, 6.0, 11.0, 15.0]
values = [
    31.2, 17.4,  9.6, 5.2,
    33.0, 19.6, 11.0, 6.0,
    36.0, 22.0, 12.6, 7.0,
    38.0, 24.4, 14.4, 8.2,
    36.0, 24.0, 14.6, 8.6,
]
)";

// A JSON-chunk-only GLB. Valid glTF 2.0 — a cockpit anchor file carries no geometry at all (engine
// #844), so this is exactly the shape the convention produces.
std::vector<uint8_t> jsonGlb(const std::string& json) {
    std::string chunk = json;
    while (chunk.size() % 4 != 0)
        chunk += ' ';
    const uint32_t chunkLen = static_cast<uint32_t>(chunk.size());
    const uint32_t total = 12u + 8u + chunkLen;

    std::vector<uint8_t> b;
    auto put32 = [&b](uint32_t v) {
        b.push_back(static_cast<uint8_t>(v & 0xFF));
        b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };
    for (char c : std::string("glTF"))
        b.push_back(static_cast<uint8_t>(c));
    put32(2);
    put32(total);
    put32(chunkLen);
    put32(0x4E4F534Au); // "JSON"
    for (char c : chunk)
        b.push_back(static_cast<uint8_t>(c));
    return b;
}

void writeBytes(const fs::path& p, const std::vector<uint8_t>& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// A cockpit GLB with the named nodes and no meshes.
std::vector<uint8_t> cockpitGlb(const std::vector<std::string>& nodeNames) {
    std::string nodes;
    for (size_t i = 0; i < nodeNames.size(); ++i)
        nodes += (i ? ",{\"name\":\"" : "{\"name\":\"") + nodeNames[i] + "\"}";
    std::string scene;
    for (size_t i = 0; i < nodeNames.size(); ++i)
        scene += (i ? "," : "") + std::to_string(i);
    return jsonGlb("{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
                   "\"scenes\":[{\"nodes\":[" +
                   scene + "]}],\"nodes\":[" + nodes + "]}");
}
} // namespace

TEST_CASE("validate-mod: a minimal valid pack passes (no license check)", "[validate-mod]") {
    test::TempDirGuard dir{"fl-mod"};
    writeManifest(dir.path(), kGoodManifest);
    ModValidateOptions o;
    o.checkLicenses = false;
    auto r = validateMod(dir.path().string(), o);
    if (!r.errors.empty())
        UNSCOPED_INFO("first: " << r.errors[0]);
    CHECK(r.ok);
}

TEST_CASE("validate-mod: a missing manifest is an error", "[validate-mod]") {
    test::TempDirGuard dir{"fl-mod"};
    auto r = validateMod(dir.path().string());
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "no manifest.toml"));
}

TEST_CASE("validate-mod: an incompatible engine-api is an error", "[validate-mod]") {
    test::TempDirGuard dir{"fl-mod"};
    writeManifest(dir.path(), "[mod]\nid = \"t\"\nname = \"t\"\nversion = \"1.0\"\n"
                              "engine-api = \"2.0\"\npriority = 1\n");
    ModValidateOptions o;
    o.checkLicenses = false;
    auto r = validateMod(dir.path().string(), o);
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "engine-api"));
}

TEST_CASE("validate-mod: an unknown top-level directory warns", "[validate-mod]") {
    test::TempDirGuard dir{"fl-mod"};
    writeManifest(dir.path(), kGoodManifest);
    fs::create_directories(dir.path() / "junk");
    ModValidateOptions o;
    o.checkLicenses = false;
    auto r = validateMod(dir.path().string(), o);
    CHECK(hasSubstr(r.warnings, "'junk'"));
}

TEST_CASE("validate-mod: [files] sha256 mismatch and match", "[validate-mod]") {
    test::TempDirGuard dir{"fl-mod"};
    writeText(dir.path() / "data" / "note.txt", "hello");
    const std::string good = sha256Hex(std::string("hello").data(), 5);

    // Match -> clean.
    writeManifest(dir.path(), std::string(kGoodManifest) + "\n[files]\n\"data/note.txt\" = \"" + good + "\"\n");
    ModValidateOptions o;
    o.checkLicenses = false;
    CHECK(validateMod(dir.path().string(), o).ok);

    // Mismatch -> error.
    std::string wrong = good;
    wrong[0] = (wrong[0] == 'a') ? 'b' : 'a';
    writeManifest(dir.path(), std::string(kGoodManifest) + "\n[files]\n\"data/note.txt\" = \"" + wrong + "\"\n");
    auto r = validateMod(dir.path().string(), o);
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "sha256 mismatch"));

    // A [files] entry for a missing file -> error.
    writeManifest(dir.path(), std::string(kGoodManifest) + "\n[files]\n\"data/gone.txt\" = \"" + good + "\"\n");
    auto r2 = validateMod(dir.path().string(), o);
    CHECK_FALSE(r2.ok);
    CHECK(hasSubstr(r2.errors, "does not exist"));
}

TEST_CASE("validate-mod: a broken campaign in missions/ is caught with the campaigns: prefix", "[validate-mod]") {
    test::TempDirGuard dir{"fl-mod"};
    writeManifest(dir.path(), kGoodManifest);
    // A campaign (has story:) with a dangling next.id.
    writeText(dir.path() / "missions" / "camp.yaml",
              "name: C\nsides: [a, b]\npilot:\n  side: a\n"
              "story:\n  - id: s1\n    file: m.yaml\n    trigger: campaign_start\n"
              "    on_complete:\n      next:\n        id: ghost\n");
    ModValidateOptions o;
    o.checkLicenses = false;
    auto r = validateMod(dir.path().string(), o);
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "campaigns:"));
}

// ---------------------------------------------------------------------------
// Pack conventions the whole-pack check used to reject (#1104)
// ---------------------------------------------------------------------------

TEST_CASE("validate-mod: an expect fixture is gated, not parsed as a flight model (#1104)", "[validate-mod]") {
    // aircraft/**/*.toml is two file kinds. Running the flight-model validator over an fm-trim
    // expectation fixture reported eight missing tables per file and declared a conforming pack
    // invalid, which is what kept fl-base-pack from adopting validate-mod as its gate.
    test::TempDirGuard dir{"fl-mod"};
    writeManifest(dir.path(), kGoodManifest);
    writeText(dir.path() / "aircraft" / "tf" / "tf.toml", kFighterToml);

    // A tolerance wide enough that any converged model meets it: this row is here to prove the
    // fixture is ACCEPTED, not to pin a number.
    writeText(dir.path() / "aircraft" / "tf" / "tf.expect.toml",
              "[[expect]]\nmetric = \"stall_speed_1g_mps\"\naltitude_m = 4572.0\n"
              "mass_kg = 6500.0\nexpected = 90.0\ntolerance = 0.5\n");

    ModValidateOptions o;
    o.checkLicenses = false;
    auto r = validateMod(dir.path().string(), o);
    if (!r.errors.empty())
        UNSCOPED_INFO("first: " << r.errors[0]);
    CHECK(r.ok);
    CHECK_FALSE(hasSubstr(r.errors, "missing [flight_model] table"));
    CHECK_FALSE(hasSubstr(r.errors, "missing [aircraft] table"));
}

TEST_CASE("validate-mod: an expect fixture the model MISSES is an error (#1104)", "[validate-mod]") {
    // THE PROOF THAT THE GATE RUNS. Skipping these files would pass this test too, so the fixture
    // is deliberately unmeetable: a 30 m/s stall on a 6.5 t fighter. If validate-mod ever goes back
    // to ignoring expectation files, this goes green and says so.
    test::TempDirGuard dir{"fl-mod"};
    writeManifest(dir.path(), kGoodManifest);
    writeText(dir.path() / "aircraft" / "tf" / "tf.toml", kFighterToml);
    writeText(dir.path() / "aircraft" / "tf" / "tf.expect.toml",
              "[[expect]]\nmetric = \"stall_speed_1g_mps\"\naltitude_m = 4572.0\n"
              "mass_kg = 6500.0\nexpected = 30.0\ntolerance = 0.02\n");

    ModValidateOptions o;
    o.checkLicenses = false;
    auto r = validateMod(dir.path().string(), o);
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "expectations:"));
    CHECK(hasSubstr(r.errors, "stall_speed_1g_mps"));
}

TEST_CASE("validate-mod: an expect fixture with no flight model beside it is an error (#1104)", "[validate-mod]") {
    test::TempDirGuard dir{"fl-mod"};
    writeManifest(dir.path(), kGoodManifest);
    writeText(dir.path() / "aircraft" / "tf" / "tf.expect.toml",
              "[[expect]]\nmetric = \"stall_speed_1g_mps\"\naltitude_m = 0.0\n"
              "mass_kg = 6500.0\nexpected = 90.0\ntolerance = 0.5\n");

    ModValidateOptions o;
    o.checkLicenses = false;
    auto r = validateMod(dir.path().string(), o);
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "no flight model"));
}

TEST_CASE("validate-mod: a cockpit anchor file is checked for its anchor, not its meshes (#1104)", "[validate-mod]") {
    // A `_cockpit.glb` is a camera anchor: it legitimately carries no geometry (engine #844), so the
    // mesh validator's "no meshes found" was a false rejection of the documented convention.
    test::TempDirGuard dir{"fl-mod"};
    writeManifest(dir.path(), kGoodManifest);
    writeBytes(dir.path() / "aircraft" / "tf" / "tf_cockpit.glb", cockpitGlb({"camera_anchor"}));

    ModValidateOptions o;
    o.checkLicenses = false;
    auto r = validateMod(dir.path().string(), o);
    if (!r.errors.empty())
        UNSCOPED_INFO("first: " << r.errors[0]);
    CHECK(r.ok);
    CHECK_FALSE(hasSubstr(r.errors, "no meshes found"));
}

TEST_CASE("validate-mod: a cockpit file without a camera_anchor is an error (#1104)", "[validate-mod]") {
    // The other half of not-skipping: the anchor IS the reason the file exists, so its absence is a
    // real defect the pack author wants told. A plain skip would have hidden this.
    test::TempDirGuard dir{"fl-mod"};
    writeManifest(dir.path(), kGoodManifest);
    writeBytes(dir.path() / "aircraft" / "tf" / "tf_cockpit.glb", cockpitGlb({"panel", "seat"}));

    ModValidateOptions o;
    o.checkLicenses = false;
    auto r = validateMod(dir.path().string(), o);
    CHECK_FALSE(r.ok);
    CHECK(hasSubstr(r.errors, "camera_anchor"));
}
