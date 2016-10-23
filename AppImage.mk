# Copyright (c) 2016 Bignaux Ronan

RUNTIME				?=	./build/runtime
APPNAME				?=	appimagetool
transport			?=	bintray-zsync
bintray-user		?=	probono
bintray-repo		?=	AppImages
zsync				?=	RetroArch-_latestVersion-x86_64.AppImage.zsync
COMP				?=	lz4
mksquashfs_options	?=	-root-owned -noappend -comp $(COMP)

all: $(APPNAME).AppImage
.PHONY: all clean mrproper
.PRECIOUS: %.squashfs

%.AppImage: $(RUNTIME) %.squashfs
	cat $^ > $@ && chmod a+x $@

%.squashfs: %.AppDir $(shell find $(APPNAME).AppDir)
	mksquashfs $< $@ $(mksquashfs_options)

%.sha256: %.AppImage
	sha256sum $< | cut -d " " -f 1 > $@

%.asc: %.sha256
	gpg2 --detach-sign --armor $< -o $@
	cat $@

#objdump -h -j .sha256_sig krita-3.0.1.1-x86_64.AppImage
printoffset: $(APPNAME).AppImage
	SIGHEXOFFSET=$$(objdump -h $< | grep .sha256_sig | awk '{print $6}')
	SIGHEXLENGTH=$$(objdump -h $< | grep .gpg_sig | awk '{print $3}')
	echo $(SIGHEXOFFSET) $(SIGHEXLENGTH)

#%.asc : %.AppImage
#	digest $< > $@

clean:
	rm -f $(APPNAME).squashfs

mrproper: clean
	rm -f $(APPNAME).AppImage
