/*
 R. J. Tidey 2017/02/22
 Bell Push detector / notifier to IFTTT
 Can action via a URL like snapshot from a camera
 Also Supports temperature sensors to Easy IoT server.
 Both can be used together if required.
 Web software update service included
 WifiManager can be used to config wifi network
 
 Temperature reporting Code based on work by Igor Jarc
 Some code based on https://github.com/DennisSc/easyIoT-ESPduino/blob/master/sketches/ds18b20.ino
 
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 version 2 as published by the Free Software Foundation.
 */
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "Base64.h"
#include <IFTTTMaker.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WiFiManager.h>

/*
Wifi Manager Web set up
If WM_NAME defined then use WebManager
*/
#define WM_NAME "bellPushWebSetup"
#define WM_PASSWORD "password"
#ifdef WM_NAME
	WiFiManager wifiManager;
#endif
//uncomment to use a static IP
//#define WM_STATIC_IP 192,168,0,100
//#define WM_STATIC_GATEWAY 192,168,0,1

int timeInterval = 50;
#define WIFI_CHECK_TIMEOUT 30000
unsigned long elapsedTime;
unsigned long wifiCheckTime;

#define AP_AUTHID "1234"

//IFTT and request key words
#define MAKER_KEY "makerkey"

//For update service
String host = "esp8266-hall";
const char* update_path = "/firmware";
const char* update_username = "admin";
const char* update_password = "password";

//bit mask for server support
#define EASY_IOT_MASK 1
#define BOILER_MASK 4
#define BELL_MASK 8
int serverMode = 1;

//define BELL_PIN negative to disable
#define BELL_PIN 12
#define BELL_MIN_INTERVAL 10
#define BELL_OFF 0
#define BELL_ON 1
#define BELL_ACTIONED 2
int bellState = BELL_OFF;
unsigned long  bellOnTime = 0;

#define ONE_WIRE_BUS 13  // DS18B20 pin
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature DS18B20(&oneWire);
int tempValid;

//History
#define MAX_RECENT 64
unsigned long recentTimes[MAX_RECENT];
int recentIndex;

//Timing
int minMsgInterval = 10; // in units of 1 second
int forceInterval = 300; // send message after this interval even if temp same 
int boilerInterval = 0; // interval for measuring boiler utilisation % Typically 600. 0 is off

//AP definitions
#define AP_SSID "ssid"
#define AP_PASSWORD "password"
#define AP_MAX_WAIT 10
String macAddr;

#define AP_PORT 80

ESP8266WebServer server(AP_PORT);
ESP8266HTTPUpdateServer httpUpdater;
WiFiClient cClient;
WiFiClientSecure sClient;
IFTTTMaker ifttt(MAKER_KEY, sClient);

//Config remote fetch from web page
#define CONFIG_IP_ADDRESS  "192.168.0.7"
#define CONFIG_PORT        80
//Comment out for no authorisation else uses same authorisation as EIOT server
#define CONFIG_AUTH 1
#define CONFIG_PAGE "espConfig"
#define CONFIG_RETRIES 10

// EasyIoT server definitions
#define EIOT_USERNAME    "admin"
#define EIOT_PASSWORD    "password"
#define EIOT_IP_ADDRESS  "192.168.0.7"
#define EIOT_PORT        80
#define USER_PWD_LEN 40
char unameenc[USER_PWD_LEN];
String eiotNode = "-1";
String bellNode = "-1";
String bellEvent = "-1";
String bellActionURL = "-1";
String bellNotify = "-1";
String boilerNode = "-1";

//Action URL access
//Comment out username if no authentication
#define ACTION_USERNAME "cam"
#define ACTION_PASSWORD "password"
char action_nameenc[USER_PWD_LEN];

//general variables
float oldTemp, newTemp;
int forceCounter = 0;
long lastMsg = 0;
float diff = 0.1;
float boilerDelta = 0.02; //if Temp change per second > then boilerOn
float boilerOn;
int boilerOnCount;

/*
  Bell Push interrupt handler
*/
void ICACHE_RAM_ATTR bellPushInterrupt() {
	//Ignore any edges unless state is off
	if (bellState == BELL_OFF)
		bellState = BELL_ON;
}

/*
  Set up basic wifi, collect config from flash/server, initiate update server
*/
void setup() {
	Serial.begin(115200);
	char uname[USER_PWD_LEN];
	String str = String(EIOT_USERNAME)+":"+String(EIOT_PASSWORD);  
	str.toCharArray(uname, USER_PWD_LEN); 
	memset(unameenc,0,sizeof(unameenc));
	base64_encode(unameenc, uname, strlen(uname));
#ifdef ACTION_USERNAME
	str = String(ACTION_USERNAME)+":"+String(ACTION_PASSWORD);  
	str.toCharArray(uname, USER_PWD_LEN); 
	memset(action_nameenc,0,sizeof(action_nameenc));
	base64_encode(action_nameenc, uname, strlen(uname));
#endif
	Serial.println("Set up Web update service");
	wifiConnect(0);
	macAddr = WiFi.macAddress();
	macAddr.replace(":","");
	Serial.println(macAddr);
	getConfig();

	//Update service
	MDNS.begin(host.c_str());
	httpUpdater.setup(&server, update_path, update_username, update_password);
	server.on("/recent", recentEvents);
	server.on("/bellPush", testBellPush);
	server.on("/reloadConfig", reloadConfig);
	server.begin();

	MDNS.addService("http", "tcp", 80);
	if(serverMode & BELL_MASK) {
		pinMode(BELL_PIN, INPUT);
		attachInterrupt(BELL_PIN, bellPushInterrupt, RISING);
	}
	Serial.println("Set up complete");
}

/*
  Connect to local wifi with retries
  If check is set then test the connection and re-establish if timed out
*/
int wifiConnect(int check) {
	if(check) {
		if(WiFi.status() != WL_CONNECTED) {
			if((elapsedTime - wifiCheckTime) * timeInterval > WIFI_CHECK_TIMEOUT) {
				Serial.println("Wifi connection timed out. Try to relink");
			} else {
				return 1;
			}
		} else {
			wifiCheckTime = elapsedTime;
			return 0;
		}
	}
	wifiCheckTime = elapsedTime;
#ifdef WM_NAME
	Serial.println("Set up managed Web");
#ifdef WM_STATIC_IP
	wifiManager.setSTAStaticIPConfig(IPAddress(WM_STATIC_IP), IPAddress(WM_STATIC_GATEWAY), IPAddress(255,255,255,0));
#endif
	wifiManager.autoConnect(WM_NAME, WM_PASSWORD);
#else
	Serial.println("Set up manual Web");
	int retries = 0;
	Serial.print("Connecting to AP");
	#ifdef AP_IP
		IPAddress addr1(AP_IP);
		IPAddress addr2(AP_DNS);
		IPAddress addr3(AP_GATEWAY);
		IPAddress addr4(AP_SUBNET);
		WiFi.config(addr1, addr2, addr3, addr4);
	#endif
	WiFi.begin(AP_SSID, AP_PASSWORD);
	while (WiFi.status() != WL_CONNECTED && retries < AP_MAX_WAIT) {
		delay(1000);
		Serial.print(".");
		retries++;
	}
	Serial.println("");
	if(retries < AP_MAX_WAIT) {
		Serial.print("WiFi connected ip ");
		Serial.print(WiFi.localIP());
		Serial.printf(":%d mac %s\r\n", AP_PORT, WiFi.macAddress().c_str());
		return 1;
	} else {
		Serial.println("WiFi connection attempt failed"); 
		return 0;
	} 
#endif
}

/*
  Get config from server
*/
void getConfig() {
	int responseOK = 0;
	int retries = CONFIG_RETRIES;
	String line = "";

	while(retries > 0) {
		clientConnect(CONFIG_IP_ADDRESS, CONFIG_PORT);
		Serial.print("Try to GET config data from Server for: ");
		Serial.println(macAddr);

		cClient.print(String("GET /") + CONFIG_PAGE + " HTTP/1.1\r\n" +
			"Host: " + String(CONFIG_IP_ADDRESS) + "\r\n" + 
		#ifdef CONFIG_AUTH
				"Authorization: Basic " + unameenc + " \r\n" + 
		#endif
			"Content-Length: 0\r\n" + 
			"Connection: close\r\n" + 
			"\r\n");
		int config = 100;
		int timeout = 0;
		while (cClient.connected() && timeout < 10){
			if (cClient.available()) {
				timeout = 0;
				line = cClient.readStringUntil('\n');
				if(line.indexOf("HTTP") == 0 && line.indexOf("200 OK") > 0)
					responseOK = 1;
				//Don't bother processing when config complete
				if (config >= 0) {
					line.replace("\r","");
					Serial.println(line);
					//start reading config when mac address found
					if (line == macAddr) {
						config = 0;
					} else {
						if(line.charAt(0) != '#') {
							switch(config) {
								case 0: host = line;break;
								case 1: serverMode = line.toInt();break;
								case 2: eiotNode = line;break;
								case 3: break; //spare
								case 4: minMsgInterval = line.toInt();break;
								case 5:	forceInterval = line.toInt();
								case 6:	boilerInterval = line.toInt();
								case 7:	boilerNode = line;
								case 8: bellNode  = line;break;
								case 9: bellNotify  = line;break;
								case 10:bellEvent = line;break;
								case 11:
									bellActionURL = line;
									Serial.println("Config fetched from server OK");
									config = -100;
									break;
							}
							config++;
						}
					}
				}
			} else {
				delay(1000);
				timeout++;
				Serial.println("Wait for response");
			}
		}
		cClient.stop();
		if(responseOK == 1)
			break;
		retries--;
	}
	Serial.println();
	Serial.println("Connection closed");
	Serial.print("host:");Serial.println(host);
	Serial.print("serverMode:");Serial.println(serverMode);
	Serial.print("eiotNode:");Serial.println(eiotNode);
	Serial.print("minMsgInterval:");Serial.println(minMsgInterval);
	Serial.print("forceInterval:");Serial.println(forceInterval);
	Serial.print("boilerInterval:");Serial.println(boilerInterval);
	Serial.print("bellNode:");Serial.println(bellNode);
	Serial.print("bellNotify:");Serial.println(bellNotify);
	Serial.print("boilerNode:");Serial.println(boilerNode);
	Serial.print("bellEvent:");Serial.println(bellEvent);
	Serial.print("bellActionURL:");Serial.println(bellActionURL);
}

/*
  reload Config
*/
void reloadConfig() {
	if (server.arg("auth") != AP_AUTHID) {
		Serial.println("Unauthorized");
		server.send(401, "text/html", "Unauthorized");
	} else {
		getConfig();
		server.send(200, "text/html", "Config reload");
	}
}


/*
 return recent events
*/
void recentEvents() {
	Serial.println("recent events request received");
	if (server.arg("auth") != AP_AUTHID) {
		Serial.println("Unauthorized notify request");
		server.send(401, "text/html", "Unauthorized");
	} else {
		String response = "Recent events<BR>";
		long minutes;
		int recent = recentIndex - 1;
		if(recent < 0) recent = MAX_RECENT - 1;
		for(int i = 0;i<MAX_RECENT;i++) {
			if((recentTimes[recent]) >0) {
				minutes = (elapsedTime - recentTimes[recent]) * timeInterval / 60000;
				response += "Bell pushed " + String(minutes) + " minutes ago<BR>";
			}
			recent--;
			if(recent < 0) recent = MAX_RECENT - 1;
		}
		server.send(200, "text/html", response);
	}
}

/*
  testBellPush
*/
void testBellPush() {
	if (server.arg("auth") != AP_AUTHID) {
		Serial.println("Unauthorized");
		server.send(401, "text/html", "Unauthorized");
	} else {
		String response;
		if (bellState == BELL_OFF) {
			bellState = BELL_ON;
			response = "Bell push simulated<BR>";
			Serial.println("Bell push simulated");
		} else {
			response = "Bell already in Pushed state; Ignored<BR>";
			Serial.println("Bell push simulation ignored");
		}
		server.send(200, "text/html", response);
	}
}


/*
  Establish client connection
*/
void clientConnect(char* host, uint16_t port) {
	int retries = 0;
   
	while(!cClient.connect(host, port)) {
		Serial.print("?");
		retries++;
		if(retries > CONFIG_RETRIES) {
			Serial.print("Client connection failed:" );
			Serial.println(host);
			wifiConnect(0); 
			retries = 0;
		} else {
			delay(5000);
		}
	}
}

/*
  Check if value changed enough to report
*/
bool checkBound(float newValue, float prevValue, float maxDiff) {
	return !isnan(newValue) &&
         (newValue < prevValue - maxDiff || newValue > prevValue + maxDiff);
}

/*
 Send notify trigger to IFTTT
*/
int ifttt_notify(String eventName, String value1, String value2, String value3) {
  if(ifttt.triggerEvent(eventName, value1, value2, value3)){
    Serial.println("Notification successfully sent");
	return 1;
  } else {
    Serial.println("Failed!");
	return 0;
  }
}

/*
 Send report to easyIOTReport
 if digital = 1, send digital else analog
*/
void easyIOTReport(String node, float value, int digital) {
	int retries = CONFIG_RETRIES;
	int responseOK = 0;
	String url = "/Api/EasyIoT/Control/Module/Virtual/" + node;
	
	// generate EasIoT server node URL
	if(digital == 1) {
		if(value > 0)
			url += "/ControlOn";
		else
			url += "/ControlOff";
	} else
		url += "/ControlLevel/" + String(value);

	Serial.print("POST data to URL: ");
	Serial.println(url);
	while(retries > 0) {
		clientConnect(EIOT_IP_ADDRESS, EIOT_PORT);
		cClient.print(String("POST ") + url + " HTTP/1.1\r\n" +
				"Host: " + String(EIOT_IP_ADDRESS) + "\r\n" + 
				"Connection: close\r\n" + 
				"Authorization: Basic " + unameenc + " \r\n" + 
				"Content-Length: 0\r\n" + 
				"\r\n");

		delay(100);
		while(cClient.available()){
			String line = cClient.readStringUntil('\r');
			if(line)
			Serial.print(line);
			if(line.indexOf("HTTP") == 0 && line.indexOf("200 OK") > 0)
				responseOK = 1;
		}
		cClient.stop();
		if(responseOK)
			break;
		else
			Serial.println("Retrying EIOT report");
		retries--;
	}
	Serial.println();
	Serial.println("Connection closed");
}

/*
 Access a URL
*/
void getFromURL(String url) {
	String url_host;
	uint16_t url_port;
	char host[32];
	int portStart, addressStart;
	
	Serial.println("get from " + url);
	portStart = url.indexOf(":");
	addressStart = url.indexOf("/");
	if(portStart >=0) {
		url_port = (url.substring(portStart+1,addressStart)).toInt();
		url_host = url.substring(0,portStart);
	} else {
		url_port = 80;
		url_host = url.substring(0,addressStart);
	}
	strcpy(host, url_host.c_str());
	
	clientConnect(host, url_port);
	cClient.print(String("GET ") + url.substring(addressStart) + " HTTP/1.1\r\n" +
		   "Host: " + url_host + "\r\n" + 
#ifdef ACTION_USERNAME
			"Authorization: Basic " + action_nameenc + " \r\n" + 
#endif
		   "Connection: close\r\n" + 
		   "Content-Length: 0\r\n" + 
		   "\r\n");
	cClient.stop();
}

/*
 Actions when bell push detected
*/
void actionBellOn() {
	Serial.println("Bell push detected");
	if (bellEvent != "-1")
		ifttt_notify(bellEvent, bellNotify, "", "");
	if(bellNode != "-1") {
		easyIOTReport(bellNode, 1, 1);
	}
	if(bellActionURL != "-1") {
		getFromURL(bellActionURL);
	}
	bellOnTime = elapsedTime;
	recentTimes[recentIndex] = elapsedTime;
	recentIndex++;
	if(recentIndex >= MAX_RECENT) recentIndex = 0;
}

/*
  Main loop to read temperature and publish as required
*/
void loop() {
	tempValid = 0;
	float value;
	for(int r = 0;r<5;r++) {
		DS18B20.requestTemperatures(); 
		newTemp = DS18B20.getTempCByIndex(0);
		if(newTemp != 85.0 && newTemp != (-127.0)) {
			tempValid = 1;
			break;
		}
	}

	if (tempValid) {
		if(boilerInterval > 0 && (serverMode & BOILER_MASK)) {
			if((newTemp-oldTemp) / minMsgInterval > boilerDelta) {
				Serial.println("Boiler On");
				boilerOn += 100;
			}
			boilerOnCount++;
			if(boilerOnCount * minMsgInterval >= boilerInterval) {
				value = boilerOn/boilerOnCount;
				easyIOTReport(boilerNode, value, 0);
				boilerOnCount = 0;
				boilerOn = 0;
			}
		}
		if(checkBound(newTemp, oldTemp, diff) || forceCounter >= forceInterval) {
			forceCounter = 0;
			oldTemp = newTemp;
			Serial.print("New temperature:");
			Serial.println(String(newTemp).c_str());
			if(serverMode & EASY_IOT_MASK) {
				easyIOTReport(eiotNode, newTemp, 0);
			}
		}
	} else {
		Serial.println("Invalid temp reading");
	}
	for(int i = minMsgInterval; i > 0;i--){
		server.handleClient();
		forceCounter++;
		for(int j = 0; j < (1000 / timeInterval);j++) {
			if(bellState == BELL_ON) {
				actionBellOn();
				bellState = BELL_ACTIONED;
			} else if(bellState == BELL_ACTIONED && (elapsedTime - bellOnTime) * timeInterval / 1000 > BELL_MIN_INTERVAL) {
				bellState = BELL_OFF;
				if(bellNode != "-1") easyIOTReport(bellNode, 0, 1);
			}
			delay(timeInterval);
			elapsedTime++;
			wifiConnect(1);
		}
	}
}
