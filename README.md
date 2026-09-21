# Static Joystick for The Binding of Isaac: Repentance iOS

A JIT-less static joystick tweak for **The Binding of Isaac: Repentance** on iOS, designed for use with **LiveContainer**.

## Compatibility

This tweak is specifically made for:

- **iOS game version:** 1.4
- **Repentance version:** 1.7.9b.J754
- **Architecture:** ARM64
- **Mach-O UUID:** `F4357753-A25F-30EE-BACF-63709F902895`

**This tweak is only compatible with this exact game build.**

The tweak checks the game's Mach-O UUID before doing anything. If the UUID does not match, **no patch is applied**.

## Features

- Works with LiveContainer
- Does **not** require JIT
- Uses ARM64 hardware breakpoints instead of modifying executable `__TEXT` memory
- Runtime-configurable through an external `.cfg` file

## Installation

1. Place `IsaacStaticJoystick.dylib` in your LiveContainer tweaks folder.
2. Set that folder as the tweak folder for your Isaac Guest app.
3. Place `IsaacStaticJoystick.cfg` in the container's `$HOME` directory, **at the same level as the `Documents` folder**, not inside `Documents`.
4. Launch Isaac through LiveContainer.

## Configuration

The tweak reads:

```text
$HOME/IsaacStaticJoystick.cfg

$HOME refers to the root of the app's container data, next to the Documents folder.

Configuration format:

target_uuid=XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
patch1_offset=0xXXXXXXXX
patch1_original=0xXXXXXXXX
patch1_new=0xXXXXXXXX
patch2_offset=0xXXXXXXXX
patch2_original=0xXXXXXXXX
patch2_new=0xXXXXXXXX

A ready-to-use `.cfg` file is included in the Releases section.

The UUID and patch values must correspond to the supported game build.

## Acknowledgements

Special thanks to **[emp0ry](https://github.com/emp0ry)** for his projects [**IsaacExternalItemDescriptionsiOS**](https://github.com/emp0ry/IsaacExternalItemDescriptionsiOS) and [**IsaacSteamSynciOS**](https://github.com/emp0ry/IsaacSteamSynciOS), which served as inspiration and as a foundation for this project.

Developed with assistance from OpenAI Codex, ChatGPT, and Anthropic Claude.

Made for use with LiveContainer and The Binding of Isaac: Repentance.
