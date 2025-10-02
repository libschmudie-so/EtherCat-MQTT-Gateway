#!/bin/sh
set -e

# Default values
: "${IFACE:=eth0}"
: "${BROKER:=127.0.0.1}"
: "${PORT:=1883}"
: "${FREQ:=10}"
: "${ESI_DIR:=/data/esi}"
: "${RETAIN:=false}"
: "${NO_OUTPUT:=false}"
: "${CLIENT_ID:=EtherCATMaster}"
: "${TOPIC:=ethercat}"
: "${USE_REPORTED_CSA:=false}"
: "${LOGLEVEL:=info}"

# Ensure ESI directory exists (mounted volume recommended)
mkdir -p "$ESI_DIR"

# Map LOGLEVEL env → CLI switches
LOG_OPTS=""
case "$LOGLEVEL" in
  debug)   LOG_OPTS="--debug" ;;
  quiet)   LOG_OPTS="--quiet" ;;
  verbose) LOG_OPTS="--verbose" ;;
esac

# Map retain
RETAIN_OPT=""
[ "$RETAIN" = "true" ] && RETAIN_OPT="--retain"

# Map no-output
NO_OUTPUT_OPT=""
[ "$NO_OUTPUT" = "true" ] && NO_OUTPUT_OPT="--no-output"

# Map use-reported-csa
CSA_OPT=""
[ "$USE_REPORTED_CSA" = "true" ] && CSA_OPT="--use-reported-csa"

exec dotnet EtherCatMqttGateway.dll \
  --iface "$IFACE" \
  --broker "$BROKER" \
  --port "$PORT" \
  --frequency "$FREQ" \
  --esi "$ESI_DIR" \
  --client-id "$CLIENT_ID" \
  --topic "$TOPIC" \
  $RETAIN_OPT \
  $NO_OUTPUT_OPT \
  $CSA_OPT \
  $LOG_OPTS \
  "$@"
