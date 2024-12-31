This is a mirror/archive of the source code of [HamClock](https://clearskyinstitute.com/ham/HamClock), primarily intended for use in maintaining the AUR and PACSTALL packages. Forked from [HamClock](https://github.com/kj7rrv/hamclock) previous maintainer, [KJ7RRV](https://github.com/kj7rrv).

* HamClock's update system is disabled in the AUR packages (but *not* in this repo). This is intentional, these patches are provided below.
* If you have a problem with HamClock itself, and it is not related to updates, see the link above.
* Please do not submit pull requests to this repo.
* Please note that this repo and the AUR and PACSTALL packages should rarely be out of date by more than a couple days. If it is out-of-odate, it is most likely due to being on vacation or because I am busy with other projects or work. Issues and Flags will not do any good.

## Patches

```
# Add -AUR to version
sed -i 's/"/-AUR"/g' version.h
sed -i 's/\t-AUR"/\t"/g' version.h

# Do not check for/install updates
sed -i "s/tft.print (F(\"You're up to date\!\"));"'/tft.print(F("Updates disabled for AUR")); tft.setCursor (tft.width()\/8, tft.height()\/3+40); tft.print(F("If this build is outdated by more than a few days,")); tft.setCursor (tft.width()\/8, tft.height()\/3+80); tft.print(F("please email fang64@gmail.com.")); wdDelay(2000);/g' ESPHamClock.ino
sed -i 's/bool found_newer = false;/return false;bool found_newer;/g' OTAupdate.cpp
```
This simply redirects users to contact AUR maintainer if they are out-of-date. It also disables the update check entirely.

Downstream libgpio patch:

```
diff -ura hamclock-4.10.orig/ESPHamClock/ArduinoLib.h hamclock-4.10.new/ESPHamClock/ArduinoLib.h
--- hamclock-4.10.orig/ESPHamClock/ArduinoLib.h	2024-11-30 16:25:57.740178616 -0500
+++ hamclock-4.10.new/ESPHamClock/ArduinoLib.h	2024-11-30 16:27:11.201553340 -0500
@@ -62,9 +62,9 @@
   #elif defined(_IS_LINUX)
     // be prepared for either gpiod or legacy broadcom memory map interface
     #if __has_include(<gpiod.h>)
-        #include <gpiod.h>
-        #define _NATIVE_GPIO_LINUX              // set for either GPIOD or GPIOBC
-        #define _NATIVE_GPIOD_LINUX             // gpiod is sufficiently mature to use
+        // #include <gpiod.h>
+        // #define _NATIVE_GPIO_LINUX              // set for either GPIOD or GPIOBC
+        // #define _NATIVE_GPIOD_LINUX             // gpiod is sufficiently mature to use
     #endif
     #if __has_include(<pigpio.h>)
         #if !defined(_NATIVE_GPIO_LINUX)
```
This is a patch to disable the use of the newer gpiod library on linux. This is because HamClock does not support the libgpiod 2.0 or greater library. After a discussion with the author of HamClock, it was decided that this feature is most likely not used by many. Since it is far easier to use a USB GPIO device over built-in GPIO pins that are board specific. 

A decision was made to potentially disable this functionality in future versions of HamClock or replace it with a entirely different library. So eventually this patch can and will be removed, whenever those changes are made.

