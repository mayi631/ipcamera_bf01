include $(SRCTREE)/build/rule.mk

TOPTARGETS := all

module-$(CONFIG_MODULE_RTSP)		+= cvi_osal
module-$(CONFIG_MODULE_RTSP)		+= cvi_rtsp
module-$(CONFIG_MODULE_RTSP)		+= ringbuffer
module-$(CONFIG_MODULE_RECORD)		+= ringbuffer

SUBDIRS += $(module-y)

$(TOPTARGETS): $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@ $(MAKECMDGOALS)

clean :
	$(MAKE) -C cvi_osal clean
	$(MAKE) -C cvi_rtsp clean
	$(MAKE) -C ringbuffer clean

install :

.PHONY: $(TOPTARGETS) $(SUBDIRS)
