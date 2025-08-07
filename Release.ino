#include <Arduino.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <GyverNTP.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>
#include <UniversalTelegramBot.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>

#include "CG_RadSens.h"
#include "Credentials.h"

#define rxPin 4
#define txPin 2
#define BAUD_RATE 9600

SoftwareSerial SIM800(15, 16);                // G16 G17 UART

CG_RadSens radSens(RS_DEFAULT_I2C_ADDRESS);   // G21 G22       

GyverNTP ntp(3, 3600, "ntp.msk-ix.ru");

class Notifier {
private:
  bool first_boot_;
  bool daytime_notification_done_;
  bool nighttime_notification_done_;
  int event_counter_;
  uint64_t event_start_timer_;
  const int PERIOD_OF_NOTIFICATIONS_PER_FIRST_HOUR_MINS_ = 5;
  const int PERIOD_OF_NOTIFICATIONS_PER_SECOND_THIRD_HOURS_MINS_ = 15;
  const int PERIOD_OF_NOTIFICATIONS_PER_DAY_MINS_ = 30;
  const int DAYTIME_NOTIFICATION_ = 8;
  const int NIGHTTIME_NOTIFICATION_ = 20;
public:
  Notifier() {
    first_boot_ = 1;
    event_counter_ = 0;
    event_start_timer_ = 0;
  }

  void EventOccured() {
    ++event_counter_;
  }

  void EepromInitialize() {
      if (first_boot_) {
        EEPROM.get(0, daytime_notification_done_);
        EEPROM.get(1, nighttime_notification_done_);
      first_boot_ = 0;
    }
  }

  void SetNotificationDone(int hour) {
    Serial.println("Before statuses: day/night: " + String(GetDayDoneStatus())
                  + "/" + String(GetNightDoneStatus()));
    if (hour == DAYTIME_NOTIFICATION_) {
      daytime_notification_done_ = 1;
      nighttime_notification_done_ = 0;
    }
    else if (hour == NIGHTTIME_NOTIFICATION_) {
      nighttime_notification_done_ = 1;
      daytime_notification_done_ = 0;
    }
    EEPROM.put(0, daytime_notification_done_);
    EEPROM.put(1, nighttime_notification_done_);
    EEPROM.commit();
    Serial.println("After statuses: day/night: " + String(GetDayDoneStatus())
                  + "/" + String(GetNightDoneStatus()));
  }

  bool GetDayDoneStatus() const {
    return daytime_notification_done_;
  }

  bool GetNightDoneStatus() const {
    return nighttime_notification_done_;
  }

  int GetDayNotificationTime() const {
    return DAYTIME_NOTIFICATION_;
  }

  int GetNightNotificationTime() const {
    return NIGHTTIME_NOTIFICATION_;
  }

};

Notifier notifier;

struct Flags {
  bool radiation = 1;
  bool balance = 1;
  bool is_first_notification = 1;
};

Flags indicator;

const String TEXT_USSD_BALANCE_QUERY = "AT+CUSD=1,\"*100#\"";  // Отправляет ТЕКСТОВЫЙ ответ, а не PDU-пакет, альтернативный запрос - "#100#"
const String BALANCE_TEXT_QUERY = "Balance";                   // Содержание СМС для получения текстового ответа на запрос о балансе
const String TEMPERATURE_QUERY = "Temperature";                // Содержание СМС для получения ответа на запрос о температуре
const String CURRENCY = " RUB";                                // Текстовое представление валюты
const float BALANCE_TRESHOLD = 15.0;                           // Нижнее пороговое значение температуры
const float RADIATION_TRESHOLD = 30.0;                         // Нижнее пороговое значение баланса
constexpr int64_t BALANCE_CHECK_PERIOD = 12 * 60 * 60 * 1000;

float GetFloatFromString(String str) {
  bool flag = false;
  String result = "";
  str.replace(",", ".");
  for (size_t i = 0; i < str.length(); ++i) {
    if (isDigit(str[i]) || (str[i] == (char)46 && flag)) {
      if (i != 0 && result == "" && (String)str[i - 1] == "-") {
        result += "-";
      }
      result += str[i];
      if (!flag) {
        flag = true;
      }
    } else if (str[i] != (char)32 && flag) {
      break;
    }
  }
  return result.toFloat();
}

String WaitAndReturnResponse() {
  String response = "";
  int64_t timeout = millis() + 10000;
  while (!SIM800.available() && millis() < timeout) {

  };
  if (SIM800.available()) {
    response = SIM800.readString();
  }
  else {
    Serial.println("Timeout...");
  }
  return response;
}

String SendATCommand(String cmd, bool waiting, bool new_line = false) {
  String response = "";
  Serial.println(cmd);
  if (new_line) {
    SIM800.println(cmd);
  }
  else {
    SIM800.print(cmd);
  }
  if (waiting) {
    response = WaitAndReturnResponse();
    if (response.startsWith(cmd)) {
      response = response.substring(response.indexOf("\r", cmd.length()) + 2);
    }
    Serial.println(response);
  }
  return response;
}

void SendSMS(const String& phone, const String& message) {
  SendATCommand("AT+CMGS=\"" + phone + "\"", true, true);
  SendATCommand(message + (String)((char)26), true, true);
  delay(2000);
}

void NotifySubscribers(int radiation_intensity, const String* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    SendSMS(data[i], "Radiation: " + String(radiation_intensity));
  }
}

void SendTelegramMessage(const String& message) {
  HTTPClient http;

  String url = "https://api.telegram.org/bot" + BOT_TOKEN + "/sendMessage";
  String payload = "chat_id=" + CHAT_ID + "&text=" + message;

  http.begin(std::move(url));
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int httpCode = http.POST(std::move(payload));

  if (httpCode > 0) {
    String response = http.getString();
    Serial.println("HTTP Response: " + response);
  } else {
    Serial.println("HTTP Error: " + httpCode);
  }

  http.end();
}

void RadiationCheck(float radiation_intensity_static, float radiation_intensity_dynamic) {
  Serial.println("-------------");
  Serial.println(radiation_intensity_static);
  Serial.println(radiation_intensity_dynamic);
  if ((radiation_intensity_static >= RADIATION_TRESHOLD
    || radiation_intensity_dynamic >= RADIATION_TRESHOLD)
    && indicator.radiation) {
    int radiation_intensity = static_cast<int>(max(radiation_intensity_static, radiation_intensity_dynamic));
    SendTelegramMessage("ВНИМАНИЕ! Текущий уровень радиации " +
      String(radiation_intensity) +
      " мкР!");
    SendSMS(MAIN_PHONE, "Radiation: " + String(radiation_intensity));
    NotifySubscribers(radiation_intensity, PHONES, 3);
    indicator.radiation = 0;
  } else if ((radiation_intensity_static < RADIATION_TRESHOLD ||
    radiation_intensity_dynamic < RADIATION_TRESHOLD) &&
    !indicator.radiation) {
    indicator.radiation = 1;
  }
}

float ExtractBalanceFromString(const String& response) {
  String msgBalance = response.substring(response.indexOf("\"") + 2);
  msgBalance = msgBalance.substring(0, msgBalance.indexOf("\""));
  return GetFloatFromString(std::move(msgBalance));
}

void BalanceChecking() {
  SendATCommand(TEXT_USSD_BALANCE_QUERY, true);
  String answer = WaitAndReturnResponse();
  answer.trim();
  float balance = ExtractBalanceFromString(answer);
  Serial.println("Balance is " + String(balance));
  if (balance < BALANCE_TRESHOLD && indicator.balance) {
    SendSMS(MAIN_PHONE, "Balance is below " + String(BALANCE_TRESHOLD) + CURRENCY + " and equal to " + String(balance) + CURRENCY + ".");
    indicator.balance = 0;
  } else if (balance >= BALANCE_TRESHOLD && !indicator.balance) {
    indicator.balance = 1;
  }
}

void CheckInternerConnection(void* pvParameters) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);

  while (1) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Reconnecting to WiFi...");
      WiFi.disconnect();
      WiFi.reconnect();   
    }
    else {
      Serial.println("WiFi is okay");
      ntp.updateNow();
    }
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
  vTaskDelete(NULL);
}

void RadiationCheck(void* pvParameters) {
  while (1) {
    RadiationCheck(radSens.getRadIntensyDynamic(), radSens.getRadIntensyStatic());
    Serial.println("Checked");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  vTaskDelete(NULL);
}

void BalanceCheck(void* pvParameters) {
  while (1) {
    BalanceChecking();
    vTaskDelay(pdMS_TO_TICKS(43200000));
  }
  vTaskDelete(NULL);
}

void MqttHandler(void* pvParameters) {
  IPAddress server(192, 168, 1, 71);
  WiFiClient client;
  PubSubClient mqtt(client);
  mqtt.setServer(server, 1883);

  constexpr size_t BUFFER_SIZE = 7;                             // 1 char for the sign, 1 char for the decimal dot, 4 chars for the value & 1 char for null termination
  char buffer_dynamic[BUFFER_SIZE];
  char buffer_static[BUFFER_SIZE];

  while (1) {
    while (!mqtt.connected()) {
      if (mqtt.connect("radiation_sensor", MQTT_LOGIN.c_str(), MQTT_PASSWORD.c_str())) {
        mqtt.publish("radiation_sensor/radiation_value", String("{ \"dynamic_radiation\": " + String(radSens.getRadIntensyDynamic())
                     + ", \"static_radiation\": " + String(radSens.getRadIntensyStatic()) + " }").c_str());
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10000));
  }
  vTaskDelete(NULL);
}

void Ntp(void* pvParameters) {
  vTaskDelay(pdMS_TO_TICKS(3000));

  ntp.begin();
  ntp.updateNow();

  uint8_t previous_hour = 0;

  while (1) {
    ntp.tick();
    if (ntp.hour() == notifier.GetDayNotificationTime() && !notifier.GetDayDoneStatus()
      || ntp.hour() == notifier.GetNightNotificationTime() && !notifier.GetNightDoneStatus()) {
      SendTelegramMessage("Штатное оповещение: текущий уровень радиации " +
        String(static_cast<int>(radSens.getRadIntensyStatic())) +
        " мкР");
      notifier.SetNotificationDone(ntp.hour());
    }

    Serial.println("Current hour: " + String(ntp.hour()));
    Serial.println(ntp.timeString());
    Serial.println("Current statuses: day/night: " + String(notifier.GetDayDoneStatus())
                  + "/" + String(notifier.GetNightDoneStatus()));

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  vTaskDelete(NULL);
}



void setup() {
  EEPROM.begin(1024);

  Wire.begin();

  SIM800.begin(9600);
  Serial.begin(9600);

  notifier.EepromInitialize();

  Serial.println("Start!");

  while (!radSens.init()) {
    Serial.println("Sensor wiring error!");
    delay(1000);
  }


  if (SIM800.available()) {
    Serial.println(SIM800.readString());
  }

  xTaskCreatePinnedToCore(
    CheckInternerConnection,
    "InternetConnection",
    4096,
    NULL,
    1,
    NULL,
    0
  );

  xTaskCreatePinnedToCore(
    MqttHandler,
    "MqttPublisher",
    4096,
    NULL,
    1,
    NULL,
    0
  );

  xTaskCreatePinnedToCore(
    RadiationCheck,
    "RadiationCheck",
    4096,
    NULL,
    1,
    NULL,
    0
  );

  xTaskCreatePinnedToCore(
    BalanceCheck,
    "BalanceCheck",
    4096,
    NULL,
    1,
    NULL,
    0
  );

  xTaskCreatePinnedToCore(
    Ntp,
    "ntp",
    4096,
    NULL,
    1,
    NULL,
    0
  );
}

void loop()
{

}