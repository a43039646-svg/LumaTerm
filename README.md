# LumaTerm

LumaTerm is a configurable Linux terminal written in C++ with GTK 3 and
[libvterm](https://www.leonerd.org.uk/code/libvterm/). LumaTerm owns its PTY
process, terminal screen model, keyboard handling, and Cairo/Pango cell renderer.
It supports still images, animated GIFs, and silent looping video backgrounds,
plus glass opacity, background color and blur, terminal colors, font settings,
and scrollback.

## Build on Debian

```sh
sudo apt install build-essential cmake pkg-config libgtk-3-dev libvterm-dev libgstreamer1.0-dev
cmake -S . -B build
cmake --build build
./build/lumaterm
```

To install it in the application menu and as `/usr/local/bin/lumaterm`:

```sh
sudo cmake --install build
```

This also installs **LumaTerm Settings** in the desktop application menu. You
can also start it with `lumaterm-settings`. Choose a background or color, adjust
the blur/opacity and text appearance, then save. Restart the terminal to apply.

To play videos, install GStreamer playback plugins:

```sh
sudo apt install gstreamer1.0-gtk3 gstreamer1.0-plugins-good gstreamer1.0-libav
```

For live video blur, also install `gstreamer1.0-plugins-bad`. The video plugin
is only used when a video background is configured. Video audio is muted, and
the video loops when it reaches the end.

## Configure

Create or edit `~/.config/lumaterm/settings.ini`:

```sh
mkdir -p ~/.config/lumaterm
cp config/settings.ini.example ~/.config/lumaterm/settings.ini
nano ~/.config/lumaterm/settings.ini
```

Example with a GIF background:

```ini
[appearance]
glass=true
background_mode=image
background=/home/your-name/Pictures/wallpaper.gif
background_fit=cover
background_color=#10131b
background_opacity=0.82
background_blur=2
font_family=JetBrains Mono
font_size=13
text_color=#f2f4f8
monochrome_text=false
```

`background` is the absolute file path, including the filename. Modes:

- `image`: PNG/JPEG and other GdkPixbuf formats; GIF files animate.
- `video`: a video supported by installed GStreamer codecs, such as MP4 or WebM.
- `color`: use `background_color` without loading an image.

Settings:

- `glass=true|false`: use a compositor-backed transparent window when available.
- `background_opacity=0.0..1.0`: background opacity; lower values show through more.
- `background_fit=cover|contain|stretch`: crop to fill, fit inside, or stretch the image.
- `background_blur=0..10`: blur strength. Video blur needs the optional Bad Plugins package.
- `background_color=#RRGGBB` and `text_color=#RRGGBB`: terminal palette colors.
- `monochrome_text=true`: turns the ANSI color palette white too, for monochrome Fastfetch output. Programs that send explicit 24-bit RGB colors can still choose their own colors.
- `font_family` and `font_size`: terminal font and point size.

For a video, change `background_mode=image` to `background_mode=video` and put
its path in `background`. For a solid color, set `background_mode=color` and
choose `background_color`. Restart LumaTerm after changing the config.

The example configuration is also available at `config/settings.ini.example`.

Keyboard font zoom: **Ctrl+Shift+=** to enlarge, **Ctrl+Shift+-** to shrink,
and **Ctrl+Shift+0** to reset. Scrollback: use the mouse wheel or **Shift+PageUp**
and **Shift+PageDown**.

## Terminal engine

VTE has been removed. The terminal now uses libvterm for VT parsing and screen
state, plus LumaTerm's GTK/Cairo/Pango renderer and a PTY managed by LumaTerm.
This is the foundation for implementing inline graphics protocols in LumaTerm;
Kitty Graphics Protocol support is not included in this initial engine migration.
