# Qubes GUI agent

- TODO: build test tools
- TODO: (watchdog) detect if the agent fails/crashes too often and disable it/return to fullscreen mode
- TODO: consider rewriting window tracking logic to use windows hooks intead of polling (I don't remember why hooks weren't used in the first place, maybe they don't work reliably for all windows since DLL injection is needed and that breaks for protected processes like winlogon)
- TODO: custom WDDM driver (maybe some time in the future)

## Local command-line build on Windows

### Prerequisites

- Microsoft EWDK iso mounted as a drive
- `qubes-builderv2`
- `powershell-yaml` PowerShell package (run `powershell -command Install-Package powershell-yaml` as admin)
  (TODO: provide offline installer for this)
- `vmm-xen-windows-pvdrivers`, `core-vchan-xen`, `windows-utils`, `core-qubesdb` and `gui-common`
  built with the same `output_dir` as below

### Build

- run `powershell qubes-builderv2\qubesbuilder\plugins\build_windows\scripts\local\build.ps1 src_dir output_dir Release|Debug`
