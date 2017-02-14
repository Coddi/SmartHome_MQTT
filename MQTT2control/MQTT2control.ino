//#define serDeb //Start Serial

#include <FS.h>                   //this needs to be first, or it all crashes and burns...
#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino
#include <PubSubClient.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <OneWire.h>
#include <DallasTemperature.h>
#include <IRremoteESP8266.h>
#include <ArduinoJson.h>

/* ==========Map of pin==========
 * GPIO 13 --> ONE WIRE (DS18B20)
 * GPIO 4  --> Relay(1)
 * GPIO 5  --> Relay(2)
 * GPIO 14 --> IRsend
 * GPIO 12 --> Motion Sensor
 */

#define Relay1 5
#define Relay2 4
#define ONE_WIRE_BUS 13
#define IR_pin 14
#define Motion_Sensor 12

char mqtt_server[40];
char mqtt_port[6] = "8080";
bool shouldSaveConfig = false;
bool shouldSendInfo = TRUE;

unsigned long nowMillisTemp = 0;

String dataIR;
IRsend irsend(IR_pin);

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
DeviceAddress insideThermometer;

uint8_t state = 0;

WiFiClient espClient;
PubSubClient clientMQTT(espClient);

void saveConfigCallback() {
#ifdef serDeb
	Serial.println("Should save config");
#endif
	shouldSaveConfig = true;
}

void sendTemp() {
	clientMQTT.publish("/roomSensor/temp", String(sensors.getTempC(insideThermometer)).c_str());
	sensors.requestTemperatures();
}

void callback(char* topic, byte* payload, unsigned int length) {
#ifdef serDeb
	Serial.println("New callback of MQTT-broker");
#endif
	payload[length] = '\0';
	String strTopic = String(topic);
	String strPayload = String((char*)payload);
#ifdef serDeb
	Serial.println(strTopic);
	Serial.println(strPayload);
#endif
	if (strTopic == "/roomSensor/gpio/5") {
		if (strPayload == "off" || strPayload == "0" || strPayload == "false") digitalWrite(Relay1, LOW);
		if (strPayload == "on" || strPayload == "1" || strPayload == "true") digitalWrite(Relay1, HIGH);
		yield();
#ifdef serDeb
		Serial.println("GPIO5 callback");
#endif
		return;
	}
	if (strTopic == "/roomSensor/gpio/4") {
		if (strPayload == "off" || strPayload == "0" || strPayload == "false") digitalWrite(Relay2, LOW);
		if (strPayload == "on" || strPayload == "1" || strPayload == "true") digitalWrite(Relay2, HIGH);
		yield();
#ifdef serDeb
		Serial.println("GPIO4 callback");
#endif
		return;
	}
	if ((strTopic == "/roomSensor/ir") && length > 4) {
		dataIR = strPayload;
#ifdef serDeb
		Serial.println("IR callback");
#endif
		return;
	}
}

void sendIR() {
	DynamicJsonBuffer jBuf;
	JsonObject& root = jBuf.parseObject(dataIR);
	if (!root.success()) {
#ifdef serDeb
		Serial.println("parseObject() failed");
#endif
		dataIR = "";
		return;
	}
	unsigned long data = root["data"];
	int nbits = root["nbits"];
#ifdef serDeb
	Serial.println("NEC");
	Serial.println(data);
	Serial.println(nbits);
#endif
	irsend.sendNEC(data, nbits);
	dataIR = "";
}

void sendReson() {
	clientMQTT.publish("/roomSensor/ResetInfo", ESP.getResetInfo().c_str());
#ifdef serDeb
	Serial.println("Send reset info");
	Serial.println();
#endif
}


void setup() {
#ifdef serDeb
	Serial.begin(115200);
	Serial.println();
	Serial.println("mounting FS...");
#endif
	//read configuration from FS json
	if (SPIFFS.begin()) {
#ifdef serDeb
		Serial.println("mounted file system");
#endif
		if (SPIFFS.exists("/config.json")) {
			//file exists, reading and loading
#ifdef serDeb
			Serial.println("reading config file");
#endif
			File configFile = SPIFFS.open("/config.json", "r");
			if (configFile) {
#ifdef serDeb
				Serial.println("opened config file");
#endif
				size_t size = configFile.size();
				// Allocate a buffer to store contents of the file.
				std::unique_ptr<char[]> buf(new char[size]);

				configFile.readBytes(buf.get(), size);
				DynamicJsonBuffer jsonBuffer;
				JsonObject& json = jsonBuffer.parseObject(buf.get());
#ifdef serDeb
				json.printTo(Serial);
#endif
				if (json.success()) {
#ifdef serDeb
					Serial.println("\nparsed json");
#endif
					strcpy(mqtt_server, json["mqtt_server"]);
					strcpy(mqtt_port, json["mqtt_port"]);

				}
				else {
#ifdef serDeb
					Serial.println("failed to load json config");
#endif
				}
			}
		}
	}
	else {
#ifdef serDeb
		Serial.println("failed to mount FS");
#endif
	}
	//end read

	// The extra parameters to be configured (can be either global or just in the setup)
	WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
	WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 6);

	//WiFiManager
	//Local intialization. Once its business is done, there is no need to keep it around
	WiFiManager wifiManager;

	//set config save notify callback
	wifiManager.setSaveConfigCallback(saveConfigCallback);

	wifiManager.addParameter(&custom_mqtt_server);
	wifiManager.addParameter(&custom_mqtt_port);

	wifiManager.setTimeout(120);

	//fetches ssid and pass and tries to connect
	//if it does not connect it starts an access point with the specified name
	//here  "AutoConnectAP"
	//and goes into a blocking loop awaiting configuration
	if (!wifiManager.autoConnect("roomSensor", "password")) {
#ifdef serDeb
		Serial.println("failed to connect and hit timeout");
#endif
		delay(3000);
		//reset and try again, or maybe put it to deep sleep
		ESP.reset();
		delay(5000);
	}

	//if you get here you have connected to the WiFi
#ifdef serDeb
	Serial.println("connected...yeey :)");
#endif
	//read updated parameters
	strcpy(mqtt_server, custom_mqtt_server.getValue());
	strcpy(mqtt_port, custom_mqtt_port.getValue());

	//save the custom parameters to FS
	if (shouldSaveConfig) {
#ifdef serDeb
		Serial.println("saving config");
#endif
		DynamicJsonBuffer jsonBuffer;
		JsonObject& json = jsonBuffer.createObject();
		json["mqtt_server"] = mqtt_server;
		json["mqtt_port"] = mqtt_port;

		File configFile = SPIFFS.open("/config.json", "w");
		if (!configFile) {
#ifdef serDeb
			Serial.println("failed to open config file for writing");
#endif
		}
#ifdef serDeb
		json.printTo(Serial);
#endif
		json.printTo(configFile);
		configFile.close();
		//end save
	}
#ifdef serDeb
	Serial.println("local ip");
	Serial.println(WiFi.localIP());
#endif
	clientMQTT.setServer(mqtt_server, atoi(mqtt_port));
	clientMQTT.setCallback(callback);
	sensors.begin();
#ifdef serDeb
	Serial.print(sensors.getDeviceCount(), DEC);
#endif
	sensors.getAddress(insideThermometer, 0);
	sensors.setResolution(insideThermometer, 10);
	sensors.requestTemperatures();
	pinMode(12, INPUT);
	pinMode(4, OUTPUT);
	pinMode(5, OUTPUT);
	irsend.begin();
}

void loop() {
	if (dataIR == "null")dataIR = "";
	if (dataIR != "") sendIR();
	if (!clientMQTT.connected()) {
		if (clientMQTT.connect("roomSensor")) {
#ifdef serDeb
			Serial.println("Connected to MQTT server");
#endif
			clientMQTT.subscribe("/roomSensor/gpio/4");
			clientMQTT.subscribe("/roomSensor/gpio/5");
			clientMQTT.subscribe("/roomSensor/gpio/ir");
		}
		else {
			//Если не подключились, ждем 10 секунд и пытаемся снова
#ifdef serDeb
			Serial.print("Failed, rc=");
			Serial.print(clientMQTT.state());
			Serial.println(" try again in 10 seconds");
#endif
			delay(10000);
		}
	}
	else {
		if (nowMillisTemp + 5000 <= millis()) {
			sendTemp();
			nowMillisTemp = millis();
		}
		if (shouldSendInfo) { 
			sendReson();
			shouldSendInfo = FALSE;
		}
		if (digitalRead(Motion_Sensor) != state) {
			state = digitalRead(Motion_Sensor);
#ifdef serDeb
			Serial.print(state);
#endif
			clientMQTT.publish("/roomSensor/motion", String(state).c_str());
		}
		yield();
		clientMQTT.loop();
		yield();
	}
}
