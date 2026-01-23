# Axelera AI Linux PCIe Driver

Linux kernel module for Axelera AI devices.

## Prerequisites

**Supported Operating Systems:**
- Ubuntu 22.04 LTS
- Ubuntu 24.04 LTS

**Requirements:**
- Linux kernel headers for your target kernel version
- GCC and build tools (make, etc.)
- Root privileges for installation

To install kernel headers:

```bash
sudo apt-get install linux-headers-$(uname -r)
```

## Building

### Build with System Kernel

To build the driver module for your currently running kernel:

```bash
make          # Build all modules
make clean    # Clean build artifacts
```

### Build with Custom Kernel

To build against a custom kernel build directory:

```bash
make KDIR=/path/to/kernel/build
```

Where `KDIR` points to the configured kernel source tree or build output directory.

## Installation

To install the built module to the system:

```bash
sudo make install
```

This installs the module to `/lib/modules/$(uname -r)/extra/`.

To load the module:

```bash
sudo modprobe metis
```

## Module Details

This driver consists of the following module:

- **metis.ko** - Main PCIe driver for Axelera AI devices

## Uninstallation

To unload the module:

```bash
sudo modprobe -r metis
```

## Troubleshooting

### Module fails to load

Check kernel logs for errors:
```bash
dmesg | tail -n 50
```

### Build fails with missing headers

Ensure kernel headers match your running kernel version:
```bash
uname -r  # Check running kernel version
ls /lib/modules/$(uname -r)/build  # Verify headers are installed
```

## License

See LICENSE file for details.
