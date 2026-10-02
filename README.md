# webos-vncserver
An somewhat hacky VNC server for WebOS.

Requires root privileges.

# Usage

Install provided package, launch VNC Server app, adjust configuration (mind
default enabled password), press "Save settings", and switch "Running" on.

You should then be able to connect via a VNC client of your choice, on port 5900.

Here it is, running on a "headless" TV motherboard, being accessed via a VNC mobile app.

![Demo](./img/demo.jpg?raw=true)

Alternatively, `webos-vncserver` can be launched via CLI for debugging/testing:
```
# ./webos-vncserver
```

This will immediately launch VNC server, regardless of autostart options.

For extra logging `VNCSERVER_DEBUG=1` environment variable can be set.

In order to make the service register on Luna bus when running via CLI
`LS_SERVICE_NAMES=org.webosbrew.vncserver.service` environment variable needs to
be set. This is usually not required, unless debugging some deeper
frontend-service integration, since the service shall be started automatically
by `ls-hubd` when called by frontend.

## Video capture
Hardware video planes (HDMI inputs, Live TV, apps using hardware video
decoding) are captured along with UI layers, using:
- `libvtcapture` (webOS 5.x+) - up to 1920 pixels wide, "blended" output is
  used, where the TV has already composited UI layers over video, so separate
  UI capture is skipped while video is displayed. Wider resolutions (eg.
  3840x2160) are captured from display output (video only - blended output
  can't be wider than 1920 pixels), with UI layers blended in software. If
  unsupported, the other output is used.
- `libdile_vt` (webOS 3.x - 4.x) - video is blended with UI layers in software.

Video is captured at the configured resolution ("Capture width/height") -
1920x1080 updates several times faster, 3840x2160 shows full 4K detail.

Video capture only runs while VNC clients are connected and video is
displayed - otherwise only UI layers are captured, as before. HDR video
(HDR10, Dolby Vision, HLG) is converted to SDR, based on video output status
reported by `com.webos.service.videooutput`.

Video capture can be disabled with the "Video capture" switch (`videoCapture`
setting).

Notes:
- `libvtcapture` only works within the elevated service (it registers its own
  Luna service names, which are only allowed by Luna role of the service
  executable).
- UI layers blended over video in software are captured at most at
  1920x1080 (at 4K it's ~10x slower), twice a second or soon after client
  input, and skipped while fully transparent.
- HDR video from display output is converted to SDR by the server; blended
  output has already been tone mapped by the TV.
- `libvtcapture` keeps rotating its capture buffers regardless of readers, so
  every frame is copied out of the capture buffer (and verified) before
  conversion - converting it in place shows as tearing. Above 1920x1080,
  capture frame rate is limited to 15 fps and buffers are copied right after
  hardware has moved on from them.
- `VNCSERVER_VTCAPTURE_SIZE` (eg. `1920x1080`) overrides video capture
  resolution, for testing.
- DRM-protected content (eg. streaming apps running on the TV itself) can't be
  captured and shows up black.
- For debugging, `VNCSERVER_VTCAPTURE_DUMP` environment variable forces
  `libvtcapture` dump location (`2` - display output, `3` - blended output),
  and `VNCSERVER_VIDEO_COLOR` forces video color decoding (`bt709`, `bt2020`,
  `pq`, `hlg`).

## Performance
- Frames are only captured once clients have received the previous one, and
  only changed parts of the screen are sent. Next frame is captured while
  previous one is being sent (double buffering).
- Encoding is usually the bottleneck - use a client supporting Tight encoding
  with JPEG (eg. TigerVNC) for video. JPEG rectangles are compressed by
  multiple threads (see `prebuilt/patches`), using NEON. Video frames are
  JPEG-compressed with 4:2:0 chroma subsampling even if client asks for
  4:4:4 (default for higher quality levels), since captured video has 4:2:0
  chroma anyway.
- Large frames are converted (YCbCr -> RGB, HDR -> SDR) by multiple threads,
  using NEON.
- macOS Screen Sharing only supports zlib/ZRLE encodings - zlib encoding is
  lossless, done in parallel by multiple threads, using bundled libdeflate
  (patched with a faster mode for 32-bit pixels, see `prebuilt/patches`) on
  CPUs with NEON, otherwise bundled zlib-ng (built with runtime NEON
  detection, which old webOS glibc doesn't support out of the box). At 4K,
  Wi-Fi throughput becomes the limit (each frame is ~6-12MB).
- Rough numbers for video from an HDMI source on a 2024 TV (4x Cortex-A76 at
  1.4GHz, Wi-Fi), with TigerVNC (Tight, quality 8): ~20 updates/s at
  1920x1080, ~4 updates/s at 3840x2160 (Dolby Vision source). With zlib
  (macOS Screen Sharing): ~2-2.8 updates/s at 3840x2160.
- `VNCSERVER_DEBUG=1` logs encoding used by clients, update rate and timings.

## Caveats
- Capture may conflict with other applications or webOS services.
  If video display hangs or crashes, try stopping relevant services (eg. via
  `pkill -f captureservice`) before starting the service up/connecting using VNC.
  Notable conflicting services:
    - `piccap`: `hyperion-webos`
    - webOS 5.x+: `captureservice`

- `tigervnc`: "React too big" error can be alleviated by adding `Autoselect=0`
  option to command line
- webOS limited network throughput (100mbps over ethernet) requires use of JPEG,
  make sure your VNC clients supports it.
- All configuration changes cause client disconnects.

## Keybindings

- SUPER ("Windows") button is HOME
- Right mouse button is BACK
- Mouse scroll should properly be interpreted as scrollwheel

## Luna service control
**(advanced users only)**

VNC server is implemented as a proper Luna service. In order to make it run as
root `elevate-service` script of Homebrew Channel needs to be used after install
or upgrade - this is done automatically on first "VNC Server" app launch.

Service can be controlled using Luna service bus calls:

* `luna://org.webosbrew.vncserver.service/start` - start up the service
* `luna://org.webosbrew.vncserver.service/stop` - stop the service
* `luna://org.webosbrew.vncserver.service/status` - return current status and
  configuration
* `luna://org.webosbrew.vncserver.service/quit` - shut down the service
  altogether
* `luna://org.webosbrew.vncserver.service/configure` - change configuration,
  supports:
    * `width` - capture width
    * `height` - capture height
    * `framerate` - framerate limit
    * `autostart` - start automatically on boot (assumes correct Homebrew
      Channel autostart configuration)
    * `password` - basic authentication - can be set to empty string for no
      authentication
    * `videoCapture` - capture hardware video planes (default: `true`)

As usual - all these commands can be issued using `luna-send` command like so:
```sh
luna-send -n 1 'luna://org.webosbrew.hbchannel.service/configure' '{"password": "test"}'
luna-send -n 1 'luna://org.webosbrew.hbchannel.service/start' '{}'
```

# Building
## Service
To cross-compile for WebOS, you will [need an
toolchain](https://github.com/openlgtv/buildroot-nc4/releases/tag/webos-c592d84).

```sh
cmake -S . -B build && cmake --build build --target webos-vncserver --target capture_gm --target capture_halgal --target capture_vtcapture --target capture_dile_vt
```

This should have produced a `build/service/` directory. Copy it over to your TV and run `./webos-vncserver` as root!

## Frontend
Configuration frontend is based on web technologies - in order to build it you
will need NodeJS (18.x) and use:
```sh
npm install
npm run build
```

A convenience shortcut for native build here is provided as:

```sh
npm run build-native
```

Then, in order to package and deploy an app, use:

```sh
npm run package
npm run deploy
npm run launch
```
