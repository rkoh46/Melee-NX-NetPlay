// Step-by-step instructions for connecting the controllers people use with Melee on Apple devices, shown by the
// "Connect a controller" flow on macOS and iOS. Apple does not let apps pair Bluetooth controllers themselves, so the
// flow puts the pad in pairing mode, opens Bluetooth settings, and confirms the moment the controller shows up.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace host {
struct PairingGuide {
  const char* name;     // shown in the picker
  const char* symbol;   // SF Symbol
  const char* steps;    // how to put this controller in pairing mode
};

inline constexpr PairingGuide kPairingGuides[] = {
  {"PlayStation", "playstation.logo",
   "DualSense or DualShock 4: hold Create (Share on DualShock 4) and the PS button together until the light bar flashes quickly."},
  {"Xbox", "xbox.logo",
   "Xbox Wireless Controller: turn it on with the Xbox button, then hold the small pair button on the top edge until the Xbox logo "
   "flashes quickly. Controllers from 2016 and later have Bluetooth."},
  {"Switch Pro", "gamecontroller",
   "Switch Pro Controller: hold the small sync button on the top edge, next to the USB-C port, until the player lights run back and forth."},
  {"Other", "gamecontroller",
   "8BitDo and other controllers: switch it to its Apple, X-input or Switch mode, then hold its pair button until the light flashes "
   "quickly. The manual names the buttons."},
};
inline constexpr int kPairingGuideCount = sizeof(kPairingGuides) / sizeof(kPairingGuides[0]);
}  // namespace host
