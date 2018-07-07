ifeq ($(PACKAGE_SET),vm)
WIN_SOURCE_SUBDIRS = .
WIN_SLN_DIR = vs2017
WIN_COMPILER = msbuild
WIN_BUILD_DEPS = gui-common core-vchan-$(BACKEND_VMM) core-qubesdb windows-utils
WIN_PREBUILD_CMD = set_version.bat && powershell -executionpolicy bypass -File set_version.ps1 < nul
endif
