KDIR ?= /lib/modules/$(shell uname -r)/build
SRC  := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(SRC) modules

clean:
	$(MAKE) -C $(KDIR) M=$(SRC) clean

install:
	$(MAKE) -C $(KDIR) M=$(SRC) modules_install
	depmod -a

.PHONY: default clean install