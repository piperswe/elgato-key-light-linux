NAME    := keylights-tray
VERSION := 0.1.0
SPEC    := $(NAME).spec

RPMTOP  := $(HOME)/rpmbuild
TARBALL := $(NAME)-$(VERSION).tar.gz

.PHONY: run probe rpm install uninstall clean

## run: launch the tray app from this checkout (development)
run:
	./tray/keylights-tray.py

## probe: dump the raw USB HID exchange with any attached Key Light Neo
probe:
	./tray/usbneo.py

## rpm: build a binary RPM from the current working tree
rpm:
	mkdir -p $(RPMTOP)/SOURCES
	# Include tracked and untracked (but not gitignored) files, so the build
	# works whether or not the changes have been committed yet.
	git ls-files -co --exclude-standard \
		| tar -czf $(RPMTOP)/SOURCES/$(TARBALL) \
			--transform 's,^,$(NAME)-$(VERSION)/,' -T -
	rpmbuild -bb $(SPEC)
	@echo
	@echo "Built RPM(s):"
	@ls -1 $(RPMTOP)/RPMS/noarch/$(NAME)-$(VERSION)-*.rpm

## install: build and install the RPM via dnf
install: rpm
	sudo dnf reinstall -y $(RPMTOP)/RPMS/noarch/$(NAME)-$(VERSION)-*.rpm

## uninstall: remove the installed RPM
uninstall:
	sudo dnf remove -y $(NAME)

## clean: remove built tarball
clean:
	rm -f $(RPMTOP)/SOURCES/$(TARBALL)
