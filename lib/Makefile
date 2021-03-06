MILIB = libmil1553
RTILIB = librti
QCKLIB = libquick
TSTLIB = libquick-serial

CPU=L865

GIT_VERSION = $(shell git describe --tags --dirty --long)

include /acc/dsc/src/co/Make.auto

%.$(CPU).a: %.$(CPU).o
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

CFLAGS= -g -Wall -I. -I../driver -fPIC
CFLAGS += -DGIT_VERSION=\"$(GIT_VERSION)\"
ARFLAGS=rcv

MILSRC=$(MILIB).c $(MILIB).h $(RTILIB).c $(RTILIB).h $(QCKLIB).c \
	$(QCKLIB).h $(TSTLIB).c $(TSTLIB).h

all: $(QCKLIB).$(CPU).a $(TSTLIB).$(CPU).a

$(MILIB).$(CPU).o: $(MILSRCS)
$(RTILIB).$(CPU).o: $(MILSRCS)
$(QCKLIB).$(CPU).o: $(MILSRCS)
$(TSTLIB).$(CPU).o: $(MILSRCS)

$(QCKLIB).$(CPU).a: $(QCKLIB).$(CPU).o $(RTILIB).$(CPU).o
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@
$(TSTLIB).$(CPU).a: $(TSTLIB).$(CPU).o $(RTILIB).$(CPU).o $(MILIB).$(CPU).o
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

clean:
	-$(RM) -rf *.$(CPU).a *.$(CPU).o 
	-$(RM) -rf html Doxyfile

install:
	dsc_install libquick.$(CPU).a /acc/local/$(CPU)/mil1553
	dsc_install libmil1553.h /acc/local/$(CPU)/mil1553
	dsc_install librti.h /acc/local/$(CPU)/mil1553
	dsc_install libquick.h /acc/local/$(CPU)/mil1553
	dsc_install ../driver/mil1553.h /acc/local/$(CPU)/mil1553

docs: Doxyfile.patch
	doxygen -s -g
	patch -p0 < Doxyfile.patch
	doxygen
	rm Doxyfile
