#========================================================
# Makefile for EM DSC & DSCPM and associated RT tasks
#========================================================



ifndef CPU
CPU=L865
endif
EM=POW
CFLAGS=-g -Wall
LDFLAGS=-g

ifeq ($(CPU),ppc4)
PROCS=pow-v_procos.c pow_procos.c
RTS= powrt.c powvrt.c
LDLIBS=-lgm -ldscrt $(TGMLIBS) -ldrvrutil -lquick -lerr -lerr_dummy $(XTRALIBS) -lbsd
else
PROCS=pci_procos.c
RTS=  pci-powvrt.c
CFLAGS += -I/acc/local/$(CPU)/mil1553
LDLIBS=-lgm -ldscrt $(TGMLIBS) -L/acc/local/$(CPU)/mil1553 -lquick -ldrvrutil -lerr $(XTRALIBS)
endif
RTX=



include ../Make.gen


clean:
	$(RM) $(LIB) $(PROCO) *.$(CPU).o $(RTE) $(RTO) $(BAKS)


powvrt.$(CPU).o: powrt.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -DV_VERSION -c $< -o $@
	
powvrt-ade.$(CPU).o: powrt-ade.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -DV_VERSION -c $< -o $@	

powvrt-cngs.$(CPU).o: powrt-cngs.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -DV_VERSION -c $< -o $@
	
mg64t.$(CPU).o: mg64t.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@		
	
powvrt.$(CPU).o: pow_messages.h
pow-v_procos.$(CPU).o: pow_messages.h

powvrt-pci.$(CPU).o: powvrt-pci.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

powvrt-pci.$(CPU): powvrt-pci.$(CPU).o
	$(LINK.o) $^  $(LOADLIBES) $(LDLIBS) -o $@
