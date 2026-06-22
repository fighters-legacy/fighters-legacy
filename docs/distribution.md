# Distribution & Monetization

## License and Charging

GPL v3 permits selling the software. Recipients receive the source and may redistribute it
freely — this is expected and not a problem. The model is identical to how id Software
ships Quake engines on Steam: GPL code, community redistribution, commercial distribution
through storefronts. Proprietary game data like community-created
asset packs are unaffected by the engine license.

**Critical constraint**: the Steamworks SDK is proprietary. Linking it into a GPL binary
creates a license conflict. Fighters Legacy must not link Steamworks — Steam is used
purely as a delivery vehicle (installer, auto-update), not as an integrated SDK.

**What GPL does and does not let us do.** GPL lets us *sell* compiled binaries — but
under §6 it also gives anyone who receives a binary (or the source) the right to
redistribute binaries freely, packaged however they like (Flatpak, RPM, DEB, AUR,
AppImage), for free or for money. We cannot revoke that, so binaries are **not** the
scarce good we sell. GPL also does **not** require us to publish binaries at all — only
that source accompany any binary we convey. Withholding first-party free binaries is
therefore fully compliant. Our revenue rests on **convenience, trust, brand, and
proprietary content** — never on binary exclusivity.

---

## Distribution Channels

The five channels below are **additive and non-exclusive**. All can be active
simultaneously. They fall into three tiers: **source** (GitHub), **free packaged**
(Flathub, plus community repackages we don't control), and **first-party paid**
(itch.io, Steam, GOG). Free community channels coexisting with paid first-party builds
is expected and welcome — not a leak to be plugged (see GPL note above).

| Channel | Tier | Cut | When to add | Notes |
|---|---|---|---|---|
| **GitHub Releases** | Source | 0% | Phase 1 alpha | Hosts **source** (GPL: source-accompanies-binary, not public hosting). First-party packaged binaries are intentionally **not** published here — ready-to-run builds come via Flathub + the paid storefronts. Primary entry point for developers and power users. |
| **itch.io** | First-party paid | 0–10% | Phase 2 early access | Zero approval friction. Best channel for early community. Pay-what-you-want with a minimum, framed as supporting development. |
| **Flathub** | Free packaged | 0% | Phase 5 (Linux milestone) | Free, zero-friction Linux desktop packaging via Flatpak. Reaches distro users who don't use itch.io. |
| **Steam** | First-party paid | 30% | After Phase 5 polish | Largest gaming audience. $100 one-time publishing fee. Fixed price. No Steamworks SDK linkage. |
| **GOG** | First-party paid | 30% | Aspirational post-Steam | GOG curates; apply after demonstrable Steam traction. Fixed price, DRM-free audience. |

### Recommended Rollout

1. **Phase 1 alpha** — GitHub Releases only. Source, plus unsigned dev binaries as a
   pre-monetization convenience for the developer audience (retired once Flathub + the
   paid storefronts exist; GitHub reverts to source-only).
2. **Phase 2 early access** — Add itch.io. Pay-what-you-want pricing optional. Community feedback loop.
3. **Phase 5** — Add Flathub. Linux Flatpak complements itch.io AppImage.
4. **After Phase 5** — Submit to Steam once the build is polished enough for a general audience.
5. **Post-Steam** — Apply to GOG if Steam reception warrants it.

---

## The Softened Model — Selling Convenience, Not the Game

Fighters Legacy is **free if you can build it, paid if you want it handed to you ready
to run**. The source builds for free at any time; the paid first-party builds buy a
one-click, signed, auto-updating, supported install. That convenience — not the bits
themselves — is the product.

Because GPL §6 guarantees the right to redistribute binaries, **community repackages
(Flatpak, AUR, DEB, RPM, AppImage) are legitimate, not piracy.** We welcome them. Trying
to gate binaries would be both unenforceable and corrosive to the community we depend on;
a hostile policy invites a "free" community build that out-competes us on goodwill, or a
hard fork.

So first-party paid builds are framed as **"support development,"** not "pay to play":

- **itch.io** — pay-what-you-want with a modest minimum.
- **Steam / GOG** — fixed price (storefront norms), positioned as supporting the project
  and getting platform conveniences (auto-update, cloud saves, achievements).

**Precedent.** [Ardour](https://ardour.org/) runs exactly this model: GPL, source free to
build, official binaries paid, distros package it free — and it funds full-time
development. The cautionary counter-example is **Aseprite**, which abandoned GPL entirely
*because* the license let people redistribute its binaries. We deliberately stay GPL and
soften the framing instead of gating, because the framing is what makes the model durable.

---

## What We Actually Control — Trademark & Content

Since binaries can't be the moat, two levers actually carry the revenue model.

### Trademark & brand

Copyright (the GPL grant) is **not** trademark. The **"Fighters Legacy"** name and logo
can be registered and reserved for **official builds**, even though the code is freely
redistributable — the same mechanism behind Firefox → IceWeasel and Chromium → Chrome.
This lets community repackages exist while keeping the brand attached to the builds we
stand behind.

Honest limits: trademark can't stop nominative/factual use ("a build of Fighters
Legacy"), enforcement has a real goodwill cost, and an aggressive brand policy can itself
provoke a fork. Any brand policy we adopt should be **permissive** — restrict the mark on
*modified* or *misrepresented* builds, not on faithful community rebuilds.

### Proprietary content moat

The durable revenue lever is **content, not engine**. As covered in
[What the Free Base Pack Changes](#what-the-free-base-pack-changes), the free experience
is the GPL engine plus fl-base-pack; premium aircraft, campaigns, and packs like
fa-content are paid and non-GPL. An unofficial free build is "engine + free base pack" —
fully playable, but not the full experience. This is the lever that survives any amount
of free binary redistribution.

---

## What the Free Base Pack Changes

Once fl-base-pack (Phase 2) is available, Fighters Legacy becomes playable with
zero financial barrier. Revenue model shifts to:

- **Content plugins** (e.g. fa-content, future community packs) as optional paid plugins if redistributed as compiled binaries
- **Community packs** priced independently by their creators
- Donations via itch.io / GitHub Sponsors for the engine itself

The engine itself remains GPL and free to compile from source at any time.
