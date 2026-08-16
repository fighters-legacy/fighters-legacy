// SPDX-License-Identifier: GPL-3.0-or-later
#include "atc/CrewPhrases.h"

namespace fl::atc {

const char* lsoPhraseVoiceKey(LsoPhrase p) noexcept {
    switch (p) {
    case LsoPhrase::OnGlideslope:
        return "lso.on_glideslope";
    case LsoPhrase::High:
        return "lso.high";
    case LsoPhrase::Low:
        return "lso.low";
    case LsoPhrase::Fast:
        return "lso.fast";
    case LsoPhrase::Slow:
        return "lso.slow";
    case LsoPhrase::WaveOff:
        return "lso.wave_off";
    case LsoPhrase::GoodTrap:
        return "lso.good_trap";
    }
    return "lso.on_glideslope";
}

const char* lsoPhraseText(LsoPhrase p) noexcept {
    switch (p) {
    case LsoPhrase::OnGlideslope:
        return "Paddles: on glideslope, on speed.";
    case LsoPhrase::High:
        return "Paddles: you're HIGH.";
    case LsoPhrase::Low:
        return "Paddles: you're LOW. Power!";
    case LsoPhrase::Fast:
        return "Paddles: you're fast.";
    case LsoPhrase::Slow:
        return "Paddles: you're slow. Power!";
    case LsoPhrase::WaveOff:
        return "Paddles: WAVE OFF, WAVE OFF!";
    case LsoPhrase::GoodTrap:
        return "Paddles: good trap!";
    }
    return "Paddles: on glideslope, on speed.";
}

const char* crewChiefPhraseVoiceKey(CrewChiefPhrase p) noexcept {
    switch (p) {
    case CrewChiefPhrase::SayAgain:
        return "crew.say_again";
    case CrewChiefPhrase::NoAircraft:
        return "crew.no_aircraft";
    case CrewChiefPhrase::ShutDownFirst:
        return "crew.shut_down_first";
    case CrewChiefPhrase::NoBase:
        return "crew.no_base";
    case CrewChiefPhrase::Refueled:
        return "crew.refueled";
    case CrewChiefPhrase::Rearmed:
        return "crew.rearmed";
    case CrewChiefPhrase::Repaired:
        return "crew.repaired";
    }
    return "crew.say_again";
}

const char* crewChiefPhraseText(CrewChiefPhrase p) noexcept {
    switch (p) {
    case CrewChiefPhrase::SayAgain:
        return "Crew chief: say again?";
    case CrewChiefPhrase::NoAircraft:
        return "Crew chief: you don't have an aircraft.";
    case CrewChiefPhrase::ShutDownFirst:
        return "Crew chief: shut down on the ramp first.";
    case CrewChiefPhrase::NoBase:
        return "Crew chief: nobody out here. Get to a base.";
    case CrewChiefPhrase::Refueled:
        return "Crew chief: fueled and topped off.";
    case CrewChiefPhrase::Rearmed:
        return "Crew chief: rearmed, pins pulled.";
    case CrewChiefPhrase::Repaired:
        return "Crew chief: patched up. She'll fly.";
    }
    return "Crew chief: say again?";
}

namespace {
// The LSO and the crew chief are not the tower, so they carry no clearance phrase. AtcPhrase::Roger
// is the neutral member of that enum — "message received, nothing sequenced" — and saying so beats
// leaving the field at its default HoldShort, which reads as a clearance instruction nobody issued.
RadioTransmission makeCrewTransmission(const char* speaker, const char* text, const char* voiceKey, fl::EntityId target,
                                       uint16_t dwellSeconds) {
    RadioTransmission t;
    t.target = target;
    t.phrase = AtcPhrase::Roger;
    t.speaker = speaker;
    t.text = text;
    t.voiceKey = voiceKey;
    t.displaySeconds = dwellSeconds;
    return t;
}
} // namespace

RadioTransmission makeLsoTransmission(LsoPhrase phrase, fl::EntityId target) {
    // 4 s: an LSO call is a short shout during a 20-second approach, and the next one is close behind.
    return makeCrewTransmission("Paddles", lsoPhraseText(phrase), lsoPhraseVoiceKey(phrase), target, 4);
}

RadioTransmission makeCrewChiefTransmission(CrewChiefPhrase phrase, fl::EntityId target) {
    return makeCrewTransmission("Crew chief", crewChiefPhraseText(phrase), crewChiefPhraseVoiceKey(phrase), target, 5);
}

} // namespace fl::atc
