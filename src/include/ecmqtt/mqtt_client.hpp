#pragma once

#include <chrono>
#include <functional>
#include <string>

struct mosquitto;
struct mosquitto_message;

namespace ecmqtt {

// RAII wrapper around libmosquitto's C API. Runs the network loop on a
// background thread (mosquitto_loop_start); publish/subscribe are safe to
// call from any thread per libmosquitto's documented thread-safety.
//
// onConnect fires on every (re)connect (including automatic reconnects
// handled internally by libmosquitto), which is used by the caller to
// re-publish "online" and re-subscribe output topics -- replacing the
// manual reconnect/resubscribe logic the .NET original needed.
class MqttClient {
public:
    using MessageHandler = std::function<void(const std::string& topic, const std::string& payload)>;
    using ConnectHandler = std::function<void()>;

    MqttClient(const std::string& clientId, const std::string& willTopic, const std::string& willPayload);
    ~MqttClient();

    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    void setOnMessage(MessageHandler handler);
    void setOnConnect(ConnectHandler handler);

    // Blocking connect with up to maxAttempts tries, sleeping retryDelay
    // between failures. Starts the background loop thread on success.
    bool connect(const std::string& host, int port, int maxAttempts, std::chrono::milliseconds retryDelay);

    void publish(const std::string& topic, const std::string& payload, int qos = 1, bool retain = false);
    void subscribe(const std::string& filter, int qos = 1);
    void unsubscribe(const std::string& filter);

    bool isConnected() const { return connected_; }

    void disconnect();

private:
    static void onMessageThunk(mosquitto*, void* userdata, const mosquitto_message* msg);
    static void onConnectThunk(mosquitto*, void* userdata, int rc);

    mosquitto* mosq_ = nullptr;
    bool connected_ = false;
    MessageHandler onMessage_;
    ConnectHandler onConnect_;
};

} // namespace ecmqtt
