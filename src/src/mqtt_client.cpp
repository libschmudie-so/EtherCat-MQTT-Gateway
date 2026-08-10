#include "ecmqtt/mqtt_client.hpp"

#include <mosquitto.h>

#include <spdlog/spdlog.h>
#include <stdexcept>
#include <thread>

namespace ecmqtt {

MqttClient::MqttClient(const std::string& clientId, const std::string& willTopic, const std::string& willPayload) {
    mosquitto_lib_init();

    mosq_ = mosquitto_new(clientId.c_str(), /*clean_session=*/false, this);
    if (!mosq_) {
        mosquitto_lib_cleanup();
        throw std::runtime_error("mosquitto_new failed");
    }

    mosquitto_will_set(mosq_, willTopic.c_str(), static_cast<int>(willPayload.size()), willPayload.data(),
                        /*qos=*/1, /*retain=*/true);

    // Exponential backoff up to 30s between reconnect attempts, handled
    // internally by libmosquitto once the network loop thread is running.
    mosquitto_reconnect_delay_set(mosq_, 2, 30, true);

    mosquitto_message_callback_set(mosq_, &MqttClient::onMessageThunk);
    mosquitto_connect_callback_set(mosq_, &MqttClient::onConnectThunk);
}

MqttClient::~MqttClient() {
    if (mosq_) {
        mosquitto_loop_stop(mosq_, true);
        mosquitto_destroy(mosq_);
    }
    mosquitto_lib_cleanup();
}

void MqttClient::setOnMessage(MessageHandler handler) { onMessage_ = std::move(handler); }
void MqttClient::setOnConnect(ConnectHandler handler) { onConnect_ = std::move(handler); }

bool MqttClient::connect(const std::string& host, int port, int maxAttempts, std::chrono::milliseconds retryDelay) {
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        int rc = mosquitto_connect(mosq_, host.c_str(), port, /*keepalive=*/60);
        if (rc == MOSQ_ERR_SUCCESS) {
            connected_ = true;
            mosquitto_loop_start(mosq_);
            return true;
        }
        spdlog::warn("MQTT connect attempt {} failed: {}", attempt, mosquitto_strerror(rc));
        std::this_thread::sleep_for(retryDelay);
    }
    return false;
}

void MqttClient::publish(const std::string& topic, const std::string& payload, int qos, bool retain) {
    int rc = mosquitto_publish(mosq_, nullptr, topic.c_str(), static_cast<int>(payload.size()), payload.data(),
                                qos, retain);
    if (rc != MOSQ_ERR_SUCCESS)
        spdlog::warn("Publish failed for {}: {}", topic, mosquitto_strerror(rc));
}

void MqttClient::subscribe(const std::string& filter, int qos) {
    int rc = mosquitto_subscribe(mosq_, nullptr, filter.c_str(), qos);
    if (rc != MOSQ_ERR_SUCCESS)
        spdlog::warn("Subscribe failed for {}: {}", filter, mosquitto_strerror(rc));
}

void MqttClient::disconnect() {
    connected_ = false;
    mosquitto_disconnect(mosq_);
}

void MqttClient::onMessageThunk(mosquitto*, void* userdata, const mosquitto_message* msg) {
    auto* self = static_cast<MqttClient*>(userdata);
    if (!self->onMessage_ || !msg || !msg->topic) return;
    std::string payload(static_cast<const char*>(msg->payload), msg->payloadlen);
    self->onMessage_(msg->topic, payload);
}

void MqttClient::onConnectThunk(mosquitto*, void* userdata, int rc) {
    auto* self = static_cast<MqttClient*>(userdata);
    if (rc != 0) {
        spdlog::warn("MQTT connect callback reported failure: {}", mosquitto_connack_string(rc));
        return;
    }
    self->connected_ = true;
    if (self->onConnect_) self->onConnect_();
}

} // namespace ecmqtt
