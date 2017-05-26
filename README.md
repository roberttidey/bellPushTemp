# bellPushTemp
Bell Push and Temperature sensor module for esp8266 reporting to EasyIOT and IFTTT

Features
Reports temperature and doorbell activity to EasyIOT
Sends IFTTT notification on doorbell activity
Can trigger other activity via a url (e.g snapshot of a camera) when doorbell pushed
Configuration fetched from a web server file keyed on Mac Address of esp-8266 to allow for multiple units
Configurable update interval time and force updates even temperature unchanged
Can detect Central heating boiler activity if temp sensor on boiler output
Web update of software
Retries on network and server connection requests
Basic network conections controlled by wifiManager or can be manually set up in code.




