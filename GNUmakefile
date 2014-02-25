# -*- mode: Makefile; -*-

## -----------------------------------------------------------------------------
## --SECTION--                                                  COMMON VARIABLES
## -----------------------------------------------------------------------------

-include Makefile

VERSION_MAJOR := $(wordlist 1,1,$(subst ., ,$(VERSION)))
VERSION_MINOR := $(wordlist 2,2,$(subst ., ,$(VERSION)))
VERSION_PATCH := $(wordlist 3,3,$(subst ., ,$(VERSION)))

## -----------------------------------------------------------------------------
## --SECTION--                                                   SPECIAL TARGETS
## -----------------------------------------------------------------------------

################################################################################
### @brief setup
################################################################################

.PHONY: setup

setup:
	@echo ACLOCAL
	@aclocal -I m4
	@echo AUTOMAKE
	@automake --add-missing --force-missing --copy
	@echo AUTOCONF
	@autoconf -I m4
	@echo auto system configured, proceed with configure

################################################################################
### @brief add maintainer files
################################################################################

MAINTAINER = \
	README \
	arangod/Ahuacatl/ahuacatl-tokens.c \
	arangod/Ahuacatl/ahuacatl-grammar.c \
	arangod/Ahuacatl/ahuacatl-grammar.h \
	lib/JsonParser/json-parser.c \
	lib/V8/v8-json.cpp \
	lib/V8/v8-json.h \
	lib/BasicsC/voc-errors.h \
	lib/BasicsC/voc-errors.c \
	js/common/bootstrap/errors.js \
	mr/common/bootstrap/mr-error.h

AUTOMAGIC = \
	Makefile.in \
	aclocal.m4 \
	configure \
	config/compile \
	config/config.guess \
	config/config.sub \
	config/depcomp \
	config/install-sh \
	config/missing

.PHONY: add-maintainer add-automagic

add-maintainer:
	@echo adding generated files to GIT
	git add -f $(MAINTAINER)

remove-maintainer:
	@echo removing generated files from GIT
	git rm -f $(MAINTAINER)

add-automagic:
	@echo adding automagic files to GIT
	git add -f $(AUTOMAGIC)

remove-automagic:
	@echo removing automagic files from GIT
	git rm -f $(AUTOMAGIC)

################################################################################
### @brief make love
################################################################################

love: 
	@echo ArangoDB loves you

## -----------------------------------------------------------------------------
## --SECTION--                                                     CMAKE & CPACK
## -----------------------------------------------------------------------------

################################################################################
### @brief MacOSX bundle
################################################################################

.PHONY: pack-dmg pack-dmg-cmake

DMG_NAME := ArangoDB-CLI.app

pack-dmg:
	rm -rf Build && mkdir Build

	./configure \
		--prefix=/opt/arangodb \
		--enable-all-in-one-icu \
		--enable-all-in-one-v8 \
		--enable-all-in-one-libev \
		--enable-mruby

	${MAKE} pack-dmg-cmake

pack-dmg-cmake:
	cd Build && cmake \
		-D "BUILD_PACKAGE=dmg-cli" \
		-D "CMAKE_INSTALL_PREFIX=${prefix}" \
		-D "USE_MRUBY=ON" \
		-D "ARANGODB_VERSION=${VERSION}" \
		-D "CPACK_PACKAGE_VERSION_MAJOR=${VERSION_MAJOR}" \
		-D "CPACK_PACKAGE_VERSION_MINOR=${VERSION_MINOR}" \
		-D "CPACK_PACKAGE_VERSION_PATCH=${VERSION_PATCH}" \
		..

	${MAKE} .libev-build-64
	${MAKE} .zlib-build-64
	${MAKE} .icu-build-64
	${MAKE} .v8-build-64
	${MAKE} .mruby-build-64

	${MAKE} ${BUILT_SOURCES}

	cd Build && ${MAKE}

	cd Build && cpack \
		-G Bundle \
		-D "CPACK_INSTALL_PREFIX=${DMG_NAME}/Contents/MacOS/opt/arangodb"

################################################################################
### @brief MacOSX
################################################################################

.PHONY: pack-macosx pack-macosx-cmake

PACK_DESTDIR ?= .

pack-macosx:
	rm -rf Build && mkdir Build

	./configure \
		--prefix=/opt/arangodb \
		--enable-all-in-one-icu \
		--enable-all-in-one-v8 \
		--enable-all-in-one-libev \
		--enable-mruby

	${MAKE} pack-macosx-cmake

pack-macosx-cmake:
	cd Build && cmake \
		-D "BUILD_PACKAGE=dmg-cli" \
		-D "CMAKE_INSTALL_PREFIX=${prefix}" \
		-D "USE_MRUBY=ON" \
		-D "ARANGODB_VERSION=${VERSION}" \
		-D "CPACK_PACKAGE_VERSION_MAJOR=${VERSION_MAJOR}" \
		-D "CPACK_PACKAGE_VERSION_MINOR=${VERSION_MINOR}" \
		-D "CPACK_PACKAGE_VERSION_PATCH=${VERSION_PATCH}" \
		..

	${MAKE} .libev-build-64
	${MAKE} .zlib-build-64
	${MAKE} .icu-build-64
	${MAKE} .v8-build-64
	${MAKE} .mruby-build-64

	${MAKE} ${BUILT_SOURCES}

	cd Build && ${MAKE}
	cd Build && ${MAKE} install DESTDIR=${PACK_DESTDIR}

################################################################################
### @brief debian arm package
################################################################################

.PHONY: pack-arm pack-arm-cmake

pack-arm:
	rm -rf Build && mkdir Build

	./configure \
		--prefix=/usr \
		--sysconfdir=/etc \
		--localstatedir=/var \
		--disable-all-in-one-icu \
		--disable-all-in-one-v8 \
		--disable-all-in-one-libev \
		--with-libev=./3rdParty-ARM \
		--with-v8=./3rdParty-ARM \
		--disable-mruby

	${MAKE} pack-arm-cmake

pack-arm-cmake:
	cd Build && cmake \
		-D "BUILD_PACKAGE=raspbian" \
		-D "CMAKE_INSTALL_PREFIX=${prefix}" \
		-D "ETCDIR=${sysconfdir}" \
		-D "VARDIR=${localstatedir}" \
		-D "USE_MRUBY=OFF" \
		-D "ARANGODB_VERSION=${VERSION}" \
		-D "CPACK_PACKAGE_VERSION_MAJOR=${VERSION_MAJOR}" \
		-D "CPACK_PACKAGE_VERSION_MINOR=${VERSION_MINOR}" \
		-D "CPACK_PACKAGE_VERSION_PATCH=${VERSION_PATCH}" \
                -D "CMAKE_CXX_FLAGS_RELEASE:STRING=-O2 -DNDEBUG" \
                -D "CMAKE_C_FLAGS_RELEASE:STRING=-O2 -DNDEBUG" \
		..

	${MAKE} ${BUILT_SOURCES}

	cd Build && ${MAKE}

	cd Build && cpack \
		-G DEB

################################################################################
### @brief Windows 64-bit bundle
################################################################################

.PHONY: pack-win32 pack-winXX pack-winXX-cmake

pack-win32:
	$(MAKE) pack-winXX BITS=32 TARGET="Visual Studio 12"

pack-win64:
	$(MAKE) pack-winXX BITS=64 TARGET="Visual Studio 12 Win64"

pack-winXX:
	rm -rf Build$(BITS) && mkdir Build$(BITS)

	${MAKE} pack-winXX-cmake BITS="$(BITS)" TARGET="$(TARGET)" VERSION="`awk '{print substr($$3,2,length($$3)-2);}' build.h`"

pack-winXX-cmake:
	cd Build$(BITS) && cmake \
		-G "$(TARGET)" \
		-D "USE_MRUBY=OFF" \
		-D "ARANGODB_VERSION=${VERSION}" \
		-D "CPACK_PACKAGE_VERSION_MAJOR=${VERSION_MAJOR}" \
		-D "CPACK_PACKAGE_VERSION_MINOR=${VERSION_MINOR}" \
		-D "CPACK_PACKAGE_VERSION_PATCH=${VERSION_PATCH}" \
		..

	cd Build$(BITS) && cmake --build . --config Release

	cd Build$(BITS) && cpack -G NSIS

################################################################################
### @brief cmake build
################################################################################

.PHONY: cmake-setup

cmake-setup:
	test -d Build || mkdir Build

	cd Build && cmake \
		-D "CMAKE_INSTALL_PREFIX=${prefix}" \
		-D "ETCDIR=${sysconfdir}" \
		-D "VARDIR=${localstatedir}" \
		\
		-D ARANGODB_VERSION=${VERSION} \
		-D USE_MRUBY=${tr_MRUBY} \
		\
		-D ICU_INCLUDE= \
		-D ICU_LIB_PATH= \
		-D ICU_LIBS= \
		-D ICU_VERSION= \
		\
		-D LIBEV_INCLUDE= \
		-D LIBEV_LIB_PATH= \
		-D LIBEV_LIBS= \
		-D LIBEV_VERSION= \
		\
		-D MRUBY_INCLUDE= \
		-D MRUBY_LIB_PATH= \
		-D MRUBY_LIBS= \
		-D MRUBY_VERSION= \
		\
		-D OPENSSL_INCLUDE= \
		-D OPENSSL_LIB_PATH= \
		-D OPENSSL_LIBS= \
		-D OPENSSL_VERSION= \
		\
		-D READLINE_INCLUDE= \
		-D READLINE_LIB_PATH= \
		-D READLINE_LIBS= \
		-D READLINE_VERSION= \
		\
		-D V8_INCLUDE= \
		-D V8_LIB_PATH= \
		-D V8_LIBS= \
		-D V8_VERSION= \
		\
		-D ZLIB_INCLUDE= \
		-D ZLIB_LIB_PATH= \
		-D ZLIB_LIBS= \
		-D ZLIB_VERSION= \
		\
		..

## -----------------------------------------------------------------------------
## --SECTION--                                                       END-OF-FILE
## -----------------------------------------------------------------------------

## Local Variables:
## mode: outline-minor
## outline-regexp: "### @brief\\|## --SECTION--\\|# -\\*-"
## End:
