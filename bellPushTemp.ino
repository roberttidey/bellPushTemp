/*
 R. J. Tidey 2017/02/22
 Bell Push detector / notifier to pushover or IFTTT
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
//Uncomment to use IFTTT instead of pushover
//#define USE_IFTTT
#define ESP8266

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#ifdef USE_IFTTT
	#include <IFTTTMaker.h>
#endif
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WiFiManager.h>

//put -1 s at end
int unusedPins[11] = {0,2,4,5,12,14,15,16,-1,-1,-1};

/*
Wifi Manager Web set up
If WM_NAME defined then use WebManager
*/
#define WM_NAME "bellPIRWebSetup"
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

#define AP_AUTHID "1234567"
#define AP_SECURITY "?event=zoneSet&auth=12345678"

//For update service
String host = "esp8266-hall";
const char* update_path = "/firmware";
const char* update_username = "admin";
const char* update_password = "password";

//bit mask for server support
#define EASY_IOT_MASK 1
#define BOILER_MASK 4
#define BELL_MASK 8
#define SECURITY_MASK 16
#define LIGHTCONTROL_MASK 32
#define RESET_MASK 64
int serverMode = 1;

//define BELL_PIN negative to disable
#define BELL_PIN 12
#define BELL_MIN_INTERVAL 10
#define BELL_OFF 0
#define BELL_ON 1
#define BELL_ACTIONED 2
// minimum time in mSec to recognise bell edges
unsigned long  bellIntTime = 0;
#define BELL_INTTIME_MIN 14
#define BELL_INTTIME_MAX 26
// minimum number of bell edges to trigger bell on event
int bellIntTrigger = 5;
int bellIntCount = 0;
int bellState = BELL_OFF;
unsigned long  bellOnTime = 0;

// Push notifications
const String NOTIFICATION_APP =  "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzz";
const String NOTIFICATION_USER = "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzz";  // This can be a group or user

bool isSendPush = false;
String pushParameters;

//Pins for Reset testing
#define RESET_PIN 4
#define PROG_PIN 0

//define SECURITY sensor pin
#define SECURITY_PIN 5
int securityState = 0;
int securityDevice = 0;

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
HTTPClient cClient;
WiFiClientSecure https;

#ifdef USE_IFTTT
	//IFTT and request key words
	#define MAKER_KEY "zzzzzzzzzzzzzzzzzzz"
	IFTTTMaker ifttt(MAKER_KEY, https);
#endif

//Config remote fetch from web page, change port in url if needed
#define CONFIG_IP_ADDRESS  "http://192.168.0.250/espConfig"
//Comment out for no authorisation else uses same authorisation as EIOT server
#define CONFIG_AUTH 1
#define CONFIG_PAGE "espConfig"
#define CONFIG_RETRIES 10

// EasyIoT server definitions, change port in url if needed
#define EIOT_USERNAME    "admin"
#define EIOT_PASSWORD    "password"
#define EIOT_IP_ADDRESS  "http://192.168.0.250/Api/EasyIoT/Control/Module/Virtual/"
String eiotNode = "-1";
String bellNode = "-1";
String bellEvent = "-1";
String bellActionURL = "-1";
String bellNotify = "-1";
String boilerNode = "-1";
String securityURL = "-1";

//Action URL access
//Comment out username if no authentication
#define ACTION_USERNAME "cam"
#define ACTION_PASSWORD "password"

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
	unsigned long m = millis();
	//Ignore fast edges
	if(((m-bellIntTime) > BELL_INTTIME_MIN) && ((m-bellIntTime) < BELL_INTTIME_MAX)) {
		//Ignore any edges unless state is off
		if (bellState == BELL_OFF) {
			bellIntCount++;
			bellState = BELL_ON;
		}
	}
	bellIntTime = m;
}

void ICACHE_RAM_ATTR  delaymSec(unsigned long mSec) {
	unsigned long ms = mSec;
	while(ms > 100) {
		delay(100);
		ms -= 100;
		ESP.wdtFeed();
	}
	delay(ms);
	ESP.wdtFeed();
	yield();
}

void ICACHE_RAM_ATTR  delayuSec(unsigned long uSec) {
	unsigned long us = uSec;
	while(us > 100000) {
		delay(100);
		us -= 100000;
		ESP.wdtFeed();
	}
	delayMicroseconds(us);
	ESP.wdtFeed();
	yield();
}

void unusedIO() {
	int i;
	
	for(i=0;i<11;i++) {
		if(unusedPins[i] < 0) {
			break;
		} else if(unusedPins[i] != 16) {
			pinMode(unusedPins[i],INPUT_PULLUP);
		} else {
			pinMode(16,INPUT_PULLDOWN_16);
		}
	}
}

/*
  Set up basic wifi, collect config from flash/server, initiate update server
*/
void setup() {
	unusedIO();
	Serial.begin(115200);
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
	server.on("/resetTest", resetTest);
	server.begin();

	MDNS.addService("http", "tcp", 80);
	if(serverMode & BELL_MASK) {
		pinMode(BELL_PIN, INPUT);
		attachInterrupt(BELL_PIN, bellPushInterrupt, RISING);
	}
	if(serverMode & SECURITY_MASK) {
		pinMode(SECURITY_PIN, INPUT);
	}
	if(serverMode & RESET_MASK) {
		digitalWrite(RESET_PIN,1);
		digitalWrite(PROG_PIN,1);
		pinMode(RESET_PIN, OUTPUT);
		pinMode(PROG_PIN, OUTPUT);
	}
	Serial.println("Set up complete");
}

/*
  Connect to local wifi with retries
  If check is set then test the connection and re-establish if timed out
*/
int wifiConnect(int check) {
	if(check) {
		if((elapsedTime - wifiCheckTime) * timeInterval > WIFI_CHECK_TIMEOUT) {
			if(WiFi.status() != WL_CONNECTED) {
				Serial.println("Wifi connection timed out. Try to relink");
			} else {
				wifiCheckTime = elapsedTime;
				return 1;
			}
		} else {
			return 0;
		}
	}
	wifiCheckTime = elapsedTime;
#ifdef WM_NAME
	Serial.println("Set up managed Web");
#ifdef WM_STATIC_IP
	wifiManager.setSTAStaticIPConfig(IPAddress(WM_STATIC_IP), IPAddress(WM_STATIC_GATEWAY), IPAddress(255,255,255,0));
#endif
	wifiManager.setConfigPortalTimeout(180);
	wifiManager.autoConnect(WM_NAME, WM_PASSWORD);
	WiFi.mode(WIFI_STA);
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
		delaymSec(1000);
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
	int httpCode;
	int len;
	int retries = CONFIG_RETRIES;
	String url = String(CONFIG_IP_ADDRESS);
	Serial.println("Config url - " + url);
	String line = "";

	while(retries > 0) {
		Serial.print("Try to GET config data from Server for: ");
		Serial.println(macAddr);
		#ifdef CONFIG_AUTH
			cClient.setAuthorization(EIOT_USERNAME, EIOT_PASSWORD);
		#else
			cClient.setAuthorization("");		
		#endif
		cClient.begin(url);
		httpCode = cClient.GET();
		if (httpCode > 0) {
			if (httpCode == HTTP_CODE_OK) {
				responseOK = 1;
				int config = 100;
				len = cClient.getSize();
				if (len < 0) len = -10000;
				Serial.println("Response Size:" + String(len));
				WiFiClient * stream = cClient.getStreamPtr();
				while (cClient.connected() && (len > 0 || len <= -10000)) {
					if(stream->available()) {
						line = stream->readStringUntil('\n');
						len -= (line.length() +1 );
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
										case 11:bellActionURL = line;break;
										case 12:securityDevice = line.toInt();break;
										case 13:securityURL = line;break;
										case 14:
											bellIntTrigger = line.toInt();
											Serial.println("Config fetched from server OK");
											config = -100;
											break;
									}
									config++;
								}
							}
						}
					}
				}
			}
		} else {
			Serial.printf("[HTTP] POST... failed, error: %s\n", cClient.errorToString(httpCode).c_str());
		}
		cClient.end();
		if(responseOK)
			break;
		Serial.println("Retrying get config");
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
	Serial.print("securityDevice:");Serial.println(securityDevice);
	Serial.print("securityURL:");Serial.println(securityURL);
	Serial.print("bellIntTrigger:");Serial.println(bellIntTrigger);
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
				if(recentTimes[recent] & 0x01)
					response += "Security change ";
				else
					response += "Bell pushed ";
				response += String(minutes) + " minutes ago<BR>";
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
  resetTest
*/
void resetTest() {
	int resetWidth;
	int progWidth;
	if(serverMode & RESET_MASK)	{
		resetWidth = server.arg("reset").toInt();
		progWidth = server.arg("prog").toInt();
		Serial.printf("reset %d prog %d\r\n", resetWidth, progWidth);
		if(progWidth && progWidth < resetWidth)
			progWidth = resetWidth;
		if(progWidth)
			digitalWrite(PROG_PIN, 0);
		digitalWrite(RESET_PIN, 0);
		delayuSec(resetWidth);
		digitalWrite(RESET_PIN, 1);
		if(progWidth) {
			delayuSec(progWidth - resetWidth);
			digitalWrite(PROG_PIN, 1);
		}
		server.send(200, "text/html", "reset-prog sent");
	} else {
		server.send(200, "text/html", "reset-prog not active");
	}
}


/*
  Check if value changed enough to report
*/
bool checkBound(float newValue, float prevValue, float maxDiff) {
	return !isnan(newValue) &&
         (newValue < prevValue - maxDiff || newValue > prevValue + maxDiff);
}

#ifdef USE_IFTTT
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
#endif

/*
 Start notification to pushover
*/
void startPushNotification(String message) {
	if(isSendPush == false) {
		// Form the string
		pushParameters = "token=" + NOTIFICATION_APP + "&user=" + NOTIFICATION_USER + "&message=" + message;
		isSendPush = true;
		Serial.println("Connecting to push server");
		https.connect("api.pushover.net", 443);
	}
}

// Keep track of the pushover server connection status without holding 
// up the code execution, and then send notification
void updatePushServer(){
    if(isSendPush == true) {
		if(https.connected()) {
			int length = pushParameters.length();
			Serial.println("Posting push notification: " + pushParameters);
			https.println("POST /1/messages.json HTTP/1.1");
			https.println("Host: api.pushover.net");
			https.println("Connection: close\r\nContent-Type: application/x-www-form-urlencoded");
			https.print("Content-Length: ");
			https.print(length);
			https.println("\r\n");
			https.print(pushParameters);

			https.stop();
			isSendPush = false;
			Serial.println("Finished posting notification.");
		} else {
			Serial.println("Not connected.");
		}
    }
}

/*
 Send report to easyIOTReport
 if digital = 1, send digital else analog
*/
void easyIOTReport(String node, float value, int digital) {
	int retries = CONFIG_RETRIES;
	int responseOK = 0;
	int httpCode;
	String url = String(EIOT_IP_ADDRESS) + node;
	// generate EasIoT server node URL
	if(digital == 1) {
		if(value > 0)
			url += "/ControlOn";
		else
			url += "/ControlOff";
	} else {
		url += "/ControlLevel/" + String(value);
	}
	Serial.print("POST data to URL: ");
	Serial.println(url);
	while(retries > 0) {
		cClient.setAuthorization(EIOT_USERNAME, EIOT_PASSWORD);
		cClient.begin(url);
		httpCode = cClient.GET();
		if (httpCode > 0) {
			if (httpCode == HTTP_CODE_OK) {
				String payload = cClient.getString();
				Serial.println(payload);
				responseOK = 1;
			}
		} else {
			Serial.printf("[HTTP] POST... failed, error: %s\n", cClient.errorToString(httpCode).c_str());
		}
		cClient.end();
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
void getFromURL(String url, int retryCount, char* user, char* password) {
	int retries = retryCount;
	int responseOK = 0;
	int httpCode;
	
	Serial.println("get from " + url);
	
	while(retries > 0) {
		if(user) cClient.setAuthorization(user, password);
		cClient.begin(url);
		httpCode = cClient.GET();
		if (httpCode > 0) {
			if (httpCode == HTTP_CODE_OK) {
				String payload = cClient.getString();
				Serial.println(payload);
				responseOK = 1;
			}
		} else {
			Serial.printf("[HTTP] POST... failed, error: %s\n", cClient.errorToString(httpCode).c_str());
		}
		cClient.end();
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
 Actions when bell push detected
*/
void actionBellOn() {
	Serial.println("Bell push detected");
	if (bellEvent != "-1") {
#ifdef USE_IFTTT
		ifttt_notify(bellEvent, bellNotify, "", "");
#else
		startPushNotification(bellEvent);
#endif
	}
	if(bellNode != "-1") {
		easyIOTReport(bellNode, 1, 1);
	}
	if(bellActionURL != "-1") {
		getFromURL(bellActionURL, CONFIG_RETRIES, ACTION_USERNAME, ACTION_PASSWORD);
	}
	bellOnTime = elapsedTime;
	//even times are bell pushes
	recentTimes[recentIndex] = elapsedTime & 0xFFFFFFFE;
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
		updatePushServer();
		forceCounter++;
		for(int j = 0; j < (1000 / timeInterval);j++) {
			if(bellState == BELL_OFF) {
				if(bellIntCount>bellIntTrigger) {
					 bellState = BELL_ON;
				} else {
					//lower bellcount to remove isolated interrupts
					if(bellIntCount>0) bellIntCount--;
				}
			}
			if(bellState == BELL_ON) {
				actionBellOn();
				bellState = BELL_ACTIONED;
				bellIntCount = 0;
			} else if(bellState == BELL_ACTIONED && (elapsedTime - bellOnTime) * timeInterval / 1000 > BELL_MIN_INTERVAL) {
				bellState = BELL_OFF;
				if(bellNode != "-1") easyIOTReport(bellNode, 0, 1);
			}
			//sanitise bellIntCount to stop it going out of range
			if(bellIntCount < 0 || bellIntCount > bellIntTrigger + 5)
				bellIntCount=0;
			delaymSec(timeInterval);
			elapsedTime++;
			wifiConnect(1);
		}
	}
}
