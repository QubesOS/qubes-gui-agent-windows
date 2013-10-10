ifeq ($(PACKAGE_SET),vm)
WIN_SOURCE_SUBDIRS = .
WIN_COMPILER = WDK
WIN_BUILD_DEPS = gui-common core-vchan-$(BACKEND_VMM)
WIN_BUILD_DEPS += vmm-xen-windows-pvdrivers
WIN_PREBUILD_CMD = powershell -executionpolicy bypass set_version.ps1
endif
