# Build stage
FROM debian:trixie-slim AS build
WORKDIR /src

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++ git ca-certificates pkg-config \
    nlohmann-json3-dev libspdlog-dev libcxxopts-dev libtinyxml2-dev libmosquitto-dev \
 && rm -rf /var/lib/apt/lists/*

COPY src/ ./src/
RUN cmake -B build -S src -DCMAKE_BUILD_TYPE=Release -DEC_BACKEND=SOEM \
 && cmake --build build -j"$(nproc)"

# Runtime stage
FROM debian:trixie-slim AS runtime
WORKDIR /app

# EtherCAT (SOEM) needs raw socket access; the MQTT/JSON/XML/logging libs
# below are the runtime counterparts of the -dev packages used to build.
RUN apt-get update && apt-get install -y --no-install-recommends \
    libmosquitto1 libtinyxml2-11 libspdlog1.15 \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/ethercat-mqtt-gateway ./
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
