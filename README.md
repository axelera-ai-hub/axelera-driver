# Axelera AI Linux PCIe Driver

Linux kernel module for Axelera AI devices.

## Prerequisites

**Supported Operating Systems:**
- Ubuntu 22.04 LTS
- Ubuntu 24.04 LTS
- Red Hat Enterprise Linux 10 [Experimental]

**Requirements:**
- Linux kernel headers for your target kernel version
- GCC and build tools (make, etc.)
- Root privileges for installation

To install kernel headers:

**Ubuntu/Debian:**
```bash
sudo apt-get install linux-headers-$(uname -r)
```

**RHEL/Fedora:**
```bash
sudo dnf install kernel-devel-$(uname -r)
```

## Installation Options

You can install this driver using one of two methods:

1. **DKMS Package**: Automated installation with automatic rebuilds on kernel updates
2. **Manual Build and Install**: Direct compilation and installation

Choose one method below.

## Option 1: DKMS Package Installation

DKMS (Dynamic Kernel Module Support) automatically rebuilds the driver when you install a new kernel, ensuring the driver remains functional across kernel updates without manual intervention.

### Prerequisites for DKMS

Install required build tools and DKMS:

**Ubuntu/Debian:**
```bash
sudo apt install dkms debhelper cmake fakeroot
```

**RHEL/Fedora:**
```bash
sudo dnf install dkms cmake rpm-build fakeroot
```

### Building the DKMS Package

Navigate to the repository root and build the DKMS package:

```bash
mkdir -p dkms/build
cd dkms/build
cmake ../
```

**Ubuntu/Debian:**
```bash
make metis-dkms-deb-pkg
```
This generates `metis-dkms_<version>_all.deb` in the build directory.

**RHEL/Fedora:**
```bash
make metis-dkms-rpm-pkg
```
This generates `metis-dkms-<version>-1.el10.noarch.rpm` in `rpmbuild/RPMS/noarch/`.

### Installing the DKMS Package

If you previously installed the driver manually, unload it first:

```bash
sudo modprobe -r metis
sudo make uninstall  # If installed via 'make modules_install'
```

Install the generated package:

**Ubuntu/Debian:**
```bash
sudo dpkg -i metis-dkms_<version>_all.deb
```

**RHEL/Fedora:**
```bash
sudo dnf install rpmbuild/RPMS/noarch/metis-dkms-<version>-1.el10.noarch.rpm
```

The module will be automatically built for your current kernel. Load the module:

```bash
sudo modprobe metis
```

To verify the module is loaded:

```bash
lsmod | grep metis
```

### Uninstalling DKMS Package

To remove the DKMS package:

**Ubuntu/Debian:**
```bash
sudo dpkg -r metis-dkms
```

**RHEL/Fedora:**
```bash
sudo dnf remove metis-dkms
```

This automatically unloads the module and removes it from the system.

## Option 2: Manual Build and Installation

### Building

### Build with System Kernel

To build the driver module for your currently running kernel:

```bash
make          # Build the module
make clean    # Clean build artifacts
```

### Build with Custom Kernel

To build against a custom kernel build directory:

```bash
make KDIR=/path/to/kernel/build
```

Where `KDIR` points to the configured kernel source tree or build output directory.

### Installation

First, install udev rules to allow non-root users to access the device:

```bash
sudo sh -c '
cp udev/72-axelera.rules /etc/udev/rules.d/
udevadm control --reload-rules && udevadm trigger
'
```

This only needs to be run once per machine.

Then, install the built module to the system:

```bash
sudo make modules_install
```

This installs the module to `/lib/modules/$(uname -r)/extra/`.

To load the module:

```bash
sudo modprobe metis
```

### Uninstallation

To unload the module:

```bash
sudo modprobe -r metis
```

To remove the installed module:

```bash
sudo make uninstall
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
