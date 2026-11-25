# metaPC
metaPC is a fork of [PCem](https://pcem-emulator.co.uk/), the IBM PC emulator.

Copyright (C) 2026 Pedro A. Pelaez for the metaPC changes. PCem is
Copyright (C) 2007-2020 Sarah Walker and Contributors and Copyright (C) 2021-2026 Michael Manley,
Sarah Walker and Contributors.

metaPC is an independent project and is not affiliated with or endorsed by the PCem project.

## AI Use
Much of the code added in metaPC was written with Claude (an AI model made by Anthropic), under
Pedro A. Pelaez's direction and testing. PCem does not accept AI generated or assisted code, so
**do not submit metaPC changes to the PCem project**.

## License
metaPC, like PCem, is licensed under GPL v2.0, see [COPYING](COPYING) for more details.

## Patch Submission
Issues and pull requests go to [PSF1/metapc](https://github.com/PSF1/metapc), not to PCem.

## Note about ROM Files
<b>NO COPYRIGHTED ROM FILES ARE INCLUDED NOR WILL THEY BE. PLEASE DO NOT ASK FOR THEM.</b>

## How to Build
You will need the following libraries and buildtools (and their dependencies):
- SDL2
- wxWidgets 3.x
- OpenAL
- CMake
- ALSA (Linux only)
- GTK3 Dev Package (Linux only)
- PCap
- LibSlirp
- Ninja (Recommended, but you can use a Makefile generator if you prefer)

Open a terminal window, navigate to the metaPC directory, create a build directory, then enter in that build directory: 
```
cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release ..
ninja
```

then `./src/pcem` to run on most platforms, and open `./src/metaPC.app` on macOS

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
  -DPLUGIN_ENGINE=ON         : Build with plugin support. Builds libpcem-plugin-api and links metaPC with it.
  -DPCEM_MARCH=x86_64-v2     : Change the architecture used for generated instructions, by default we set it for
                               >= Nehalem for Intel, and >= Bulldozer for AMD. 
```

If you are using -DCMAKE_BUILD_TYPE=Debug, there are some more debug options you can enable if needed
```
  -DPCEM_SLIRP_DEBUG=OFF           : Build metaPC with SLIRP_DEBUG debug output
  -DPCEM_RECOMPILER_DEBUG=OFF      : Build metaPC with RECOMPILER_DEBUG debug output
  -DPCEM_NE2000_DEBUG=OFF          : Build metaPC with NE2000_DEBUG debug output
  -DPCEM_EMU8K_DEBUG_REGISTERS=OFF : Build metaPC with EMU8K_DEBUG_REGISTERS debug output
  -DPCEM_SB_DSP_RECORD_DEBUG=OFF   : Build metaPC with SB_DSP_RECORD_DEBUG debug output
  -DPCEM_MACH64_DEBUG=OFF          : Build metaPC with MACH64_DEBUG debug output
  -DPCEM_DEBUG_EXTRA=OFF           : Build metaPC with DEBUG_EXTRA debug output
```

If you are using -DCMAKE_BUILD_TYPE=RelWithDebInfo, there are additional options you can do
```
  -DPCEM_RELDEB_AS_RELEASE=ON     : Builds RelWithDebInfo with debugging logging enabled when this is off
```

They are some extra modules you can add if you build with `-DUSE_EXPERIMENTAL=ON`. These modules are untested.
incomplete, and may or may not be in a future build of metaPC. We do not provide builds with these enabled as
well. It is also possible they may not even build.
```
  -DUSE_EXPERIMENTAL_PGC=ON       : Build metaPC with Professional Graphics Controller support.
  -DUSE_EXPERIMENTAL_PRINTER=ON   : Build metaPC with Printer support. Requires freetype.
``` 

CD-ROM support currently only accesses `/dev/cdrom`. It has not been heavily tested.

On Windows, if you want to use PCAP networking, you will need to install [npcap](https://npcap.com) and replace the libpcap.dll in the metaPC dir using the wpcap.dll installed in C:\Windows\System32  

## Links

### [metaPC emulates the following hardware](SYSTEMS.md):
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
