#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <FastLED.h>
#include <FastLED_NeoMatrix.h>
#include <Fonts/TomThumb.h>
#include <LightDependentResistor.h>
#include <Wire.h>
#include <SparkFun_APDS9960.h>

String version = "0.36";

////////////////////////////////////////////////////////////////
///////////////////////// Config begin /////////////////////////
// Wifi Config
const char *ssid = "Kindergarten";
const char *password = "53825382";
char *awtrix_server = "192.168.178.39";

/// LDR Config
#define LDR_RESISTOR 10000 //ohms
#define LDR_PIN A0
#define LDR_PHOTOCELL LightDependentResistor::GL5516

/// Matrix Config
#define MATRIX_PIN D2
//uncomment following line to use Matrixtype 2
//#define MATRIX_MODEV2

///////////////////////// Config end /////////////////////////
//////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////
//////////////////////// Don't touch /////////////////////////
char *topics = "awtrixmatrix/";
#define NUMMATRIX (32 * 8)
CRGB leds[NUMMATRIX];

#ifdef MATRIX_MODEV2
FastLED_NeoMatrix *matrix = new FastLED_NeoMatrix(leds, 32, 8, NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_ROWS + NEO_MATRIX_ZIGZAG);
#endif
#ifndef MATRIX_MODEV2
FastLED_NeoMatrix *matrix = new FastLED_NeoMatrix(leds, 32, 8, NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG);
#endif

WiFiClient espClient;
PubSubClient client(espClient);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
LightDependentResistor photocell(LDR_PIN, LDR_RESISTOR, LDR_PHOTOCELL);

unsigned long startTime = 0;
unsigned long endTime = 0;
unsigned long duration;


// APDS-9960
#define APDS9960_INT    D6  //AKA GPIO12 -- Interupt pin
#define APDS9960_SDA    D3  //AKA GPIO0
#define APDS9960_SCL    D1  //AKA GPIO5
SparkFun_APDS9960 apds = SparkFun_APDS9960();
volatile bool isr_flag = 0;

// from http://playground.arduino.cc/Main/Utf8ascii
// ****** UTF8-Decoder: convert UTF8-string to extended ASCII *******
static byte c1;  // Last character buffer

// Convert a single Character from UTF8 to Extended ASCII
// Return "0" if a byte has to be ignored
byte utf8ascii(byte ascii) {
  if ( ascii < 128 ) // Standard ASCII-set 0..0x7F handling
  { c1 = 0;
    return ( ascii );
  }
  // get previous input
byte last = c1;   // get last char
  c1 = ascii;       // remember actual character
  switch (last)     // conversion depending on first UTF8-character
  { case 0xC2: return  (ascii) - 34;  break;
    case 0xC3: return  (ascii | 0xC0) - 34;  break;// TomThumb extended characters off by 34
    case 0x82: if (ascii == 0xAC) return (0xEA);   // special case Euro-symbol
  }
  return  (0);                                     // otherwise: return zero, if character has to be ignored
}

// convert String object from UTF8 String to Extended ASCII
String utf8ascii(String s) {
  String r = "";
  char c;
  for (int i = 0; i < s.length(); i++)
  {
    c = utf8ascii(s.charAt(i));
    if (c != 0) r += c;
  }
  return r;
}

// In Place conversion UTF8-string to Extended ASCII (ASCII is shorter!)
void utf8ascii(char* s) {
  int k = 0;
  char c;
  for (int i = 0; i < strlen(s); i++)
  {
    c = utf8ascii(s[i]);
    if (c != 0)
      s[k++] = c;
  }
  s[k] = 0;
}


String GetChipID()
{
	return String(ESP.getChipId());
}

int GetRSSIasQuality(int rssi)
{
	int quality = 0;

	if (rssi <= -100)
	{
		quality = 0;
	}
	else if (rssi >= -50)
	{
		quality = 100;
	}
	else
	{
		quality = 2 * (rssi + 100);
	}
	return quality;
}

void handleNotFound()
{
	server.sendHeader("Location", String("/update"), true);
	server.send(302, "text/plain", "");
}

void callback(char *topic, byte *payload, unsigned int length)
{
	String s_payload = String((char *)payload);


	String s_topic = String(topic);
	int last = s_topic.lastIndexOf("/") + 1;
	String channel = s_topic.substring(last);

	DynamicJsonBuffer jsonBuffer;
	JsonObject &json = jsonBuffer.parseObject(s_payload);

	if (channel.equals("show"))
	{
		matrix->show();
	}
	else if (channel.equals("clear"))
	{
		matrix->clear();
	}
	else if (channel.equals("drawText"))
	{
		if (json["font"].as<String>().equals("big"))
		{
			matrix->setFont();
			matrix->setCursor(json["x"].as<int16_t>(), json["y"].as<int16_t>() - 1);
		}
		else
		{
			matrix->setFont(&TomThumb);
			matrix->setCursor(json["x"].as<int16_t>(), json["y"].as<int16_t>() + 5);
		}
		matrix->setTextColor(matrix->Color(json["color"][0].as<int16_t>(), json["color"][1].as<int16_t>(), json["color"][2].as<int16_t>()));
		String text = json["text"];
		
		matrix->print(utf8ascii(text));
	}
	else if (channel.equals("drawBMP"))
	{
		int16_t h = json["height"].as<int16_t>();
		int16_t w = json["width"].as<int16_t>();
		int16_t x = json["x"].as<int16_t>();
		int16_t y = json["y"].as<int16_t>();

		for (int16_t j = 0; j < h; j++, y++)
		{
			for (int16_t i = 0; i < w; i++)
			{
				matrix->drawPixel(x + i, y, json["bmp"][j * w + i].as<int16_t>());
			}
		}
	}
	else if (channel.equals("drawLine"))
	{
		matrix->drawLine(json["x0"].as<int16_t>(), json["y0"].as<int16_t>(), json["x1"].as<int16_t>(), json["y1"].as<int16_t>(), matrix->Color(json["color"][0].as<int16_t>(), json["color"][1].as<int16_t>(), json["color"][2].as<int16_t>()));
	}
	else if (channel.equals("drawCircle"))
	{
		matrix->drawCircle(json["x0"].as<int16_t>(), json["y0"].as<int16_t>(), json["r"].as<int16_t>(), matrix->Color(json["color"][0].as<int16_t>(), json["color"][1].as<int16_t>(), json["color"][2].as<int16_t>()));
	}
	else if (channel.equals("drawRect"))
	{
		matrix->drawRect(json["x"].as<int16_t>(), json["y"].as<int16_t>(), json["w"].as<int16_t>(), json["h"].as<int16_t>(), matrix->Color(json["color"][0].as<int16_t>(), json["color"][1].as<int16_t>(), json["color"][2].as<int16_t>()));
	}
		else if (channel.equals("fill"))
	{
		matrix->fillScreen(matrix->Color(json["color"][0].as<int16_t>(), json["color"][1].as<int16_t>(), json["color"][2].as<int16_t>()));
	}
	else if (channel.equals("drawPixel"))
	{
		matrix->drawPixel(json["x"].as<int16_t>(), json["y"].as<int16_t>(), matrix->Color(json["color"][0].as<int16_t>(), json["color"][1].as<int16_t>(), json["color"][2].as<int16_t>()));
	}
	else if (channel.equals("setBrightness"))
	{
		matrix->setBrightness(json["brightness"].as<int16_t>());
	}
	else if (channel.equals("speedtest"))
	{
		matrix->setFont(&TomThumb);
		matrix->setCursor(0, 7);

		endTime = millis();
		duration = endTime - startTime;
		if (duration > 55 || duration < 45)
		{
			matrix->setTextColor(matrix->Color(255, 0, 0));
		}
		else
		{
			matrix->setTextColor(matrix->Color(0, 255, 0));
		}
		matrix->print(duration);
		startTime = millis();
	}
	else if (channel.equals("getMatrixInfo"))
	{
		StaticJsonBuffer<200> jsonBuffer;
		JsonObject& root = jsonBuffer.createObject();
		root["version"] = version;
		root["wifirssi"] = String(WiFi.RSSI());
		root["wifiquality"] =GetRSSIasQuality(WiFi.RSSI());
		root["wifissid"] =WiFi.SSID();
		root["getIP"] =WiFi.localIP().toString();
		String JS;
		root.printTo(JS);
		client.publish("matrixInfo", JS.c_str());
	}
	else if (channel.equals("getLUX"))
	{
		StaticJsonBuffer<200> jsonBuffer;

		client.publish("matrixLux", String(photocell.getCurrentLux()).c_str());
	}
}

void reconnect()
{
	// Loop until we're reconnected
	while (!client.connected())
	{

		// Attempt to connect

		String clientId = "AWTRIXController-";
    clientId += String(random(0xffff), HEX);

		if (client.connect(clientId.c_str()))
		{
			// ... and resubscribe
			client.subscribe((String(topics) + "#").c_str());
			// ... and publish
			client.publish("matrixstate", "connected");
		}
		else
		{
			// Wait 5 seconds before retrying
			delay(5000);
		}
	}
}

void interruptRoutine() {
  isr_flag = 1;
}

void handleGesture() {
    if ( apds.isGestureAvailable() ) {
    switch ( apds.readGesture() ) {
      case DIR_UP:
			 Serial.println("UP");
        client.publish("control", "UP");
        break;
      case DIR_DOWN:
			     Serial.println("DOWN");
        client.publish("control", "DOWN");
        break;
      case DIR_LEFT:
			    Serial.println("LEFT");
     client.publish("control", "LEFT");
        break;
      case DIR_RIGHT:
			 Serial.println("RIGHT");
       client.publish("control", "RIGHT");
        break;
      case DIR_NEAR:
			  Serial.println("NEAR");
         client.publish("control", "NEAR");
        break;
      case DIR_FAR:
			  Serial.println("FAR");
        client.publish("control", "FAR");
        break;
      default:
			 Serial.println("NONE");
        client.publish("control", "NONE");
    }
  }
}

void setup()
{
	FastLED.addLeds<NEOPIXEL, MATRIX_PIN>(leds, NUMMATRIX).setCorrection(TypicalLEDStrip);
	WiFi.mode(WIFI_STA);
	WiFi.begin(ssid, password);
	matrix->begin();
	matrix->setTextWrap(false);
	matrix->setBrightness(80);
	matrix->setFont(&TomThumb);
	matrix->setCursor(0, 7);
	matrix->print("WiFi...");
	matrix->show();
	while (WiFi.status() != WL_CONNECTED)
	{
		delay(500);
	}

	MDNS.begin("AWTRIXController");

	photocell.setPhotocellPositionOnGround(false);
	
	matrix->clear();
	matrix->setCursor(0, 7);
	matrix->print("Ready!");
	matrix->show();

	httpUpdater.setup(&server);
	server.onNotFound(handleNotFound);
	server.begin();
Serial.begin(115200);
	client.setServer(awtrix_server, 7001);
	client.setCallback(callback);

	Wire.begin(APDS9960_SDA,APDS9960_SCL);
  pinMode(APDS9960_INT, INPUT);
	attachInterrupt(APDS9960_INT, interruptRoutine, FALLING);

	  // Initialize APDS-9960 (configure I2C and initial values)
  if ( apds.init() ) {
    Serial.println(F("APDS-9960 initialization complete"));
  } else {
    Serial.println(F("Something went wrong during APDS-9960 init!"));
  }
  
  // Start running the APDS-9960 gesture sensor engine
  if ( apds.enableGestureSensor(true) ) {
    Serial.println(F("Gesture sensor is now running"));
  } else {
    Serial.println(F("Something went wrong during gesture sensor init!"));
  }
}

void loop()
{

	if (!client.connected())
	{
		reconnect();
	}else{
		if( isr_flag == 1 ) {
    detachInterrupt(APDS9960_INT);
    handleGesture();
    isr_flag = 0;
    attachInterrupt(APDS9960_INT, interruptRoutine, FALLING);
  }
	}
	client.loop();

	server.handleClient();
	 

}

