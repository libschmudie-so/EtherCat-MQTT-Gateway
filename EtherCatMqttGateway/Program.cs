using System;
using System.Buffers;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.IO;
using System.Collections.Generic;

using CommandLine;
using Microsoft.Extensions.Logging;
using MQTTnet;
using MQTTnet.Protocol;
using Newtonsoft.Json.Linq;

using EtherCAT.NET;
using EtherCAT.NET.Extension;
using EtherCAT.NET.Infrastructure;

namespace EtherCatMqttGateway
{
    /// <summary>
    /// CLI options parsed by CommandLineParser.
    /// </summary>
    public sealed class CliOptions
    {
        /// <summary>Network interface for EtherCAT (e.g., eth0).</summary>
        [Option('i', "iface", Required = true, HelpText = "Network interface for EtherCAT (e.g., eth0).")]
        public string Interface { get; set; } = string.Empty;

        /// <summary>MQTT broker hostname or IP (default: 127.0.0.1).</summary>
        [Option('b', "broker", Required = false, Default = "127.0.0.1", HelpText = "MQTT broker address.")]
        public string Broker { get; set; } = "127.0.0.1";

        /// <summary>MQTT broker port (default: 1883).</summary>
        [Option('p', "port", Required = false, Default = 1883, HelpText = "MQTT broker port.")]
        public int Port { get; set; } = 1883;

        /// <summary>ESI directory (default: %LocalAppData%/ESI).</summary>
        [Option('e', "esi", Required = false, HelpText = "ESI directory path.")]
        public string? EsiDir { get; set; }

        /// <summary>Cycle frequency in Hz (default: 10).</summary>
        [Option('f', "frequency", Required = false, Default = (uint)10, HelpText = "Cycle frequency in Hz.")]
        public uint FrequencyHz { get; set; } = 10;

        /// <summary>Retain process-data MQTT messages (default: false).</summary>
        [Option("retain", Required = false, Default = false, HelpText = "Retain process-data messages.")]
        public bool RetainProcessData { get; set; }

        /// <summary>Verbose logging (Information).</summary>
        [Option('v', "verbose", Required = false, Default = false, HelpText = "Verbose logging (Information).")]
        public bool Verbose { get; set; }

        /// <summary>Quiet logging (Warning).</summary>
        [Option('q', "quiet", Required = false, Default = false, HelpText = "Quiet logging (Warning).")]
        public bool Quiet { get; set; }

        /// <summary>Debug logging.</summary>
        [Option("debug", Required = false, Default = false, HelpText = "Debug logging.")]
        public bool Debug { get; set; }
    }

    /// <summary>
    /// EtherCAT ⇄ MQTT bridge entry point.
    /// </summary>
    internal class Program
    {
        private sealed record Args(
            string Interface,
            string Broker,
            int Port,
            string? EsiDir,
            uint FrequencyHz,
            bool RetainProcessData,
            LogLevel LogLevel);

        private sealed record WriteRequest(ushort Csa, ushort Index, byte SubIndex, JToken Value);

        private static readonly object EcLock = new();
        private static readonly ConcurrentDictionary<string, string> Cache = new(StringComparer.Ordinal);
        private static readonly ConcurrentQueue<WriteRequest> PendingWrites = new();

        private static ILogger Logger = default!;
        private static EcMaster Master = default!;
        private static List<SlaveDevice> SlaveDevices = new();
        private static IMqttClient? MqttClient;
        private static Args Parsed = default!;
        private static readonly string StatusTopic = "ethercat/bridge/status";

        /// <summary>
        /// Application entry point. Parses CLI options, initializes logging, and runs the bridge.
        /// </summary>
        public static async Task<int> Main(string[] rawArgs)
        {
            var exitCode = 0;

            await Parser.Default.ParseArguments<CliOptions>(rawArgs)
                .WithNotParsed(errs =>
                {
                    // CommandLineParser prints help automatically; keep non-zero exit.
                    exitCode = 2;
                })
                .WithParsedAsync(async cli =>
                {
                    Parsed = Map(cli);

                    using var loggerFactory = LoggerFactory.Create(builder =>
                    {
                        builder
                            .SetMinimumLevel(Parsed.LogLevel)
                            .AddSimpleConsole(o =>
                            {
                                o.TimestampFormat = "yyyy-MM-dd HH:mm:ss.fff ";
                                o.SingleLine = true;
                            });
                    });
                    Logger = loggerFactory.CreateLogger("EtherCatMqttGateway");

                    try
                    {
                        exitCode = await RunAsync();
                    } catch (OperationCanceledException)
                    {
                        exitCode = 0;
                    } catch (Exception ex)
                    {
                        Logger.LogCritical(ex, "Fatal error");
                        exitCode = 1;
                    }
                });

            return exitCode;
        }

        /// <summary>
        /// Maps CLI options to an immutable Args record and resolves logging level preference.
        /// </summary>
        private static Args Map(CliOptions cli)
        {
            // Priority: --debug > --quiet > --verbose > default(Information)
            var level = LogLevel.Information;
            if (cli.Verbose) level = LogLevel.Information;
            if (cli.Quiet) level = LogLevel.Warning;
            if (cli.Debug) level = LogLevel.Debug;

            if (cli.Port <= 0 || cli.Port > 65535)
                throw new ArgumentException("Invalid --port");

            if (cli.FrequencyHz == 0 || cli.FrequencyHz > 1000)
                throw new ArgumentException("Invalid --frequency (1..1000 Hz suggested)");

            if (string.IsNullOrWhiteSpace(cli.Interface))
                throw new ArgumentException("--iface is required");

            return new Args(
                Interface: cli.Interface,
                Broker: cli.Broker,
                Port: cli.Port,
                EsiDir: cli.EsiDir,
                FrequencyHz: cli.FrequencyHz,
                RetainProcessData: cli.RetainProcessData,
                LogLevel: level
            );
        }

        /// <summary>
        /// Executes the bridge lifecycle: EtherCAT scan/config, MQTT connect, main loop, and cleanup.
        /// </summary>
        private static async Task<int> RunAsync()
        {
            Logger.LogInformation("Starting EtherCatMqttGateway");
            Logger.LogInformation("Interface={Interface} Broker={Broker}:{Port} ESI={EsiDir} Freq={FreqHz}Hz RetainProcessData={Retain}",
                Parsed.Interface, Parsed.Broker, Parsed.Port, Parsed.EsiDir ?? "<default>", Parsed.FrequencyHz, Parsed.RetainProcessData);

            var esiDirectoryPath = Parsed.EsiDir ?? Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ESI");
            Directory.CreateDirectory(esiDirectoryPath);

            var settings = new EcSettings(cycleFrequency: Parsed.FrequencyHz, esiDirectoryPath, Parsed.Interface);

            // Discover slaves and generate dynamic data.
            var rootSlave = EcUtilities.ScanDevices(settings.InterfaceName);
            var allSlaves = rootSlave.Descendants().ToList();
            foreach (var slave in allSlaves)
                EcUtilities.CreateDynamicData(settings.EsiDirectoryPath, slave);

            Logger.LogInformation("Discovered {Count} slaves", allSlaves.Count);

            Master = new EcMaster(settings);

            Logger.LogInformation("Configuring EtherCAT master (this may take a few seconds)...");

            try
            {
                Master.Configure(rootSlave);
            }
            catch (Exception ex)
            {
                Logger.LogCritical(ex, "Failed to configure EtherCAT master");
                return 1;
            }

            // CSA derived from ring order (1-based).
            SlaveDevices = allSlaves.Select((s, i) => new SlaveDevice(Master, s, (ushort)(i + 1))).ToList();

            var factory = new MqttClientFactory();
            MqttClient = factory.CreateMqttClient();

            var mqttOptions = new MqttClientOptionsBuilder()
                .WithClientId("EtherCATMaster")
                .WithTcpServer(Parsed.Broker, Parsed.Port)
                .WithCleanSession(false)
                .WithWillTopic(StatusTopic)
                .WithWillPayload("offline")
                .WithWillRetain(true)
                .WithWillQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
                .Build();

            // Inbound messages enqueue write requests; actual writes occur under the EC lock in the cycle.
            MqttClient.ApplicationMessageReceivedAsync += OnMqttMessage;

            Logger.LogInformation("Connecting to MQTT broker at {Broker}:{Port}…", Parsed.Broker, Parsed.Port);

            var connected = false;
            for (int attempt = 1; attempt <= 5 && !connected; attempt++)
            {
                try
                {
                    await MqttClient.ConnectAsync(mqttOptions);
                    Logger.LogInformation("MQTT connected on attempt {Attempt}", attempt);
                    await PublishStatusAsync("online", retain: true);
                    connected = true;
                }
                catch (Exception ex)
                {
                    Logger.LogWarning(ex, "MQTT connect attempt {Attempt} failed", attempt);
                    await Task.Delay(TimeSpan.FromSeconds(2));
                }
            }

            if (!connected)
            {
                Logger.LogCritical("Unable to connect to MQTT broker after retries");
                return 1; // bail out instead of crashing
            }

            // Reconnect and re-subscribe on disconnect.
            MqttClient.DisconnectedAsync += async _ =>
            {
                Logger.LogWarning("MQTT disconnected, attempting reconnect…");
                await Task.Delay(TimeSpan.FromSeconds(2));
                try
                {
                    await MqttClient.ConnectAsync(mqttOptions);
                    Logger.LogInformation("MQTT reconnected");
                    await ResubscribeOutputsAsync();
                    await PublishStatusAsync("online", retain: true);
                }
                catch (Exception ex)
                {
                    Logger.LogError(ex, "MQTT reconnect failed");
                }
            };

            await PublishStatusAsync("online", retain: true);

            // Publish static metadata and subscribe to per-slave wildcard output topics.
            foreach (var sd in SlaveDevices)
            {
                var metaTopic = $"ethercat/{sd.GetCsa()}/metadata";
                var metaPayload = sd.GetMetadata().ToString();

                var msg = new MqttApplicationMessageBuilder()
                    .WithTopic(metaTopic)
                    .WithPayload(metaPayload)
                    .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
                    .WithRetainFlag(true)
                    .Build();
                await MqttClient.PublishAsync(msg);

                var outFilter = $"ethercat/{sd.GetCsa()}/+/+";
                await MqttClient.SubscribeAsync(
                    new MqttClientSubscribeOptionsBuilder()
                        .WithTopicFilter(outFilter, MqttQualityOfServiceLevel.AtLeastOnce)
                        .Build());
            }

            // Expose bridge info for observability.
            var info = new JObject
            {
                ["interface"] = Parsed.Interface,
                ["frequency_hz"] = Parsed.FrequencyHz,
                ["esi_path"] = esiDirectoryPath,
                ["started_utc"] = DateTime.UtcNow
            };
            await PublishJsonAsync("ethercat/bridge/info", info, retain: true);

            using var cts = new CancellationTokenSource();
            Console.CancelKeyPress += (_, e) => { e.Cancel = true; cts.Cancel(); };

            var period = TimeSpan.FromMilliseconds(1000.0 / Parsed.FrequencyHz);
            using var timer = new PeriodicTimer(period);

            var worker = Task.Run(async () =>
            {
                var sw = new Stopwatch();
                long overruns = 0;

                while (await timer.WaitForNextTickAsync(cts.Token))
                {
                    sw.Restart();

                    // Snapshot read buffer outside the lock; EtherCAT I/O inside.
                    var snapshot = new List<(string topic, JToken value)>(capacity: 256);

                    lock (EcLock)
                    {
                        // Apply queued writes from MQTT.
                        while (PendingWrites.TryDequeue(out var req))
                        {
                            var sd = SlaveDevices.FirstOrDefault(x => x.GetCsa() == req.Csa);
                            if (sd == null) continue;

                            var varDesc = sd.GetOutputVariables()
                                .FirstOrDefault(v => v.Index == req.Index && v.SubIndex == req.SubIndex);
                            if (varDesc == null) continue;

                            try
                            {
                                sd.WriteVariableAsJToken(varDesc, req.Value);
                            }
                            catch (Exception ex)
                            {
                                Logger.LogWarning(ex, "Failed to write CSA={Csa} {Index:X4}/{Sub:X2}",
                                    req.Csa, req.Index, req.SubIndex);
                            }
                        }

                        Master.UpdateIO(DateTime.UtcNow);

                        foreach (var sd in SlaveDevices)
                        foreach (var v in sd.GetAllVariables())
                        {
                            if (v.DataType <= 0) continue;
                            var topic = $"ethercat/{sd.GetCsa()}/{v.Index:X4}/{v.SubIndex:X2}";
                            var value = sd.ReadVariableAsJToken(v);
                            snapshot.Add((topic, value));
                        }
                    }

                    // Publish MQTT outside the lock.
                    foreach (var (topic, value) in snapshot)
                    {
                        var strVal = value.ToString();

                        if (Cache.TryGetValue(topic, out var old) && old == strVal)
                            continue;

                        Cache[topic] = strVal;

                        var builder = new MqttApplicationMessageBuilder()
                            .WithTopic(topic)
                            .WithPayload(strVal)
                            .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce);

                        if (Parsed.RetainProcessData)
                            builder.WithRetainFlag();

                        try
                        {
                            if (MqttClient != null)
                                await MqttClient.PublishAsync(builder.Build(), cts.Token);
                        }
                        catch (OperationCanceledException)
                        {
                            break; // shutting down
                        }
                        catch (Exception ex)
                        {
                            Logger.LogWarning(ex, "Publish failed for {Topic}", topic);
                        }
                    }

                    sw.Stop();
                    if (sw.Elapsed > period + TimeSpan.FromMilliseconds(2))
                    {
                        overruns++;
                        Logger.LogWarning("Cycle overrun: elapsed={Elapsed}ms > period={Period}ms (count={Count})",
                            sw.Elapsed.TotalMilliseconds.ToString("F2"),
                            period.TotalMilliseconds.ToString("F2"),
                            overruns);
                    }
                }
            }, cts.Token);

            Logger.LogInformation("Press Ctrl+C to exit.");
            await worker;

            try
            {
                await PublishStatusAsync("offline", retain: true);
                if (MqttClient?.IsConnected == true)
                    await MqttClient.DisconnectAsync();
            }
            catch (Exception ex)
            {
                Logger.LogWarning(ex, "Error during MQTT disconnect");
            }

            Master.Dispose();
            Logger.LogInformation("Shutdown complete.");
            return 0;
        }

        /// <summary>
        /// MQTT inbound handler: decodes payload, validates topic, and enqueues write requests.
        /// </summary>
        private static Task OnMqttMessage(MqttApplicationMessageReceivedEventArgs e)
        {
            var topic = e.ApplicationMessage.Topic;

            if (!TryParseValue(e.ApplicationMessage.Payload, out var value))
                return Task.CompletedTask;

            string strVal = value.ToString();

            if (Cache.TryGetValue(topic, out var old) && old == strVal)
                return Task.CompletedTask;

            // Topic shape: ethercat/{CSA}/{IndexHex4}/{SubIdxHex2}
            ushort csa; ushort index; byte sub;
            try
            {
                var parts = topic.Split('/');
                if (parts.Length < 4 || !string.Equals(parts[0], "ethercat", StringComparison.Ordinal))
                    return Task.CompletedTask;

                csa   = Convert.ToUInt16(parts[^3]);
                index = Convert.ToUInt16(parts[^2], 16);
                sub   = Convert.ToByte(parts[^1], 16);
            }
            catch
            {
                return Task.CompletedTask;
            }

            var sd = SlaveDevices.FirstOrDefault(x => x.GetCsa() == csa);
            if (sd == null) return Task.CompletedTask;

            // Validate that this variable is an output before accepting writes.
            var isOutput = sd.GetOutputVariables().Any(v => v.Index == index && v.SubIndex == sub);
            if (!isOutput) return Task.CompletedTask;

            Cache[topic] = strVal;
            PendingWrites.Enqueue(new WriteRequest(csa, index, sub, value));
            return Task.CompletedTask;
        }

        /// <summary>
        /// Tries to parse an MQTT payload to a JSON token; falls back to a string value if not JSON.
        /// </summary>
        private static bool TryParseValue(ReadOnlySequence<byte> payload, out JToken value)
        {
            var bytes = payload.ToArray();
            var s = Encoding.UTF8.GetString(bytes);
            try
            {
                value = JToken.Parse(s);
                return true;
            }
            catch
            {
                value = new JValue(s);
                return true;
            }
        }

        /// <summary>
        /// Publishes an online/offline status string to the status topic.
        /// </summary>
        private static async Task PublishStatusAsync(string state, bool retain)
        {
            if (MqttClient == null) return;

            var msg = new MqttApplicationMessageBuilder()
                .WithTopic(StatusTopic)
                .WithPayload(state)
                .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
                .WithRetainFlag(retain)
                .Build();

            await MqttClient.PublishAsync(msg);
        }

        /// <summary>
        /// Publishes a JSON token to a topic with optional retain flag.
        /// </summary>
        private static async Task PublishJsonAsync(string topic, JToken json, bool retain)
        {
            if (MqttClient == null) return;

            var msg = new MqttApplicationMessageBuilder()
                .WithTopic(topic)
                .WithPayload(json.ToString())
                .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
                .WithRetainFlag(retain)
                .Build();

            await MqttClient.PublishAsync(msg);
        }

        /// <summary>
        /// Re-subscribes to all output wildcard topics after reconnect.
        /// </summary>
        private static async Task ResubscribeOutputsAsync()
        {
            if (MqttClient == null) return;

            foreach (var sd in SlaveDevices)
            {
                var outFilter = $"ethercat/{sd.GetCsa()}/+/+";
                await MqttClient.SubscribeAsync(
                    new MqttClientSubscribeOptionsBuilder()
                        .WithTopicFilter(outFilter, MqttQualityOfServiceLevel.AtLeastOnce)
                        .Build());
                Logger.LogInformation("Re-subscribed outputs: {Topic}", outFilter);
            }
        }
    }
}
