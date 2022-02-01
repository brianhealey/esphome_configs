// Copyright 2022, Charles Powell

#include "esphome/components/json/json_util.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/application.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

#ifdef ARDUINO_ARCH_ESP32
#include "AsyncUDP.h"
#endif
#ifdef ARDUINO_ARCH_ESP8266
#include "ESPAsyncUDP.h"
#endif


#define RES_SIZE 400
#define REQ_SIZE 70
#define MAX_PLUG_COUNT 10  // Somewhat arbitrary as of now

class ESPSensePlug {
 public:
  std::string name;
  std::string mac;
  bool encrypt = true;
  float voltage = 120.0;
  sensor::Sensor *power_sid = NULL;
  sensor::Sensor *voltage_sid = NULL;
  sensor::Sensor *current_sid = NULL;
  sensor::Sensor *status_sensor_sid = NULL;
  switch_::Switch *status_switch_sid = NULL;
  
  std::string base_json = "{\"emeter\": {\"get_realtime\":{ "
                              "\"current\": %.02f, \"voltage\": %.02f, \"power\": %.02f, \"total\": 0, \"err_code\": 0}}, "
                           "\"system\": {\"get_sysinfo\": "
                              "{\"err_code\": 0, \"hw_ver\": 1.5, \"type\": \"IOT.SMARTPLUGSWITCH\", \"model\": \"HS666(US)\", "
                           "\"mac\": \"%s\", \"deviceId\": \"%s\", \"alias\": \"%s\", \"relay_state\": %i, \"updating\": 0 }}}";

  ESPSensePlug() {}

  void set_name(std::string name) { this->name = name; }
  void set_mac_address(std::string mac) { this->mac = mac; }
  void set_encrypt(bool encrypt) { this->encrypt = encrypt; }
  void set_voltage(float voltage) { this->voltage = voltage; }
  void set_power_sensor(sensor::Sensor *sensor) { this->power_sid = sensor; }
  void set_voltage_sensor(sensor::Sensor *sensor) { this->voltage_sid = sensor; }
  void set_current_sensor(sensor::Sensor *sensor) { this->current_sid = sensor; }
  void set_status_sensor(sensor::Sensor *sensor) { this->status_sensor_sid = sensor; }
  void set_status_switch(switch_::Switch *sensor) { this->status_switch_sid = sensor; }

  float get_power() {
    return get_sensor_reading(power_sid, 0.0);
  }
  
  float get_voltage() {
    return get_sensor_reading(voltage_sid, voltage);
  }
  
  float get_current() {
    return get_sensor_reading(current_sid, get_power() / get_voltage());
  }

  boolean get_active() {
    return (status_sensor_sid == NULL && status_switch_sid == NULL) || //default to true
            (status_sensor_sid != NULL && id(status_sensor_sid).state) ||
            (status_switch_sid != NULL && id(status_switch_sid).state);
  }
  
  float get_sensor_reading(sensor::Sensor *sid, float default_value) {
    if(sid != NULL && id(sid).has_state()) {
      return id(sid).state;
    } else {
      return default_value;
    }
  }
  
  int generate_response(char *data) {
    float power = get_power();
    float voltage = get_voltage();
    float current = get_current();
    int active_value = get_active() ? 1 : 0;
    int response_len = snprintf(data, RES_SIZE, base_json.c_str(), current, voltage, power, mac.c_str(), mac.c_str(), name.c_str(), active_value);
    ESP_LOGD("ESPSense", "JSON out: %s", data);
    return response_len;
  }
};

class ESPSense : public Component {
public:
  AsyncUDP udp;
  
  ESPSense() : Component() {}
  
  float get_setup_priority() const override { return esphome::setup_priority::AFTER_WIFI; }
  
  void setup() override {
    if(udp.listen(9999)) {
      ESP_LOGI("ESPSense","Listening on port 9999");
      // Parse incoming packets
      start_sense_response();
    } else {
      ESP_LOGE("ESPSense", "Failed to start UDP listener!");
    }
  }

  void addPlug(ESPSensePlug *plug) {
    if (plugs.size() >= MAX_PLUG_COUNT) {
      ESP_LOGW("ESPSense", "Attempted to add more than %ui plugs, ignoring", MAX_PLUG_COUNT);
    }

    if (plug->mac.empty())
    {
      if (plugs.size() == 0)
      {
        // First plug to be added, and no MAC set, so default to own hardware MAC
        plug->set_mac_address(get_mac_address_pretty());
      } else {
        // Generate a fake MAC address from the name to prevent issues when there are multiple plugs with the same MAC address
        uint32_t name_hash = fnv1_hash(plug->name);
        uint8_t *hash_pointer = (uint8_t *)&name_hash;
        char mac[20];
        sprintf(mac, "%02X:%02X:%02X:%02X:%02X:%02X", 53, 75, hash_pointer[0], hash_pointer[1], hash_pointer[2], hash_pointer[3]);
        plug->set_mac_address(mac);
      }
    }

    plugs.push_back(plug);
  }
  
private:
  float voltage;
  char response_buf[RES_SIZE];
  std::vector<ESPSensePlug *> plugs;

#if ESPHOME_VERSION_CODE >= VERSION_CODE(2022, 1, 0)
  StaticJsonDocument<200> jsonBuffer;
#else
  StaticJsonBuffer<200> jsonBuffer;
#endif
  
  void start_sense_response() {
    ESP_LOGI("ESPSense","Starting ESPSense listener");
    udp.onPacket([&](AsyncUDPPacket &packet) {
      parse_packet(packet);
    });
  }
  
  void parse_packet(AsyncUDPPacket &packet) {
    ESP_LOGD("ESPSense", "Got packet from %s", packet.remoteIP().toString().c_str());
    
    if(packet.length() > REQ_SIZE) {
      // Not a Sense request packet
      ESP_LOGD("ESPSense", "Packet is oversized, ignoring");
      return;
    }
    
    char request_buf[REQ_SIZE];
    
    // Decrypt
    decrypt(packet.data(), packet.length(), request_buf);
    
    // Add null terminator
    request_buf[packet.length()] = '\0';
    
    // Print into null-terminated string
    ESP_LOGD("ESPSense", "Got message: %s", request_buf);
    
    // Parse JSON
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2022, 1, 0)
    // ArduinoJson 6
    auto jsonError = deserializeJson(jsonBuffer, request_buf);
    if(jsonError) {
      ESP_LOGW("ESPSense", "JSON parse failed!");
      return;
    }
    


    // Check if this is a valid request by looking for emeter key
    const char* request = jsonBuffer["emeter"]["get_realtime"];

    if (jsonBuffer.containsKey("emeter") && (jsonBuffer["emeter"]).containsKey("get_realtime")) {


    //if (request) {
#else
    // ArduinoJson 5
    jsonBuffer.clear();
    JsonObject &req = jsonBuffer.parseObject(request_buf);
    if(!req.success()) {
      ESP_LOGW("ESPSense", "JSON parse failed!");
      return;
    }
    JsonVariant request = req["emeter"]["get_realtime"];
    if (request.success()) {
#endif
      ESP_LOGD("ESPSense", "Power measurement requested");
      for (auto *plug : this->plugs) {
        // Generate JSON response string
        int response_len = plug->generate_response(response_buf);
        char response[response_len];
        if (plug->encrypt) {
          // Encrypt
          encrypt(response_buf, response_len, response);
          // Respond to request
          packet.write((uint8_t *)response, response_len);
        } else {
          // Response to request
          packet.write((uint8_t *)response_buf, response_len);
        }
      }
    }
  }
  
  void decrypt(const uint8_t *data, size_t len, char* result) {
    uint8_t key = 171;
    uint8_t a;
    for (int i = 0; i < len; i++) {
      uint8_t unt = data[i];
      a = unt ^ key;
      key = unt;
      result[i] = char(a);
    }
  }
  
  void encrypt(const char *data, size_t len, char* result) {
    uint8_t key = 171;
    uint8_t a;
    for (int i = 0; i < len; i++) {
      uint8_t unt = data[i];
      a = unt ^ key;
      key = a;
      result[i] = a;
    }
  }
};
