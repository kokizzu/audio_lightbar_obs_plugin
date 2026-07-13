# Audio Lightbar OBS Plugin

Native OBS source plugin that draws a lightweight rainbow peak lightbar for audio
currently playing in OBS. Add it to a scene as `Audio Lightbar`, similar to an
old Winamp-style visualizer.

## Features

- Auto mode listens to all current OBS audio sources while the visualizer is
  visible.
- Optional specific audio source selection.
- Fixed-size peak buffers: no per-frame heap allocation and no retained audio
  buffers.
- Configurable bar count, update rate, sensitivity, decay, peak caps, mirror
  mode, width, and height.
- Scans at most 2048 samples per incoming audio block, keeping CPU use bounded
  even with high channel counts.

## Build

Linux dependencies:

- OBS Studio development files (`libobs` headers and library)
- SIMDe headers (`libsimde-dev` on Debian/Ubuntu/Pop!_OS)
- CMake 3.16+
- A C17 compiler

Build:

```sh
cmake -S . -B build
cmake --build build
```

The plugin binary will be `build/audio-lightbar.so`.

## Install

### User install on Linux

Close OBS, then copy the built plugin into OBS' user plugin directory:

```sh
mkdir -p ~/.config/obs-studio/plugins/audio-lightbar/bin/64bit
cp build/audio-lightbar.so ~/.config/obs-studio/plugins/audio-lightbar/bin/64bit/
```

Start OBS again.

### System install on Linux

If your OBS install loads plugins from `/usr/lib/x86_64-linux-gnu/obs-plugins`,
you can install system-wide with:

```sh
sudo cmake --install build --prefix /usr
```

For distributions using a different OBS plugin directory, copy
`build/audio-lightbar.so` into that directory.

## Usage

1. Open OBS and choose `Sources` -> `+` -> `Audio Lightbar`.
2. Leave `Audio Source` as `Auto: all audio sources` to visualize whatever OBS is
   currently playing, or choose a specific source.
3. Adjust `Bars`, `Peak Updates Per Second`, and `Sensitivity` if needed.

Lower `Bars` and `Peak Updates Per Second` if you want the lowest possible CPU
use. The defaults are conservative for normal streaming scenes.
