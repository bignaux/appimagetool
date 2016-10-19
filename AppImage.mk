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

clean:
	rm -f $(APPNAME).squashfs

mrproper: clean
	rm -f $(APPNAME).AppImage
