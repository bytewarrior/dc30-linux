#!/bin/sh
# Build and install the dc30 modules for the running kernel with DKMS.
# AUTOINSTALL in dkms.conf takes care of kernels installed later.
set -e

NAME=dc30
VERSION=@VERSION@

if ! dkms status -m "$NAME" -v "$VERSION" | grep -q .; then
	dkms add -m "$NAME" -v "$VERSION"
fi

if ! dkms install -m "$NAME" -v "$VERSION"; then
	echo "dc30-dkms: building the modules failed." >&2
	echo "Install the headers of your kernel and run:" >&2
	echo "  sudo dkms install -m $NAME -v $VERSION" >&2
fi

exit 0
