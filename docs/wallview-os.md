# WallView OS

How the system is put together on the development panel (stock bootloader, Android still present but not booted). This describes phase 2 work in progress.

## eMMC layout

| Partition | Size | WallView use |
|---|---|---|
| `recovery` | 64 MB | loader (boot image: kernel + initramfs + vendor DTB) |
| `cache` | 384 MB | `primary` slot: 0–64 MiB boot image, 64–320 MiB root file system (EROFS, read-only), 320–384 MiB data (ext4) |
| `backup` | 112 MB | `golden` slot: boot image only, used as rescue system |
| `misc` | 4 MB | BCB (`boot-recovery`), boot counter, logs |

All other partitions (Android `boot`, `system`, `vendor`, `userdata`, ITEAD `oem`, `stback`, …) are untouched. Once Android is removed for good, the eMMC can be repartitioned properly.

## Boot flow

1. The vendor U-Boot reads BCB `boot-recovery` and boots `recovery` with the vendor DTB.
2. Loader stage (`initramfs/init`, vendor DTB detected): increments the boot counter, picks `primary` or `golden`, extracts kernel, ramdisk and our DTB from the slot and `kexec`s into it with `wallview.slot=<slot>`.
3. WallView stage of the initramfs (our DTB): starts the watchdog daemon. In the `primary` slot it looks for an EROFS super block at 64 MiB of `cache`; if found it mounts it read-only, moves `/dev`, `/proc`, `/sys` and `switch_root`s into the Buildroot system. Otherwise (and always in `golden`, or with `wallview.rescue=1`) it stays in the **rescue system**: Wi-Fi, SSH, logs.
4. Buildroot system (busybox init, eudev): `S05wallview-data` mounts `/data`, `S20wallview-devices` sets audio and the light sensor, `S30wallview-time` starts NTP (BusyBox `ntpd`, enabled with `board/wallview/busybox.fragment` because Buildroot's BusyBox configuration leaves it out), `S40wallview-network` starts Wi-Fi (`/data/wallview/wpa_supplicant.conf`, else a built-in `/etc/wpa_supplicant.conf`, which exists only in local builds with `local/wifi` or with `DEV_FILES=1`; Buildroot's example file that joins any open network is removed; each start is logged to `/data/wallview/network.log`), `S40bluetoothd` BlueZ (`/etc/bluetooth/main.conf`: experimental interfaces for the advertisement monitor, 30 ms scan window every 320 ms), `S50dropbear` SSH, `S60wallview-agent` the Home Assistant agent, `S95wallview-health` resets the boot counter one minute after the network is up, `S99wallview-kiosk` runs Cog.

`wpa_supplicant` is built with the Unix socket control interface (`BR2_PACKAGE_WPA_SUPPLICANT_CLI`, which also installs `wpa_cli`). Without it, it rejects every configuration with a `ctrl_interface` line (the files from `local/`, `DEV_FILES=1` and Improv all have one) and Wi-Fi never connects; that stopped the first boot on the panel.

## Kiosk

Cog runs on DRM/KMS without a compositor and is restarted if it exits. It first opens `/usr/share/wallview/launcher.html` (start and offline screen in the design kit style: logo, clock and date once the time is synchronized, connection status). The page tries the dashboard URL every 5 seconds and switches to it as soon as it answers. Font: Inter (OFL, `usr/share/wallview/fonts/`). Time zone Europe/Prague, locales `en_US` and `cs_CZ`. HTTPS needs `BR2_PACKAGE_LIBSOUP3_SSL` (glib-networking with GnuTLS as the GIO TLS backend, CA certificates from `ca-certificates`); without it every HTTPS dashboard stays at "Connecting to Home Assistant…". WebKit takes the type of `file://` pages from the shared MIME database (`shared-mime-info`, only `mime.cache` on the target); without it the launcher is shown as plain text.

Storage and scale: `/data` has only 64 MiB. WebKit's HTTP cache and Mesa's shader cache filled it within an hour of using a Home Assistant dashboard (51 MB); the WebKit network process then failed at start ("WebKit encountered an internal error", no `WPENetworkProcess`) and the kiosk stayed on the start page. The HTTP cache is therefore in RAM (`XDG_CACHE_HOME=/tmp/wallview-cache`), the shader cache stays on `/data` (`/data/xdg/mesa-shader-cache`, at most 16 MB) so shaders survive reboots, and the old `/data/xdg/cache` is removed at kiosk start. Cookies and local storage (the Home Assistant tokens) stay in `/data/xdg/data`. Cog runs with `--device-scale=1.5`: the 4" panel has about 170 DPI, so the page is laid out for 320×320 CSS pixels and rendered at the full 480×480; `/data/wallview/cog-args` can override it.

Performance (Home Assistant frontend on the PX30, measured 2026-11-26/27 with the WebKit remote inspector):

- CPU: 1.416 GHz is the highest stable operating point (`cpu0_opp_table` in the DTS adds 1416 MHz at 1.35 V like the vendor table; 1512 MHz produced CoreMark CRC errors in 2 of 24 runs on the development unit, 1416 and 1296 MHz none in 32). All four cores share one clock and supply, so per-core settings are not possible. The kernel starts with `cpufreq.default_governor=performance` (mains powered, avoids frequency ramp-up delays). DDR runs at 664 MHz, the vendor maximum.
- CPU stability retest, night of 2026-11-28 (unfinished): on a kexec'd kernel with a 1512 MHz `turbo-mode` OPP (boost off by default), four CoreMark processes for 1 h at 1416 MHz passed 692 of 692 runs (CRC validated), with the SoC up to 81 °C (passive trips 70 and 85 °C, `step_wise`); log in `tools/perf/captures/cpu-1416-coremark-2026-11-28.txt`. During the following 1416 MHz memory-stress phase (started 01:29Z) or later the board stopped answering; the 1512 MHz phases may not have run. What the boot records show: the board reset (watchdog or panic, `panic_on_oops=1` was set), the primary slot then booted twice without resetting the boot counter (`network.log` has two `wlan0 missing` boots, but they may also be from T3 Code's DRAM test the evening before, which failed the same way), and the loader fell back to the golden slot (tries 3), which had Wi-Fi at 60 s but was unreachable by 06:00Z; a power cycle at 07:34Z brought the primary back. A photo of the screen just before the power cycle shows the end of a kernel call trace at uptime 20236 s on the fbcon console (so the golden boot started about 01:56Z, which dates the original reset shortly before): `drm_atomic_helper_commit` ← `drm_atomic_commit` ← `drm_atomic_helper_dirtyfb` ← `drm_fbdev_dma_helper_fb_dirty` ← `drm_fb_helper_damage_work` (workqueue). The top of the trace (warning or oops, and where) was not visible; it is a separate fault of the golden slot's older kernel (#20) in the fbdev deferred update, likely why golden stopped answering. The cause of the first failure is not known: pstore is compiled in (`CONFIG_PSTORE_RAM=y`) but the DTS has no ramoops region, the lockup and hung-task detectors are off, and the log poller had stopped. Before any further stress test: a ramoops region in the DTS, netconsole or a serial log, and the soft-lockup detector. Until then 1416 MHz stays the default; 1512 MHz is untested.
- CPU frequency limits, second run 2026-11-28 10:05Z (with the PR #26 crash records; test log written to `pmsg` too, recovered from pstore after the reset: `tools/perf/captures/cpu-frequency-2026-11-28-run2.txt`): 1416 MHz again clean, 688 of 688 CoreMark runs in 1 h and 40 of 40 memory-stress rounds in 20 min, up to 80 °C. At 1512 MHz (1.35 V, the vendor voltage and the regulator maximum) the board reset after 19 clean rounds, about 6 minutes in (11:32Z), and came back by itself on the installed kernel. The console record ends with the last line before the test; the kernel printed nothing (no oops, no lockup, no panic), so the reset came from below the kernel: all cores stopped at once and the hardware watchdog fired, or the supply dropped out. The overnight failure fits the same pattern: by the timing it happened a few minutes into the 1512 MHz phase, not at 1416 MHz. Sustained CoreMark per process was also not higher at 1512 MHz (3256 against 3418 iterations/s at 1416 MHz in this run). Verdict: 1416 MHz is the highest usable frequency on this unit (two runs, 1380 CoreMark runs and 60 memory rounds without an error); 1512 MHz is not usable, neither sustained nor as a boost.
- Home Assistant sends every entity change as its own websocket message (about 7/s with 1888 entities on the test installation) and the frontend re-renders its cards for each one. A web-process extension that batched and merged these messages (and made `ha-spinner` rotate only) raised the overview from 6 to 52 idle fps, but it changes how the page behaves, so it was removed: the kiosk shows Home Assistant exactly as served, and performance has to come from the engine, the OS and the hardware.
- Remaining limits found by profiling: WPE's legacy input path (libwpe, used by Cog) sends each touch to the page's main thread before a click or scroll starts and has no kinetic scrolling; WebKit compares the 644 custom properties Home Assistant defines on `:root` repeatedly when they change.

Kiosk browser: `wallview-browser` (`board/wallview/browser/wallview-browser.c`, built in `post-build.sh` when WebKit provides the WPE platform API) is a minimal launcher on WebKit's WPEPlatform DRM backend. Unlike Cog's libwpe path, WPEPlatform recognises gestures in the UI process: drags become wheel events handled by the scrolling thread and taps become clicks without waiting for the page's main thread. It uses the same data directories as Cog (`$XDG_DATA_HOME/wpe`, so the Home Assistant sign-in is kept), runs fullscreen with `WPE_DRM_SCALE=1.5` and reloads the page if the web process dies. Cog stays installed as a fallback: `echo cog > /data/wallview/browser` selects it. WebKit is built with `ENABLE_WPE_PLATFORM=ON` and the DRM backend (`external.mk`) and with `patches/wpewebkit/0001-…` (pointer fast path for custom property comparisons).

Profiling the kiosk: CI uploads the unstripped `libWPEWebKit-2.0.so` from the Buildroot staging directory as the `webkit-symbols` artifact of each run, so `perf` samples taken on the panel (`perf record -e cpu-clock -g -t <WebProcess main thread>`) can be symbolised (`perf report --symfs <dir with usr/lib/libWPEWebKit-2.0.so.1.x>`). The symbols must come from the same run as the installed rootfs. The WebKit remote inspector is enabled with `WEBKIT_INSPECTOR_HTTP_SERVER=0.0.0.0:9222` (Cog `--enable-developer-extras=true`); the probe scripts used for the measurements are listed in the performance notes below.

Known harmless message: when Cog exits (kiosk restart), libepoxy aborts with "No provider of eglReleaseThread found" because the EGL display is already terminated; the kiosk loop starts Cog again.

The watchdog daemon started by the initramfs keeps running after `switch_root`.

Firmware (Wi-Fi, Bluetooth) is not part of the root file system, so a rootfs built in CI contains no blobs. Before `switch_root` the initramfs mounts a tmpfs on `/lib/firmware` of the new root and copies its own firmware there; drivers that load firmware later (Bluetooth on first power-up, Wi-Fi after an SDIO reset) find it.

## Root file system

Built with Buildroot 2026.02 (LTS) from the `BR2_EXTERNAL` tree in `buildroot/`:

```
make -C ~/src/buildroot O=$HOME/build/br BR2_EXTERNAL=/Users/jakub/WallView/buildroot wallview_defconfig
make -C ~/src/buildroot O=$HOME/build/br
```

- Toolchain: Arm GNU Toolchain 14.2 (Bootlin toolchains only exist for x86-64 hosts; the build VM is arm64).
- Graphics: Mesa with Panfrost (EGL, GLES, GBM; Panfrost needs LLVM at build time), WPE WebKit 2.50, Cog 0.18 with the DRM platform (no compositor).
- Wi-Fi credentials and the SSH key come from `local/` through `board/wallview/post-build.sh`, the PSK only as a hash. Without `local/` (CI) there is no built-in Wi-Fi, Wi-Fi is set up with Improv, and `/root/.ssh` is a link to `/data/wallview/ssh`.
- `wallview-agent` is compiled natively by `post-build.sh` (`cargo build --locked` with a clean environment, so Buildroot's cross `pkg-config` is not used) on an arm64 Ubuntu 24.04 host; it needs glibc 2.39, the target has 2.40, and links `libdbus-1` from the target. The host needs Rust (`~/.cargo/bin/cargo`), `libdbus-1-dev` and `pkg-config`.
- Output `images/rootfs.erofs` (LZ4HC).

Known cost: with `BR2_PACKAGE_MESA3D_LLVM` Mesa also builds gallivm, so `libgallium` links `libLLVM.so` (82 MB) at run time although Panfrost itself only needs LLVM for its build-time shader compiler. To be optimised later (target Mesa without LLVM, host-only precompiler). `post-build.sh` removes what the target does not need from the LLVM, Clang, libclc and SPIRV packages (`/usr/share/clc`, `libclang*`, `libLLVMSPIRVLib`, `libLTO`, `libRemarks`, the `*-arch`, `diagtool`, `llvm-spirv` and `spirv-*` tools); `libLLVM` and the SPIRV-Tools libraries stay because `libgallium` links them. Without that the EROFS image was 258 MiB, more than the 256 MiB slot; with it 171 MiB.

The large unified WebCore sources need 2–3 GB per compiler process. With an 8 GB VM and four jobs the OOM killer stopped the build; with two jobs it crawled because of swapping. What works on a 16 GB Mac: VM memory 12 GB (`limactl edit wallview --memory 12`), 16 GB of swap files in the VM (`fallocate`, `mkswap`, `swapon`, again after every VM restart) and four jobs (`make … BR2_JLEVEL=4`; the defconfig keeps the safe value 2). The whole first build takes several hours (LLVM, Mesa, WebKit).

## Data partition

`/data` (ext4 on a loop device at 320 MiB of `cache`), created on first boot:

| Path | Content |
|---|---|
| `/data/wallview/url` | dashboard URL (default `http://homeassistant.local:8123`) |
| `/data/wallview/cog-args` | extra Cog arguments, e.g. `--ignore-tls-errors` when the URL is an IP address and the certificate is for a domain |
| `/data/wallview/language` | locale for WebKit and the start screen, e.g. `cs_CZ.UTF-8` (default `en_US.UTF-8`) |
| `/data/wallview/wpa_supplicant.conf` | Wi-Fi network written by Improv (SSID in hex, PSK as hash) |
| `/data/wallview/bluetooth-proxy` | `on` when the ESPHome Bluetooth proxy is on, see [Home Assistant integration](home-assistant.md#bluetooth-proxy) |
| `/data/wallview/presence` | `sensitivity=<1–100>` of the Presence binary sensor, see [Home Assistant integration](home-assistant.md#presence) |
| `/data/wallview/api.key` | ESPHome API encryption key provisioned by Home Assistant |
| `/data/wallview/network.log` | Wi-Fi start log (configuration used, missing `wlan0` with the interfaces present) |
| `/data/wallview/ssh/authorized_keys` | SSH keys when the image has none built in (development only) |
| `/data/wallview/mqtt.conf` | MQTT broker for `wallview-agent`, see [Home Assistant integration](home-assistant.md) |
| `/data/xdg/` | WebKit data (local storage with the Home Assistant login, cookies, cache) |

A slot update never writes beyond 320 MiB, so `/data` survives updates.

From the Mac, `tools/panel-config.sh <panel-ip> show|url|language|cog-args|mqtt …` writes these files and restarts the kiosk or the agent. The MQTT password is read from a local file so it does not appear on the command line.

## Building and installing a full slot

```
limactl shell wallview -- bash ~/WallView/tools/build-recovery.sh
limactl shell wallview -- bash ~/WallView/tools/build-slot.sh
IMAGE=~/WallView/out/wallview-primary.img ~/WallView/tools/install-panel.sh <panel-ip> primary
```

then `reboot -f` on the panel (from a running Buildroot system the script goes through `golden` and reboots by itself, see the bring-up guide), check, and `wallview-boot promote` (copies only the boot image to `golden`, which therefore stays a rescue system).

With the root file system from CI (GitHub Actions, `.github/workflows/rootfs.yml`, artifact `wallview-rootfs`) instead of a local Buildroot build:

```
gh run download -R bzumik1/wallview <run-id> -n wallview-rootfs -D ~/WallView/out/ci
limactl shell wallview -- env ROOTFS=/Users/jakub/WallView/out/ci/rootfs.erofs DEV_FILES=1 bash ~/WallView/tools/build-slot.sh
```

CI caches, so only the first build per configuration takes hours:

| Cache | Key | Content |
|---|---|---|
| downloads + ccache | full builds only (output cache miss), restored from the newest | `br-dl`, `.buildroot-ccache` |
| Buildroot output | Buildroot version + hash of `configs/`, `Config.in`, `external.mk`, `external.desc` | `host/`, `target/`, `staging`, `.config`, and from `build/` only the stamp and file-list files (sources and objects are deleted before saving) |
| Rust | `Cargo.lock` + agent sources | cargo registry and `agent-target` |

With a hit on the output cache, all stamps are touched with one common timestamp (the fresh Buildroot clone has newer files; with different times a `.stamp_installed` older than its `.stamp_built` would start an install from the pruned build directory) and `make` only runs the finalize step: overlay, `post-build.sh` (agent), locales and the EROFS image. A change in the defconfig or the external tree is a miss and a full build (helped by ccache). Removing a file from the overlay does not remove it from a cached `target/`; for that, or if a cached build misbehaves, raise `v1` in the cache key in `.github/workflows/rootfs.yml`.

The CI image contains neither Wi-Fi credentials nor SSH keys. `DEV_FILES=1` unpacks it, adds `/etc/wpa_supplicant.conf` and `/root/.ssh/authorized_keys` from `local/` and packs it again (LZ4HC, same as Buildroot), so a development panel stays reachable. Without it the panel starts Improv and has no SSH access.
