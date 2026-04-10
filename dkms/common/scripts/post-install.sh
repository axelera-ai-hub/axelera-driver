#!/bin/sh
# Common post-installation script for metis-dkms
# Called by both Debian postinst and RPM %post

set -e

# Reload udev rules
reload_udev_rules() {
    if command -v udevadm >/dev/null 2>&1; then
        udevadm control --reload-rules && udevadm trigger && udevadm settle || \
            echo "WARNING: Failed to reload udev rules." >&2
    fi
}

# Ensure axelera group exists
ensure_axelera_group() {
    groupname=axelera
    if getent group "${groupname}" > /dev/null 2>&1; then
        echo "Group ${groupname} already exists"
    else
        groupadd "${groupname}"
        echo "Group ${groupname} created successfully"
    fi
}

reload_udev_rules
ensure_axelera_group

exit 0
