# Audio Lightbar OBS Plugin

Native OBS source plugin that draws a lightweight rainbow spectrum lightbar for
audio currently playing in OBS. Add it to a scene as `Audio Lightbar`, similar
to an old Winamp-style visualizer.

## Features

- Auto mode listens to all current OBS audio sources while the visualizer is
  visible.
- Optional specific audio source selection.
- Log-spaced frequency bands, defaulting to 60 Hz through 16 kHz.
- Lightweight Goertzel spectrum analysis with fixed-size buffers: no per-frame
  heap allocation and no retained audio buffers.
- Configurable bar count, update rate, sensitivity, decay, peak caps, mirror
  mode, frequency range, noise floor, width, and height.
- Analyzes at most 1024 samples per audio update, keeping CPU use bounded even
  with high channel counts.

## Build

Linux dependencies:

- OBS Studio development files (`libobs` headers and library)
- SIMDe headers (`libsimde-dev` on Debian/Ubuntu/Pop!_OS)
- CMake 3.16+
- A C17 compiler

On Debian/Ubuntu/Pop!_OS, install the default dependency set with:

```sh
make deps
```

If your distro separates OBS headers from the OBS app, install that development
package too, for example `libobs-dev`.

Build:

```sh
make build
```

The plugin binary will be `build/audio-lightbar.so`.

Build and install for the current OBS user profile:

```sh
make install
```

## Install

### User install on Linux

Close OBS, then copy the built plugin into OBS' user plugin directory:

```sh
make install-user
```

Start OBS again.

### System install on Linux

If your OBS install loads plugins from `/usr/lib/x86_64-linux-gnu/obs-plugins`,
you can install system-wide with:

```sh
make install-system
```

For distributions using a different OBS plugin directory, copy
`build/audio-lightbar.so` into that directory.

## Usage

1. Open OBS and choose `Sources` -> `+` -> `Audio Lightbar`.
2. Leave `Audio Source` as `Auto: all audio sources` to visualize whatever OBS is
   currently playing, or choose a specific source.
3. Adjust `Bars`, `Spectrum Updates Per Second`, and `Sensitivity` if needed.

If the bars are still too short, raise `Sensitivity` or move `Noise Floor dB`
closer to zero, for example from `-72` to `-60`. Lower `Bars` and
`Spectrum Updates Per Second` if you want the lowest possible CPU use. The
defaults are conservative for normal streaming scenes.
