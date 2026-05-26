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

---

## Distribution Channels

The five channels below are **additive and non-exclusive**. All can be active
simultaneously.

| Channel | Cut | When to add | Notes |
|---|---|---|---|
| **GitHub Releases** | 0% | Phase 1 alpha | Required — GPL demands public source. Primary source for developers and power users. |
| **itch.io** | 0–10% | Phase 2 early access | Zero approval friction. Best channel for early community. Pay-what-you-want or fixed price. |
| **Flathub** | 0% | Phase 5 (Linux milestone) | Linux desktop packaging via Flatpak. Reaches distro users who don't use itch.io. |
| **Steam** | 30% | After Phase 5 polish | Largest gaming audience. $100 one-time publishing fee. No Steamworks SDK linkage. |
| **GOG** | 30% | Aspirational post-Steam | GOG curates; apply after demonstrable Steam traction. Best for DRM-free audience. |

### Recommended Rollout

1. **Phase 1 alpha** — GitHub Releases only. Source + unsigned binaries. Developer audience.
2. **Phase 2 early access** — Add itch.io. Pay-what-you-want pricing optional. Community feedback loop.
3. **Phase 5** — Add Flathub. Linux Flatpak complements itch.io AppImage.
4. **After Phase 5** — Submit to Steam once the build is polished enough for a general audience.
5. **Post-Steam** — Apply to GOG if Steam reception warrants it.

---

## What the Free Base Pack Changes

Once fl-base-pack (Phase 2) is available, Fighters Legacy becomes playable with
zero financial barrier. Revenue model shifts to:

- **Content plugins** (e.g. fa-content, future community packs) as optional paid plugins if redistributed as compiled binaries
- **Community packs** priced independently by their creators
- Donations via itch.io / GitHub Sponsors for the engine itself

The engine itself remains GPL and free to compile from source at any time.
