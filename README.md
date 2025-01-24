# Overview
A portable beatmaker for [CYD (cheap yellow display, aka ESP32-2432S028R)](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display).
Create and sequence loops into complete songs with 3 synth tracks and 1 drum track.

# Build
Open in VS Code with the PlatformIO extension, apply the Mozzi Patch below, connect your CYD via USB, then click "upload" to build and deploy.

Side note: I don't bother with Arduino IDE much these days, as it requires too much messing about with board selection, port speed selection, and manual library management with global versions. VSCode+PlatformIO provides easy one-click build-and-deploy from a git clone.

# Mozzi Patch
The touchscreen input configuration gets destroyed after startMozzi() is called. For now, the easiest workaround is to patch libdeps\cyd2usb\Mozzi\internal\MozziGuts_impl_ESP32.hpp like this:
```diff
#  elif MOZZI_IS(MOZZI_AUDIO_MODE, MOZZI_OUTPUT_INTERNAL_DAC)
-  i2s_set_pin((i2s_port_t)i2s_num, NULL);
-  i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN);
+  i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN); // output only on GPIO26, since GPIO25 is used for touchscreen on CYD
#  endif
```

# Hardware
Tested on the CYD2USB variant (eg [this one](https://www.aliexpress.com/item/1005006470918908.html) with USB-C) of the original CYD.
Probably works on the original with micro-USB only, using the "cyd" environment.

# License
[GNU General Public License v3.0](https://www.gnu.org/licenses/gpl-3.0.html)

# Credits
This is a port of Staffan Melin's [OscPocketM](https://www.oscillator.se/opensource/),
originally for the [M5Stack Core2](https://docs.m5stack.com/en/core/core2) licensed as GNU General Public License v3.0

This port has made minimal modifications to the original OscPocketM source via a minimal/janky implementation of the M5Stack libraries, but any future features will probably replace this with a HAL abstraction.
