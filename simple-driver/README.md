# Simple Driver Example

This directory has two examples:

- `driver_sim.c`: a user-mode teaching harness that compiles with the installed Visual Studio `cl`.
- `simple_echo_driver.c`: a minimal Windows kernel driver source that creates a device and handles one echo IOCTL.

## Compile the Teaching Harness

```powershell
cl /nologo /W4 /Fe:driver_sim.exe driver_sim.c
.\driver_sim.exe
```

## Build the Kernel Driver

A real Windows kernel driver needs the Windows Driver Kit kernel-mode headers and libraries. This machine has Visual Studio `cl`, but the WDK `km` directory was not present when this example was created.

After installing the WDK:

```powershell
.\build-driver.ps1
```

That script builds `simple_echo.sys` from `simple_echo_driver.c`.

Do driver loading and testing only in a VM or test machine with test-signing/debugging set up. A bad kernel driver can crash Windows.
