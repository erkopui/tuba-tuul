#!/bin/sh
# Run idf.py inside the official ESP-IDF Docker image, nothing is installed on the host.
#   ./idf.sh build
#   ./idf.sh -p /dev/ttyACM0 flash monitor
IMAGE=${IDF_IMAGE:-espressif/idf:release-v5.5}
cd "$(dirname "$0")" || exit 1

DEV=
for p in /dev/ttyACM* /dev/ttyUSB*; do
	[ -e "$p" ] && DEV="$DEV --device $p --group-add $(stat -c %g "$p")"
done
TTY=
[ -t 0 ] && TTY=-it

exec docker run --rm $TTY $DEV \
	-u "$(id -u):$(id -g)" -e HOME=/tmp \
	--tmpfs /opt/esp/root_managed_components:mode=1777 \
	-v "$PWD":/project -w /project \
	"$IMAGE" idf.py "$@"
