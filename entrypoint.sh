#!/bin/sh
set -e

# Default values
: "${IFACE:=eth0}"
: "${BROKER:=127.0.0.1}"
: "${PORT:=1883}"
: "${FREQ:=10}"
: "${ESI_DIR:=/data/esi}"
: "${RETAIN:=false}"
: "${LOGLEVEL:=info}"

# Ensure ESI directory exists (mounted volume recommended)
mkdir -p "$ESI_DIR"

# Map LOGLEVEL env → CLI switches
LOG_OPTS=""
case "$LOGLEVEL" in
  debug) LOG_OPTS="--debug" ;;
  quiet) LOG_OPTS="--quiet" ;;
  verbose) LOG_OPTS="--verbose" ;;
esac

# Map retain
RETAIN_OPT=""
[ "$RETAIN" = "true" ] && RETAIN_OPT="--retain"

exec dotnet EtherCatMqttGateway.dll \
  --iface "$IFACE" \
  --broker "$BROKER" \
  --port "$PORT" \
  --frequency "$FREQ" \
  --esi "$ESI_DIR" \
  $RETAIN_OPT \
  $LOG_OPTS \
  "$@"
