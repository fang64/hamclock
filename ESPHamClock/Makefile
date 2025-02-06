# Makefile for HamClock on linux or macos
# type make help for possible targets

# HamClock can be built for 16 or 32 bit frame buffers. Default is 32, add following define for 16:
# -D_16BIT_FB

# always runs these non-file targets
.PHONY: clean clobber help hclibs

# build flags common to all options and architectures
CXXFLAGS = -IArduinoLib -IwsServer/include -Izlib-hc -I. -g -O2 -Wall -pthread -std=c++17
# CXXFLAGS += -Wextra -pedantic -Werror -Wno-attributes -Wno-unknown-pragmas
LDXXFLAGS = -LArduinoLib -LwsServer -Lzlib-hc -g -pthread
LIBS = -lpthread -larduino -lzlib-hc -lws
CXX = g++

# macOS does not have X11 by default; this assumes XQuartz has been installed
ifeq ($(shell uname -s), Darwin)
    CXXFLAGS += -I/opt/X11/include
    LDXXFLAGS += -L/opt/X11/lib
endif

# FreeBSD needs libgpio libexecinfo and local xorg
ifeq ($(shell uname -s), FreeBSD)
    CXXFLAGS += -I/usr/local/include
    LDXXFLAGS += -L/usr/local/lib
    LIBS += -lgpio -lexecinfo -lm
endif

# Linux needs libgpiod
ifeq ($(shell find /usr/lib -name libgpiod.a | wc -l), 1)
    LIBS += -lgpiod
endif


# make CXXFLAGS available to sub makes
export CXXFLAGS


OBJS = \
	BME280.o \
	ESPHamClock.o \
	Germano-Bold-16.o \
	Germano-Bold-30.o \
	Germano-Regular-16.o \
	OTAupdate.o \
	P13.o \
	adif.o \
	adif_parser.o \
	asknewpos.o \
	astro.o \
	bands.o \
	blinker.o \
	brightness.o \
	cachefile.o \
	callsign.o \
	clocks.o \
	cities.o \
	color.o \
	configs.o \
	contests.o \
	cputemp.o \
	debug.o \
	drawextra.o \
	dxcluster.o \
	earthmap.o \
	earthsat.o \
	emetool.o \
	favicon.o \
        fsfree.o \
	gimbal.o \
	gpsd.o \
	grayline.o \
	kd3tree.o \
	liveweb.o \
	liveweb-html.o \
	magdecl.o \
	maidenhead.o \
	mapmanage.o \
	menu.o \
	moon_imgs.o \
	moonpane.o \
	ncdxf.o \
	nmea.o \
	nvram.o \
	ontheair.o \
	parsespot.o \
	passwd.o \
	plot.o \
	plotmap.o \
	plotmgmnt.o \
	prefixes.o \
	pskreporter.o \
	qrz.o \
	radio.o \
	robinson.o \
	rss.o \
	runner.o \
	santa.o \
	sattool.o \
	scroll.o \
	sdo.o \
	selectFont.o \
	setup.o \
	sevenseg.o \
	spacewx.o \
	sphere.o \
	spots.o \
	stopwatch.o \
	string.o \
	touch.o \
	tz.o \
	version.o \
	webserver.o \
	watchlist.o \
	wifi.o \
	wifimeter.o \
	wx.o \
	zones.o

help:
	@printf "\nThe following targets are available (as appropriate for your system)\n\n"
	@printf "    hamclock-800x480          X11 GUI desktop version, AKA hamclock\n"
	@printf "    hamclock-1600x960         X11 GUI desktop version, larger, AKA hamclock-big\n"
	@printf "    hamclock-2400x1440        X11 GUI desktop version, larger yet\n"
	@printf "    hamclock-3200x1920        X11 GUI desktop version, huge\n"
	@printf "\n";
	@printf "    hamclock-web-800x480      web server only (no display)\n"
	@printf "    hamclock-web-1600x960     web server only (no display), larger\n"
	@printf "    hamclock-web-2400x1440    web server only (no display), larger yet\n"
	@printf "    hamclock-web-3200x1920    web server only (no display), huge\n"
	@printf "\n";
	@printf "    hamclock-fb0-800x480      RPi stand-alone /dev/fb0, AKA hamclock-fb0-small\n"
	@printf "    hamclock-fb0-1600x960     RPi stand-alone /dev/fb0, larger, AKA hamclock-fb0\n"
	@printf "    hamclock-fb0-2400x1440    RPi stand-alone /dev/fb0, larger yet\n"
	@printf "    hamclock-fb0-3200x1920    RPi stand-alone /dev/fb0, huge\n"


# supporting libs
hclibs:
	$(MAKE) -C ArduinoLib libarduino.a
	$(MAKE) -C wsServer libws.a
	$(MAKE) -C zlib-hc libzlib-hc.a



# X11 versions

# N.B. do it but also remain backward compatable

hamclock-big: hamclock-1600x960
	cp $? $@

hamclock: hamclock-800x480
	cp $? $@


hamclock-800x480: CXXFLAGS+=-D_USE_X11
hamclock-800x480: LIBS+=-lX11
hamclock-800x480: $(OBJS) hclibs
	$(CXX) $(LDXXFLAGS) $(OBJS) -o $@ $(LIBS)


hamclock-1600x960: CXXFLAGS+=-D_USE_X11 -D_CLOCK_1600x960
hamclock-1600x960: LIBS+=-lX11
hamclock-1600x960: $(OBJS) hclibs
	$(CXX) $(LDXXFLAGS) $(OBJS) -o $@ $(LIBS)


hamclock-2400x1440: CXXFLAGS+=-D_USE_X11 -D_CLOCK_2400x1440
hamclock-2400x1440: LIBS+=-lX11
hamclock-2400x1440: $(OBJS) hclibs
	$(CXX) $(LDXXFLAGS) $(OBJS) -o $@ $(LIBS)


hamclock-3200x1920: CXXFLAGS+=-D_USE_X11 -D_CLOCK_3200x1920
hamclock-3200x1920: LIBS+=-lX11
hamclock-3200x1920: $(OBJS) hclibs
	$(CXX) $(LDXXFLAGS) $(OBJS) -o $@ $(LIBS)




# web server versions


hamclock-web-800x480: CXXFLAGS+=-D_WEB_ONLY
hamclock-web-800x480: $(OBJS) hclibs
	$(CXX) $(LDXXFLAGS) $(OBJS) -o $@ $(LIBS)


hamclock-web-1600x960: CXXFLAGS+=-D_WEB_ONLY -D_CLOCK_1600x960
hamclock-web-1600x960: $(OBJS) hclibs
	$(CXX) $(LDXXFLAGS) $(OBJS) -o $@ $(LIBS)


hamclock-web-2400x1440: CXXFLAGS+=-D_WEB_ONLY -D_CLOCK_2400x1440
hamclock-web-2400x1440: $(OBJS) hclibs
	$(CXX) $(LDXXFLAGS) $(OBJS) -o $@ $(LIBS)


hamclock-web-3200x1920: CXXFLAGS+=-D_WEB_ONLY -D_CLOCK_3200x1920
hamclock-web-3200x1920: $(OBJS) hclibs
	$(CXX) $(LDXXFLAGS) $(OBJS) -o $@ $(LIBS)




# RPi fb0 versions

# N.B. do it but also remain backward compatable

hamclock-fb0-small: hamclock-fb0-800x480
	cp $? $@

hamclock-fb0: hamclock-fb0-1600x960
	cp $? $@


hamclock-fb0-800x480: CXXFLAGS+=-D_USE_FB0
hamclock-fb0-800x480: $(OBJS) hclibs
	$(CXX) $(LDXXFLAGS) $(OBJS) -o $@ $(LIBS)


hamclock-fb0-1600x960: CXXFLAGS+=-D_USE_FB0 -D_CLOCK_1600x960
hamclock-fb0-1600x960: $(OBJS) hclibs
	$(CXX) $(LDXXFLAGS) $(OBJS) -o $@ $(LIBS)


hamclock-fb0-2400x1440: CXXFLAGS+=-D_USE_FB0 -D_CLOCK_2400x1440
hamclock-fb0-2400x1440: $(OBJS) hclibs
	$(CXX) $(LDXXFLAGS) $(OBJS) -o $@ $(LIBS)


hamclock-fb0-3200x1920: CXXFLAGS+=-D_USE_FB0 -D_CLOCK_3200x1920
hamclock-fb0-3200x1920: $(OBJS) hclibs
	$(CXX) $(LDXXFLAGS) $(OBJS) -o $@ $(LIBS)



install:
	@SOURCE=hamclock-*0x*0 ; \
	TARGET=/usr/local/bin/hamclock ; \
	if ! [ -x $$SOURCE ] ; then \
	    echo 'make something first' ; \
	elif [ `id -un` != 'root' ] ; then \
	    echo please run with sudo ; \
	else \
	    cp -f $$SOURCE $$TARGET \
	    && chown root $$TARGET \
	    && chmod u+s $$TARGET \
	    && echo $$SOURCE is ready as $$TARGET ; \
	fi

clean clobber:
	$(MAKE) -C ArduinoLib clean
	$(MAKE) -C wsServer clean
	$(MAKE) -C zlib-hc clean
	touch x.o x.dSYM hamclock hamclock-
	rm -rf *.o *.dSYM hamclock hamclock-*
