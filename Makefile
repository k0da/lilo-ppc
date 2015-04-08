VERSION = 22.8
dist:
	mkdir -p ../lilo-ppc-$(VERSION)
	rsync -r --exclude=.git ./ ../lilo-ppc-${VERSION}
	tar -cjf lilo-ppc-${VERSION}.tar.bz2 ../lilo-ppc-${VERSION}
