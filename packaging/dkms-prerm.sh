#!/bin/sh
# Remove the dc30 modules from all kernels before the sources go.
NAME=dc30
VERSION=@VERSION@

if dkms status -m "$NAME" -v "$VERSION" | grep -q .; then
	dkms remove -m "$NAME" -v "$VERSION" --all || true
fi

exit 0
