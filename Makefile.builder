ifeq ($(PACKAGE_SET),vm)
WIN_SOURCE_SUBDIRS = .
WIN_COMPILER = WDK
WIN_BUILD_DEPS = gui-common core-vchan-$(BACKEND_VMM)
WIN_BUILD_DEPS += vmm-xen-windows-pvdrivers windows-utils
WIN_PREBUILD_CMD = powershell -executionpolicy bypass set_version.ps1

# This is the simplest way to build 32-bit binaries in a 64-bit build
# with the current builder architecture (and WDK 7.1)...
ifeq ($(DDK_ARCH),x64)
WIN_PREBUILD_CMD += && build32.bat
endif

endif
