# Wingman & voice

Ordering your flight, and talking to other players.

## Wingman radio menu (C)

Order your flight. `C` opens the menu, `1`–`6` pick a command (arrows + Enter also work), `Escape`
closes it. It auto-closes after 8 s if you leave it open.

**The menu does not suspend flight control**, unlike the console — the aircraft keeps flying while it
is up, because a radio call is a sub-second action and freezing the jet to make one would be wrong in
a fight. Only the discrete keys the menu consumes are taken; the axes stay live.

| Item | Command | What the wingman does |
|---|---|---|
| 1 | `attack_my_target` | Attacks whatever you are **looking at** — the hostile nearest your boresight. Nothing in the cone means the order is **refused** ("Two, no joy"), not quietly redirected at something else. |
| 2 | `engage_bandits` | Engages hostiles near **itself**, at will; returns to formation when the sky is clear. |
| 3 | `rejoin` | Returns to formation on you. |
| 4 | `cover_me` | Engages hostiles closing on **you**, then returns to your wing. (The difference from `engage_bandits` is *whose* threats it reacts to.) |
| 5 | `hold_fire` | Breaks off and holds station. Sets a weapons-hold flag the server's fire control enforces (#625): the held member's trigger and store releases are read and discarded until an engage order clears the flag. |
| 6 | `return_to_base` | Disengages and orbits home. (There is no landing system yet.) |

The wingman answers with a brevity call on the HUD. In **single-player you always have one** — the
embedded server is started with `--flight-size 1`. A dedicated server gives players a flight only if
the operator sets `[flight] size` (default 0).

If another **player** is in your flight, an order is *relayed* to them as a radio call ("LEAD: Engage
bandits.") rather than applied: the server cannot fly a person's aircraft for them, and compliance is
their choice.

### Spoken orders (F8)

The same six commands can be **spoken** instead of picked from the menu (#935). Hold `F8`
(`WingmanVoiceCommand`), say the order — "two, engage bandits" — and release; the phrase is matched
against the six ordinals and dispatched through the identical path the menu uses, so a spoken order
and a menu order cannot behave differently.

**No language model is involved and none is needed.** The match is a deterministic phrase matcher
(`WingmanPhraseMatch`), which is why this tier works on any machine and on any server, and why it
behaves the same every time. A phrase that does not resolve is declined rather than guessed at.

This needs a speech-to-text backend, which is **not in the shipped builds**: `FL_ENABLE_WHISPER`
is off by default, so without a custom build `F8` does nothing and the radio menu above is the way
to order your flight. If you do build it, no model ships with the game — point `[voice]
stt_model_path` in `user.toml` at a downloaded `ggml-*.bin`. See
[development.md](../developer/development.md#optional-voice-wingman-commands-fl_enable_whisper).


## Voice comms

Voice runs on **radio nets** the server defines, not as one undifferentiated channel. The
bottom-left HUD shows which net your push-to-talk key will use, a `TX` indicator whose brightness
follows your live microphone level — the first thing to check when nobody answers — and who is
currently transmitting.

| Key | Does | Binding |
|---|---|---|
| `V` | Push-to-talk on the selected net. Held, never latched | `PushToTalkPrimary` |
| `Left Ctrl` | Push-to-talk on the **flight** net; wins if both are held | `PushToTalkSecondary` |
| `` `\` `` | Cycle the primary key through the server's nets | `VoiceNetCycle` |

Transmission is suppressed entirely while the console, the chat box or the game-master map has
focus, so a push-to-talk key cannot send a room full of your typing to the whole team.

With no microphone, no permission, or no Opus encoder, the client is simply **listen-only**. None
of those is an error you have to resolve before flying.

The server relays voice opaquely — it never decodes what you say.
