This is an example of how to use [libesphttpd](https://github.com/chmorgan/libesphttpd) with the Espressif FreeRTOS SDK [ESP-IDF](https://github.com/espressif/esp-idf).

# ESP32

Set-up your build environment by following the [instructions](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html)

After cloning this repository, ensure that you have updated submodules appropriately:

```git submodule update --init --recursive```

Run the esp32 makefile (make sure you enable the esphttpd component) and build

```make```

Load onto your esp32 and monitor

```make flash monitor```

### Using file systems

* disclaimer: Althought the framework is there for SPIFFS, FatFS, and LittleFS,  I was only able to get FatFS working correctly.  Bugfixes are welcomed!

Detailed information about using file systems are available from @loboris  [Wiki](https://github.com/loboris/MicroPython_ESP32_psRAM_LoBo/wiki/filesystems).

The type and size of the Flash file system can be configured before the build using 
menuconfig → Example Configuration → File systems.
Three file system types are available: SPIFFS, FatFS and LittleFS.

If using FatFS, then I recommend setting Code Page=US, Long file name support=heap, and max long file name length=127 in
```menuconfig → Component config → FAT Filesystem support```.  FatFS uses esp-idf wear leveling.

Start address in Flash and file system size are configured automatically from ```BUILD.sh``` based on Flash size, partition layout and command line options.

Internal file system is mounted automatically at boot in ```/internalfs``` directory.
If the internal file system is not formated, it will be formated automatically on first boot.

# ESP8266

Building for ESP8266 requires a bit more work.

Ensure that you are defining SDK_PATH to point at the ESP8266_RTOS_SDK:

```export SDK_PATH=/some/path/ESP8266_RTOS_SDK```

Update your path to point at your xtensa-lx106 tool chain, something like:

```export PATH=/some/path/esp-open-sdk/xtensa-lx106-elf/bin:$PATH```

If you don't have these tools you can get the SDK from:
https://github.com/espressif/ESP8266_RTOS_SDK

And you can get the crosstool source (you'll have to build the toolchain yourself) from :
https://github.com/pfalcon/esp-open-sdk

And then you should be able to build with:

```make -f Makefile.esp8266 USE_OPENSDK=yes FREERTOS=yes -C libesphttpd```

# Old notes

(I'm not sure these still apply, I mostly build for ESP32, feedback and pull requests welcome in this area)

The Makefile in this example isn't perfect yet: something is going wrong with the dependency
checking, causing files that have changed not always being recompiled. To work around this,
please run a
```make clean && make -C libesphttpd && make```
before running
```make flash```

