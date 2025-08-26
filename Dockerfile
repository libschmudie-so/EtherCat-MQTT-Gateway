# Build stage
FROM mcr.microsoft.com/dotnet/sdk:8.0 AS build
WORKDIR /src

# copy csproj and restore as distinct layers
COPY EtherCatMqttGateway/*.csproj ./EtherCatMqttGateway/
RUN dotnet restore EtherCatMqttGateway/*.csproj

# copy everything and build
COPY . .
WORKDIR /src/EtherCatMqttGateway
RUN dotnet publish -c Release -o /app

# Runtime stage
FROM mcr.microsoft.com/dotnet/runtime:8.0 AS runtime
WORKDIR /app

# EtherCAT master requires access to raw network devices
# Add libpcap (needed by EtherCAT.NET in most cases)
RUN apt-get update && apt-get install -y --no-install-recommends \
    libpcap0.8 \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /app ./
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
