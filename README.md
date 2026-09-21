# Static Joystick for The Binding of Isaac: Repentance iOS

A JIT-less static joystick tweak for **The Binding of Isaac: Repentance** on iOS, designed for use with **LiveContainer**.

## Features

- Works with LiveContainer
- Does **not** require JIT
- Uses ARM64 hardware breakpoints instead of modifying `__TEXT`
- Runtime-configurable through an external `.cfg` file

## Installation

1. Place `IsaacStaticJoystick.dylib` in your LiveContainer tweaks folder.
2. Set that folder as the tweak folder for your Isaac Guest app.
3. Place `IsaacStaticJoystick.cfg` in the container's `$HOME` directory.
4. Launch Isaac through LiveContainer.

## Configuration

The tweak reads:

```text
$HOME/IsaacStaticJoystick.cfg
