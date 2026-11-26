# MetaPC
MetaPC is a fork of [PCem](https://pcem-emulator.co.uk/), the IBM PC emulator.

Copyright (C) 2026 Pedro A. Pelaez for the MetaPC changes. PCem is
Copyright (C) 2007-2020 Sarah Walker and Contributors and Copyright (C) 2021-2026 Michael Manley,
Sarah Walker and Contributors.

MetaPC is an independent project and is not affiliated with or endorsed by the PCem project.

![Alley Cat running in MetaPC with the green phosphor filter and the CRT effect](docs/screenshots/alley-cat-green-crt.png)

## What's New in MetaPC
MetaPC keeps PCem's emulation core and adds a new interface and several features on top of it.

### Machine Manager
![The MetaPC machine manager](docs/screenshots/machine-manager.png)

The configuration selection dialog is replaced by a machine manager in the style of VirtualBox:
- A list of machines with their state (powered off, starting, running) and a details pane that
  summarises each machine's hardware.
- New, Settings, Start, Stop, Clone, Rename and Delete commands, plus the host network settings.
- The manager stays open while machines run. Each machine runs in its own process, so several
  machines can run at once, and a machine that is already running cannot be started twice.
- **Stop** force-stops a machine that no longer responds, after asking first (its unsaved state is lost).

### Machine Window
![A machine running Alley Cat, with its dockable panels](docs/screenshots/machine-window.png)

- The emulated screen can be drawn inside a monitor frame, surrounded by dockable panels that can
  be moved, floated or hidden from the **Panels** menu:
  - **Machine**: model, CPU, memory, video and sound cards, state, speed and a speed graph.
  - **Controls**: pause/resume, reset, Ctrl+Alt+Del, screenshot, fullscreen, shut down, speed and turbo.
  - One panel per floppy drive, CD-ROM, ZIP drive and the hard disks, with activity LEDs, load/eject,
    recently used images and drag and drop.
- The panel layout and the window position are saved per machine. Fullscreen hides the panels.
- **Screen filters**: a button on the monitor frame switches between colour (no filter), black and
  white, green phosphor and amber phosphor.
- **CRT effect**: a second button adds scanlines, a slightly softer picture and darker edges. It can
  be combined with any screen filter. Both settings are saved per machine.

### Emulation Speed
- **System > Emulation speed** runs the whole machine (CPU, timers, clock, video) at 1x, 2x, 4x, 8x
  or unlimited speed.
- **Turbo** (Ctrl+Alt+Home) runs at unlimited speed until it is pressed again. Sound is muted
  while the machine runs faster than real time.

### Drive Sounds
A new **Drive sounds** device (Sound tab) plays the noises of the drives. They are synthesised, not
recorded samples:
- Floppy drives: motor, disk friction, head steps, head load and disk insertion, with 5.25" and
  3.5" profiles.
- Hard disks: spin-up, spindle hum and seeks, from old stepper motor MFM drives to 7200 RPM drives.
  The profile is chosen from the controller and the disk size.
- CD/DVD drives: spin-up/spin-down, seeks, tray and audio playback, chosen from the drive model and speed.

### Host CD/DVD Drives
- The host's optical drives are listed with their vendor and model, and the list is refreshed when
  a drive is plugged in or removed (also from **CD-ROM > Refresh host drives**).
- The selected drive is found again at start even if its drive letter changed.
- A new **PCemDVD (DVD-ROM)** drive model, and support for DVD images larger than 4 GB.

### CompactFlash Cards as Hard Disks
- A card in a USB reader (or another removable disk of the host) can be used as a hard disk in place
  of an image file: the card button next to **New** in the hard disk settings lists the cards found.
  The emulated machine reads and writes the card directly, so it can be partitioned, formatted and
  set up in the emulator and then moved to a real machine.
- The card gets the geometry it reports to a BIOS set to auto-detect (16 heads and 63 sectors on cards
  over 504 MB). The **Hard disks** panel of the machine window shows it (for example `C 4066 H 16 S 63`)
  with a **Copy** button, to enter the same values in the BIOS of the real machine.
- On Windows, writing to a card needs administrator rights. MetaPC does not need to run as
  administrator: a machine that uses a card asks for permission (the Windows UAC prompt) when it
  starts, and only that machine's process gets it. The disk that holds the operating system and
  fixed internal disks are never offered.

### Other Changes
- **Random MAC address**: a dice button next to the MAC address field generates a new address.
- **Logs in every build**: each process writes a log in the `logs` folder of the data directory
  (`<machine>.log`, or `manager.log` for the manager), so a machine that hangs leaves a trace.
- The memory amount is kept when the machine model changes (an XT's 640 KB no longer becomes 640 MB).
- Fixed a hang when starting a machine with an IBM PC Fixed Disk Adapter (Xebec) or DTC 5150X hard
  disk controller.

Internal names are unchanged so existing setups keep working: the executable is still `pcem`, the
data folder is still `~/.pcem` and the plugin API is the same.

## AI Use
Much of the code added in MetaPC was written with Claude (an AI model made by Anthropic), under
Pedro A. Pelaez's direction and testing. PCem does not accept AI generated or assisted code, so
**do not submit MetaPC changes to the PCem project**.

## License
MetaPC, like PCem, is licensed under GPL v2.0, see [COPYING](COPYING) for more details.

## Patch Submission
Issues and pull requests go to [PSF1/metapc](https://github.com/PSF1/metapc), not to PCem.

## Note about ROM Files
<b>NO COPYRIGHTED ROM FILES ARE INCLUDED NOR WILL THEY BE. PLEASE DO NOT ASK FOR THEM.</b>

## How to Build
You will need the following libraries and buildtools (and their dependencies):
- SDL2
- wxWidgets 3.x (including the aui component)
- OpenAL
- CMake
- ALSA (Linux only)
- GTK3 Dev Package (Linux only)
- PCap
- LibSlirp
- Ninja (Recommended, but you can use a Makefile generator if you prefer)

Open a terminal window, navigate to the MetaPC directory, create a build directory, then enter in that build directory: 
```
cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release ..
ninja
```

then `./src/pcem` to run on most platforms, and open `./src/MetaPC.app` on macOS

BIOS ROM images, configuration files, and other data are stored in `~/.pcem` on non-macOS platforms. You can also create a `.pcem` folder with
the Binary, and run it in a portable mode.

The configure options are specified below. They are in the format of -D`Option`=`Value`. `Value` under here is the
default value.
```
  -DCMAKE_BUILD_TYPE=Release : Generate release build. Recommended for regular use.
  -DCMAKE_BUILD_TYPE=Debug   : Compile with debugging enabled.
  -DPCEM_DISPLAY_ENGINE=wxWidgets : Compile with the WxWidgets Display Engine. Default
  -DPCEM_DISPLAY_ENGINE=SDL  : Compile with the SDL Display Engine. This is more of a sample for additional GUI's but is here for postarity.
  -DUSE_NETWORKING=ON        : Build with networking support.
  -DUSE_PCAP_NETWORKING=ON   : Build with pcap networking support. (Needs USE_NETWORKING to compile) Requires libpcap.
  -DFORCE_X11=ON             : Enables a hack to force X11 on Wayland systems. See #128 for details. (Linux Only)
  -DPLUGIN_ENGINE=ON         : Build with plugin support. Builds libpcem-plugin-api and links MetaPC with it.
  -DPCEM_MARCH=x86_64-v2     : Change the architecture used for generated instructions, by default we set it for
                               >= Nehalem for Intel, and >= Bulldozer for AMD. 
```

If you are using -DCMAKE_BUILD_TYPE=Debug, there are some more debug options you can enable if needed
```
  -DPCEM_SLIRP_DEBUG=OFF           : Build MetaPC with SLIRP_DEBUG debug output
  -DPCEM_RECOMPILER_DEBUG=OFF      : Build MetaPC with RECOMPILER_DEBUG debug output
  -DPCEM_NE2000_DEBUG=OFF          : Build MetaPC with NE2000_DEBUG debug output
  -DPCEM_EMU8K_DEBUG_REGISTERS=OFF : Build MetaPC with EMU8K_DEBUG_REGISTERS debug output
  -DPCEM_SB_DSP_RECORD_DEBUG=OFF   : Build MetaPC with SB_DSP_RECORD_DEBUG debug output
  -DPCEM_MACH64_DEBUG=OFF          : Build MetaPC with MACH64_DEBUG debug output
  -DPCEM_DEBUG_EXTRA=OFF           : Build MetaPC with DEBUG_EXTRA debug output
```

If you are using -DCMAKE_BUILD_TYPE=RelWithDebInfo, there are additional options you can do
```
  -DPCEM_RELDEB_AS_RELEASE=ON     : Builds RelWithDebInfo with debugging logging enabled when this is off
```

They are some extra modules you can add if you build with `-DUSE_EXPERIMENTAL=ON`. These modules are untested.
incomplete, and may or may not be in a future build of MetaPC. We do not provide builds with these enabled as
well. It is also possible they may not even build.
```
  -DUSE_EXPERIMENTAL_PGC=ON       : Build MetaPC with Professional Graphics Controller support.
  -DUSE_EXPERIMENTAL_PRINTER=ON   : Build MetaPC with Printer support. Requires freetype.
``` 

CD-ROM support currently only accesses `/dev/cdrom`. It has not been heavily tested.

On Windows, if you want to use PCAP networking, you will need to install [npcap](https://npcap.com) and replace the libpcap.dll in the MetaPC dir using the wpcap.dll installed in C:\Windows\System32  

## Links

### [MetaPC emulates the following hardware](SYSTEMS.md):
Hardware | Links
--- | ---
Systems | [8088](SYSTEMS.md#8088-based)<br/>[8086](SYSTEMS.md#8086-based)<br/>[286](SYSTEMS.md#286-based)<br/>[386](SYSTEMS.md#386-based)<br/>[486](SYSTEMS.md#486-based)<br/>[Pentium](SYSTEMS.md#pentium-based)<br/>[Super Socket 7](SYSTEMS.md#super-socket-7-based)
Graphics | [Basic](SYSTEMS.md#basic-cards)<br/>[Unaccelerated (S)VGA cards](SYSTEMS.md#unaccelerated-svga-cards)<br/>[2D accelerated SVGA cards](SYSTEMS.md#2d-accelerated-svga-cards)<br/>[3D accelerated SVGA cards](SYSTEMS.md#3d-accelerated-svga-cards)<br/>[3D only cards](SYSTEMS.md#3d-only-cards)
Sound | [Cards](SYSTEMS.md#sound-cards)
HDD Controller | [Cards](SYSTEMS.md#hdd-controller-cards)
Misc | [Cards](SYSTEMS.md#misc-cards)

### [Software Tested](TESTED.md):
- [DOS](TESTED.md#dos)<br/>
- [Windows](TESTED.md#windows)<br/>
- [Windows NT](TESTED.md#windows-nt)<br/>
- [OS/2](TESTED.md#os2)<br/>
- [Linux](TESTED.md#linux)<br/>
- [Applications](TESTED.md#applications)<br/>
- [Games](TESTED.md#games)<br/>
- [Demos](TESTED.md#demos)<br/>
- [Emulators](TESTED.md#emulators)<br/>
