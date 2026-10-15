#!/bin/sh
# Run idf.py inside the official ESP-IDF image with Docker or Podman, nothing is installed on the host.
#   ./idf.sh build
#   ./idf.sh -p /dev/ttyACM0 flash monitor
IMAGE=${IDF_IMAGE:-docker.io/espressif/idf:release-v5.5}
ENGINE=${IDF_ENGINE:-$(command -v docker || command -v podman)}
cd "$(dirname "$0")" || exit 1
[ -n "$ENGINE" ] || { echo "idf.sh: docker or podman is required" >&2; exit 1; }

# Rootless Podman needs keep-id to map the host user and keep-groups to pass dialout through to the serial port.
OPTS="-u $(id -u):$(id -g)"
case $ENGINE in
*podman) OPTS="$OPTS --userns=keep-id --group-add keep-groups";;
esac

DEV=
for p in /dev/ttyACM* /dev/ttyUSB*; do
	[ -e "$p" ] || continue
	DEV="$DEV --device $p"
	case $ENGINE in
	*podman) ;;
	*) DEV="$DEV --group-add $(stat -c %g "$p")";;
	esac
done
TTY=
[ -t 0 ] && TTY=-it

exec "$ENGINE" run --rm $TTY $DEV $OPTS \
	-e HOME=/tmp \
	--tmpfs /opt/esp/root_managed_components:mode=1777 \
	-v "$PWD":/project -w /project \
	"$IMAGE" idf.py "$@"
