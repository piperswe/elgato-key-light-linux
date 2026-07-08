NAME    := keylights-tray
VERSION := 0.2.0
SPEC    := $(NAME).spec

RPMTOP  := $(HOME)/rpmbuild
TARBALL := $(NAME)-$(VERSION).tar.gz
ARCHDIR := packaging/arch
ARCH    := $(shell uname -m)

.PHONY: build run probe test rpm deb arch arch-tarball install uninstall clean

## build: configure and compile with Meson
build:
	meson setup build || meson setup build --reconfigure
	meson compile -C build

## run: launch the tray app from this checkout (development)
run: build
	./build/$(NAME)

## probe: dump the raw USB HID exchange with any attached Key Light Neo
probe: build
	./build/$(NAME) --probe

## test: run the unit tests
test: build
	meson test -C build

# Assemble a source tarball from the working tree (tracked + untracked, minus
# gitignored files) so the build works whether or not changes are committed.
define make_tarball
	git ls-files -co --exclude-standard \
		| tar -czf "$(1)" --transform 's,^,$(NAME)-$(VERSION)/,' -T -
endef

## rpm: build a binary RPM from the current working tree
rpm:
	mkdir -p $(RPMTOP)/SOURCES
	$(call make_tarball,$(RPMTOP)/SOURCES/$(TARBALL))
	rpmbuild -bb $(SPEC)
	@echo
	@echo "Built RPM(s):"
	@ls -1 $(RPMTOP)/RPMS/$(ARCH)/$(NAME)-$(VERSION)-*.rpm

## deb: build a binary .deb from the current working tree
deb:
	dpkg-buildpackage -us -uc -b
	@echo
	@echo "Built .deb(s):"
	@ls -1 ../$(NAME)_$(VERSION)_*.deb

## arch-tarball: stage the source tarball where makepkg expects it
arch-tarball:
	$(call make_tarball,$(ARCHDIR)/$(TARBALL))

## arch: build an Arch package (run as a non-root user; makepkg refuses root)
arch: arch-tarball
	cd $(ARCHDIR) && makepkg -f
	@echo
	@echo "Built Arch package(s):"
	@ls -1 $(ARCHDIR)/$(NAME)-$(VERSION)-*.pkg.tar.zst

## install: build and install the RPM via dnf
install: rpm
	sudo dnf reinstall -y $(RPMTOP)/RPMS/$(ARCH)/$(NAME)-$(VERSION)-*.rpm

## uninstall: remove the installed RPM
uninstall:
	sudo dnf remove -y $(NAME)

## clean: remove build directories and packaging artifacts
clean:
	rm -rf build build-*
	rm -f $(RPMTOP)/SOURCES/$(TARBALL)
	rm -rf $(ARCHDIR)/src $(ARCHDIR)/pkg
	rm -f $(ARCHDIR)/*.tar.gz $(ARCHDIR)/*.pkg.tar.zst
	rm -f ../$(NAME)_$(VERSION)_*.deb ../$(NAME)_$(VERSION)*.buildinfo \
		../$(NAME)_$(VERSION)*.changes
