#!/bin/sh
# Common pre-removal script for metis-dkms
# Called by both Debian prerm and RPM %preun

set -e

# Check if axsystemserver is running
check_axsystemserver() {
    if [ -d /run/systemd/system ]; then
        # We are in a systemd enabled machine
        if systemctl is-active --quiet axsystemserver.service; then
            echo "axsystemserver service is running. Please stop it before attempting to remove \`metis-dkms\`." >&2
            exit 1
        fi
        # axsystemserver is not started. We can safely remove the driver
    fi
    # We are not in a systemd enabled machine, so we can safely assume that axsystemserver can't possibly run
}

check_axsystemserver

exit 0
