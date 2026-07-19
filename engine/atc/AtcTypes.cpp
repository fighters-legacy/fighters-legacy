// SPDX-License-Identifier: GPL-3.0-or-later
#include "atc/AtcTypes.h"

namespace fl::atc {

const char* clearanceStateName(ClearanceState s) noexcept {
    switch (s) {
    case ClearanceState::None:
        return "none";
    case ClearanceState::HoldShort:
        return "hold_short";
    case ClearanceState::ClearedTakeoff:
        return "cleared_takeoff";
    case ClearanceState::Departed:
        return "departed";
    case ClearanceState::Inbound:
        return "inbound";
    case ClearanceState::Pattern:
        return "pattern";
    case ClearanceState::ClearedToLand:
        return "cleared_to_land";
    case ClearanceState::GoAround:
        return "go_around";
    case ClearanceState::Landed:
        return "landed";
    }
    return "none";
}

const char* atcPhraseVoiceKey(AtcPhrase p) noexcept {
    switch (p) {
    case AtcPhrase::HoldShort:
        return "atc.hold_short";
    case AtcPhrase::ClearedTakeoff:
        return "atc.cleared_takeoff";
    case AtcPhrase::ClearedToLand:
        return "atc.cleared_to_land";
    case AtcPhrase::GoAround:
        return "atc.go_around";
    case AtcPhrase::ContactApproach:
        return "atc.contact_approach";
    case AtcPhrase::Unable:
        return "atc.unable";
    }
    return "atc.unable";
}

namespace {
const char* phraseText(AtcPhrase p) noexcept {
    switch (p) {
    case AtcPhrase::HoldShort:
        return "hold short, traffic on the runway";
    case AtcPhrase::ClearedTakeoff:
        return "cleared for takeoff";
    case AtcPhrase::ClearedToLand:
        return "cleared to land";
    case AtcPhrase::GoAround:
        return "go around, runway occupied";
    case AtcPhrase::ContactApproach:
        return "radar contact, continue inbound";
    case AtcPhrase::Unable:
        return "unable";
    }
    return "unable";
}
} // namespace

RadioTransmission makeTransmission(AtcPhrase phrase, const std::string& speaker, fl::EntityId target) {
    RadioTransmission t;
    t.target = target;
    t.phrase = phrase;
    t.speaker = speaker;
    t.text = phraseText(phrase);
    t.voiceKey = atcPhraseVoiceKey(phrase);
    t.displaySeconds = 6;
    return t;
}

} // namespace fl::atc
