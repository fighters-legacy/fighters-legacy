// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class IContentPack;

// Callback interface for content-pack security events. Implement in the game
// layer to present consent prompts or log security notices.
class IContentPackEventHandler {
  public:
    virtual ~IContentPackEventHandler() = default;

    // Fired for every pack whose trust level is Unsigned. The game layer
    // decides whether to show a "run at own risk" consent prompt.
    virtual void onUntrustedPackLoaded(const IContentPack& pack) = 0;

    // Fired for every pack that ships a native compiled plugin (.dll/.so/.dylib),
    // regardless of trust tier. Always shown — even Maintainer-signed plugins
    // require the user to acknowledge native code execution.
    virtual void onNativeCodePackLoaded(const IContentPack& pack) = 0;
};
