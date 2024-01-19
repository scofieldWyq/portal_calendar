#include <mdns.h>
#include "Configuration.h"
#include "weather.h"
#include "time_util.h"
#include "Display.h"
#include "resources/www/index_html.h"

#define HTTP_OK 200
#define HTTP_ACCEPTED 202
#define HTTP_UNAUTHORIZED 401
#define HTTP_BAD_REQUEST 400
#define HTTP_NOT_FOUND 404
#define HTTP_CONFLICT 409
#define HTTP_INTERNAL_SERVER_ERROR 500
#define HTTP_BAD_GATEWAY 502
#define HTTP_NETWORK_AUTHENTICATION_REQUIRED 511

// Max 15 characters
#define KEY_WIFI_SSID "wifiSsid"
#define KEY_WIFI_PASS "wifiPass"
#define KEY_HOSTNAME "hostname"
#define KEY_SHOW_DAY "showDay"
#define KEY_SHOW_MONTH "showMonth"
#define KEY_SHOW_YEAR "showYear"
#define KEY_TIMEZONE_NAME "timezone"
#define KEY_LOCALE "locale"
#define KEY_PRIMARY_NTP_SERVER "ntpServer1"
#define KEY_SECONDARY_NTP_SERVER "ntpServer2"
#define KEY_PRIMARY_TIMEZONED_SERVER "tzdServer1"
#define KEY_SECONDARY_TIMEZONED_SERVER "tzdServer2"
#define KEY_WEATHER_ENABLED "weatherEnabled"
#define KEY_OWM_API_KEY "owmApiKey"
#define KEY_WEATHER_LOCATION_NAME "weatherLocName"
#define KEY_WEATHER_LOCATION_LATITUDE "weatherLat"
#define KEY_WEATHER_LOCATION_LONGITUDE "weatherLon"
#define KEY_WEATHER_DISPLAY_TYPE "weatherDisplay"
#define KEY_WEATHER_SECONDARY_INFO "weatherInfo"
#define KEY_WEATHER_UNITS "weatherUnits"
#define KEY_WEATHER_START_HOUR "weatherStartHr"
#define KEY_SHOW_24_HOUR_TIME "show24Hr"
#define KEY_MAX_RTC_CORRECTION_FACTOR "rtcCorrection"
#define KEY_2_NTP_SYNCS_PER_DAY "twoNtpSyncs"

#define DEFERRED_REQUEST_TIMEOUT 30000

ConfigurationClass Config;

ConfigurationClass::~ConfigurationClass()
{
    _prefs.end();
    if (_httpServer) {
        delete _httpServer;
    }
    if (_dnsServer) {
        delete _dnsServer;
    }
}

void ConfigurationClass::begin()
{
    _prefs.begin("portalcalendar");
    pinMode(PD_PIN, PD_PIN_STATE == HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);
}

void ConfigurationClass::reset()
{
    _prefs.clear();
}

void ConfigurationClass::startConfigServer()
{
    const String hostname = getHostname();
    log_i("Starting configuration server with hostname %s", hostname.c_str());
    WiFi.setHostname(hostname.c_str());

    #ifdef DEV_WEBSERVER
    log_i("Configuration server is in dev mode");

    WiFi.mode(WIFI_STA);
    connectToWifi(DEV_WEBSERVER_WIFI_SSID, DEV_WEBSERVER_WIFI_PASS);

    if (WiFi.waitForConnectResult() != WL_CONNECTED) {
        log_e("Failed to connect to WiFi");
        while (true) {
            yield();
        }
    }
    #else
    WiFi.softAPsetHostname(hostname.c_str());
    WiFi.softAPConfig(
        IPAddress(192, 168, 255, 1), // AP IP
        IPAddress(192, 168, 255, 1), // Gateway
        IPAddress(255, 255, 255, 0)  // Netmask
    );
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_AP_STA);

    #ifdef AP_PASS
    char apPassword[] = AP_PASS;
    #else
    char apPassword[9];
    sprintf(&apPassword[0], "%08u", esp_random() % 99999999);
    #endif // AP_PASS

    WiFi.softAP(AP_SSID, &apPassword[0]);

    connectToSavedWifi();
    #endif // DEV_WEBSERVER

    log_i("Starting DNS server");
    _dnsServer = new DNSServer();
    _dnsServer->setTTL(0);
    _dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
    if (!_dnsServer->start(53, hostname + ".local", WiFi.softAPIP())) {
        log_e("Failed to start DNS server");
    }

    log_i("Starting mDNS server");
    if (mdns_init()) {
        log_e("Failed to start mDNS server");
    } else {
        mdns_hostname_set(hostname.c_str());
        mdns_instance_name_set(hostname.c_str());
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }

    _httpServer = new AsyncWebServer(80);
    _deferredRequestQueue = xQueueCreate(20, sizeof(std::function<void(void)>));
    bool shutdown = false;

    on("/wifi/scan", HTTP_GET, [&](AsyncWebServerRequest *request) {
        log_i("GET /wifi/scan");
        deferRequest(request, [request] {
            if (WiFi.scanComplete() == WIFI_SCAN_FAILED) {
                WiFi.scanNetworks();
            }
            while (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
                yield();
            }

            int count = WiFi.scanComplete();
            if (count == WIFI_SCAN_FAILED) {
                return request->send(HTTP_INTERNAL_SERVER_ERROR);
            }

            AsyncJsonResponse *response = new AsyncJsonResponse(true, 8192);
            JsonArray networks = response->getRoot();
            for (int i = 0; i < count; ++i) {
                JsonObject network = networks.createNestedObject();
                network["ssid"] = WiFi.SSID(i);
                network["rssi"] = WiFi.RSSI(i);
                network["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
            }
            response->setLength();
            request->send(response);
            WiFi.scanDelete();
        });
    });

    on("/wifi/interface", HTTP_GET, [&](AsyncWebServerRequest *request) {
        log_i("GET /wifi/interface");
        AsyncJsonResponse *response = new AsyncJsonResponse(false, 512);
        JsonObject root = response->getRoot();

        root["mac"] = WiFi.macAddress();
        root["hostname"] = WiFi.getHostname();

        response->setLength();
        request->send(response);
    });

    on("/wifi/interface", HTTP_PATCH, [&](AsyncWebServerRequest *request) {
        log_i("PATCH /wifi/interface");
        String hostname = request->getParam("hostname", true)->value();
        if (!hostname || hostname.length() > 63) {
            return request->send(HTTP_BAD_REQUEST);
        }
        
        for (char c : hostname) {
            if (!isalnum(c) && c != '-' && c != '_') {
                return request->send(HTTP_BAD_REQUEST);
            }
        }

        _prefs.putString(KEY_HOSTNAME, hostname);

        request->send(HTTP_OK);
    });

    on("/wifi", HTTP_GET, [&](AsyncWebServerRequest *request) {
        log_i("GET /wifi");
        AsyncJsonResponse *response = new AsyncJsonResponse();
        JsonObject root = response->getRoot();

        bool connected = WiFi.isConnected();
        root["connected"] = connected;

        if (connected) {
            root["ssid"] = WiFi.SSID();
            root["ip"] = WiFi.localIP();
            root["rssi"] = WiFi.RSSI();
            root["gateway"] = WiFi.gatewayIP();
            root["mask"] = WiFi.subnetMask();
            root["dns0"] = WiFi.dnsIP(0);
            root["dns1"] = WiFi.dnsIP(1);
            root["inUse"] = !isApRequest(request);
        } else {
            root["ssid"] = getWifiSsid();
        }

        response->setLength();
        request->send(response);
    });

    on("/wifi", HTTP_POST, [&](AsyncWebServerRequest *request) {
        log_i("POST /wifi");
        deferRequest(request, [request, this] {
            String ssid = request->getParam("ssid", true)->value();
            String password = request->getParam("password", true)->value();
            log_i("Connecting to wifi '%s'", ssid.c_str());
            if (!ssid.length() || ssid.length() > 32 || (password.length() && (password.length() < 8 || password.length() > 64))) {
                return request->send(HTTP_BAD_REQUEST);
            }
            #ifdef DEV_WEBSERVER
            wl_status_t status = ssid.equals(WiFi.SSID()) ? WL_CONNECTED : WL_CONNECT_FAILED;
            #else
            wl_status_t status = connectToWifi(ssid, password);
            #endif // DEV_WEBSERVER
            log_i("Connection result: %u", status);
            if (status == WL_CONNECTED) {
                _prefs.putString(KEY_WIFI_SSID, ssid);
                _prefs.putString(KEY_WIFI_PASS, password);

                AsyncJsonResponse *response = new AsyncJsonResponse();
                JsonObject root = response->getRoot();

                root["connected"] = true;
                root["ssid"] = WiFi.SSID();
                root["ip"] = WiFi.localIP();
                root["rssi"] = WiFi.RSSI();
                root["gateway"] = WiFi.gatewayIP();
                root["mask"] = WiFi.subnetMask();
                root["dns0"] = WiFi.dnsIP(0);
                root["dns1"] = WiFi.dnsIP(1);
                root["inUse"] = !isApRequest(request);
                response->setLength();
                request->send(response);
            } else if (status == WL_NO_SSID_AVAIL) {
                connectToSavedWifi();
                request->send(HTTP_NOT_FOUND);
            } else {
                connectToSavedWifi();
                request->send(HTTP_UNAUTHORIZED);
            }
        });
    });

    on("/wifi", HTTP_DELETE, [&](AsyncWebServerRequest *request) {
        log_i("DELETE /wifi");
        
        _prefs.remove(KEY_WIFI_SSID);
        _prefs.remove(KEY_WIFI_PASS);
        if (WiFi.isConnected()) {
            WiFi.disconnect(false, true);
        }

        request->send(HTTP_OK);
    });

    on("/locales", HTTP_GET, [&](AsyncWebServerRequest *request) {
        log_i("GET /locales");
        AsyncJsonResponse *response = new AsyncJsonResponse(false, 1024);
        JsonObject root = response->getRoot();

        for (Locale locale: LOCALES) {
            root[locale.code] = locale.name;
        }

        response->setLength();
        request->send(response);
    });

    on("/preferences", HTTP_GET, [&](AsyncWebServerRequest *request) {
        log_i("GET /preferences");
        AsyncJsonResponse *response = new AsyncJsonResponse(false, 4096);
        JsonObject root = response->getRoot();
        root[KEY_SHOW_DAY] = getShowDay();
        root[KEY_SHOW_MONTH] = getShowMonth();
        root[KEY_SHOW_YEAR] = getShowYear();
        root[KEY_TIMEZONE_NAME] = getTimezoneName();
        root[KEY_LOCALE] = getLocale();
        root[KEY_PRIMARY_NTP_SERVER] = getPrimaryNtpServer();
        root[KEY_SECONDARY_NTP_SERVER] = getSecondaryNtpServer();
        root[KEY_PRIMARY_TIMEZONED_SERVER] = getPrimaryTimezonedServer();
        root[KEY_SECONDARY_TIMEZONED_SERVER] = getSecondaryTimezonedServer();
        root[KEY_WEATHER_ENABLED] = getWeatherEnabled();
        root[KEY_OWM_API_KEY] = getOwmApiKey();
        root[KEY_WEATHER_LOCATION_NAME] = getWeatherLocationName();
        root[KEY_WEATHER_LOCATION_LATITUDE] = getWeatherLocationLatitude();
        root[KEY_WEATHER_LOCATION_LONGITUDE] = getWeatherLocationLongitude();
        root[KEY_WEATHER_DISPLAY_TYPE] = static_cast<uint8_t>(getWeatherDisplayType());
        root[KEY_WEATHER_SECONDARY_INFO] = static_cast<uint8_t>(getWeatherSecondaryInfo());
        root[KEY_WEATHER_UNITS] = static_cast<uint8_t>(getWeatherUnits());
        root[KEY_WEATHER_START_HOUR] = getWeatherStartHour();
        root[KEY_SHOW_24_HOUR_TIME] = getShow24HourTime();
        root[KEY_2_NTP_SYNCS_PER_DAY] = getTwoNtpSyncsPerDay();
        root[KEY_MAX_RTC_CORRECTION_FACTOR] = getMaxRtcCorrectionFactor();
        response->setLength();
        request->send(response);
    });

    on("/preferences", HTTP_PATCH, 4096, [&](AsyncWebServerRequest *request, JsonVariant& body) {
        log_i("PATCH /preferences");

        prefs_putJsonBool(body, KEY_SHOW_DAY);
        prefs_putJsonBool(body, KEY_SHOW_MONTH);
        prefs_putJsonBool(body, KEY_SHOW_YEAR);
        prefs_putJsonString(body, KEY_TIMEZONE_NAME);
        prefs_putJsonString(body, KEY_LOCALE, 2, 5);
        prefs_putJsonString(body, KEY_PRIMARY_NTP_SERVER);
        prefs_putJsonString(body, KEY_SECONDARY_NTP_SERVER);
        prefs_putJsonString(body, KEY_PRIMARY_TIMEZONED_SERVER);
        prefs_putJsonString(body, KEY_SECONDARY_TIMEZONED_SERVER);
        prefs_putJsonBool(body, KEY_WEATHER_ENABLED);
        prefs_putJsonString(body, KEY_OWM_API_KEY, 32, 32);
        prefs_putJsonString(body, KEY_WEATHER_LOCATION_NAME);
        prefs_putJsonFloat(body, KEY_WEATHER_LOCATION_LATITUDE, -90, 90);
        prefs_putJsonFloat(body, KEY_WEATHER_LOCATION_LONGITUDE, -180, 180);
        prefs_putJsonEnum(body, KEY_WEATHER_DISPLAY_TYPE, {
            WeatherDisplayType::FORECAST_5_DAY,
            WeatherDisplayType::FORECAST_12_HOUR,
        });
        prefs_putJsonEnum(body, KEY_WEATHER_SECONDARY_INFO, {
            WeatherSecondaryInfo::POP,
            WeatherSecondaryInfo::HUMIDITY,
        });
        prefs_putJsonEnum(body, KEY_WEATHER_UNITS, {
            WeatherUnits::IMPERIAL,
            WeatherUnits::METRIC,
        });
        prefs_putJsonUChar(body, KEY_WEATHER_START_HOUR, 0, 23);
        prefs_putJsonBool(body, KEY_SHOW_24_HOUR_TIME);
        prefs_putJsonBool(body, KEY_2_NTP_SYNCS_PER_DAY);
        prefs_putJsonFloat(body, KEY_MAX_RTC_CORRECTION_FACTOR, 0, 1);

        _wasSaved = true;
        request->send(HTTP_OK);
    });

    on("/weather/test", HTTP_GET, [&](AsyncWebServerRequest *request) {
        log_i("GET /weather/test");
        deferRequest(request, [request] {
            String key = request->hasParam("appid") ? request->getParam("appid")->value() : "";
            if (key.length() != 32) {
                return request->send(HTTP_BAD_REQUEST);
            }
            if (!WiFi.isConnected()) {
                return request->send(HTTP_NETWORK_AUTHENTICATION_REQUIRED);
            }

            OwmResult result = testApiKey(key);

            if (result == OwmResult::SUCCESS) {
                request->send(HTTP_OK);
            } else if (result == OwmResult::INVALID_LOCATION) {
                request->send(HTTP_NOT_FOUND);
            } else if (result == OwmResult::INVALID_API_KEY) {
                request->send(HTTP_UNAUTHORIZED);
            } else {
                request->send(HTTP_BAD_GATEWAY);
            }
        });
    });

    on("/weather/location", HTTP_GET, [&](AsyncWebServerRequest *request) {
        log_i("GET /weather/location");
        deferRequest(request, [request] {
            String key = request->hasParam("appid") ? request->getParam("appid")->value() : "";
            String location = request->hasParam("q") ? request->getParam("q")->value() : "";
            if (key.length() != 32  || !location.length() || location.length() > 100) {
                return request->send(HTTP_BAD_REQUEST);
            }
            if (!WiFi.isConnected()) {
                return request->send(HTTP_NETWORK_AUTHENTICATION_REQUIRED);
            }

            OwmLocation result = queryLocation(location, key);

            if (result.result == OwmResult::SUCCESS) {
                AsyncJsonResponse *response = new AsyncJsonResponse();
                JsonObject root = response->getRoot();
                root["lat"] = result.lat;
                root["lon"] = result.lon;
                root["name"] = result.name;
                response->setLength();
                request->send(response);
            } else if (result.result == OwmResult::INVALID_LOCATION) {
                request->send(HTTP_NOT_FOUND);
            } else if (result.result == OwmResult::INVALID_API_KEY) {
                request->send(HTTP_UNAUTHORIZED);
            } else {
                request->send(HTTP_BAD_GATEWAY);
            }
        });
    });

    on("/ntp/test", HTTP_GET, [&](AsyncWebServerRequest *request) {
        log_i("GET /ntp/test");
        deferRequest(request, [request] {
            String server = request->hasParam("server") ? request->getParam("server")->value() : "";
            if (!server.length()) {
                return request->send(HTTP_BAD_REQUEST);
            }
            if (!WiFi.isConnected()) {
                return request->send(HTTP_NETWORK_AUTHENTICATION_REQUIRED);
            }

            if (syncNtp({ server }, true)) {
                request->send(HTTP_OK);
            } else {
                request->send(HTTP_BAD_GATEWAY);
            }
        });
    });

    on("/timezoned/lookup", HTTP_GET, [&](AsyncWebServerRequest *request) {
        log_i("GET /timezoned/lookup");
        deferRequest(request, [request] {
            String server = request->hasParam("server") ? request->getParam("server")->value() : "";
            String timezone = request->hasParam("timezone") ? request->getParam("timezone")->value() : "";
            if (!server.length() || !timezone.length()) {
                return request->send(HTTP_BAD_REQUEST);
            }
            if (!WiFi.isConnected()) {
                return request->send(HTTP_NETWORK_AUTHENTICATION_REQUIRED);
            }

            String tz;
            TimezonedResult result = getPosixTz({ server }, timezone, tz);

            if (result == TimezonedResult::Ok) {
                request->send(HTTP_OK, "text/plain", tz);
            } else if (result == TimezonedResult::TzNotFound) {
                request->send(HTTP_NOT_FOUND);
            } else {
                request->send(HTTP_BAD_GATEWAY);
            }
        });
    });

    on("/shutdown", HTTP_POST, [&](AsyncWebServerRequest *request) {
        log_i("POST /shutdown");
        request->send(HTTP_OK);
        shutdown = true;
    });

    on("/", HTTP_GET, [&](AsyncWebServerRequest *request) {
        log_i("GET http://%s/", request->host().c_str());
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", &INDEX_HTML_DATA[0], sizeof(INDEX_HTML_DATA));
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    #ifdef DEV_WEBSERVER
    _httpServer->onNotFound([](AsyncWebServerRequest *request) {
        if (request->method() == HTTP_OPTIONS) {
            log_i("OPTIONS %s", request->url().c_str());
            request->send(HTTP_OK);
        } else {
            log_i("Not found: %s %s", request->methodToString(), request->url().c_str());
            request->send(HTTP_NOT_FOUND);
        }
    });

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "*");
    #endif // DEV_WEBSERVER

    #ifdef DEV_WEBSERVER
    log_i("DEV Webserver ready at %s", WiFi.localIP().toString().c_str());
    Display.showDevWebserverScreen(WiFi.SSID(), WiFi.localIP());
    #else
    log_i("Webserver ready at %s, %s, %s", WiFi.softAPSSID().c_str(), WiFi.softAPIP().toString().c_str(), apPassword);
    Display.showConfigServerScreen(
        WiFi.softAPSSID(),
        apPassword,
        hostname,
        WiFi.isConnected() ? WiFi.SSID() : String()
    );
    #endif
    log_i("Starting webserver");
    _httpServer->begin();
    std::function<void(void)> request;

    while (isOnUsbPower() && !shutdown) {
        // Process DNS requests
        _dnsServer->processNextRequest();
        // Process deferred request handlers
        if (uxQueueMessagesWaiting(_deferredRequestQueue) && xQueueReceive(_deferredRequestQueue, &request, 0)) {
            request();
        }
        yield();
    }

    log_i("Shutting down webserver...");

    delay(100);

    _dnsServer->stop();
    delete _dnsServer;
    _dnsServer = nullptr;

    _httpServer->end();
    delete _httpServer;
    _httpServer = nullptr;

    vQueueDelete(_deferredRequestQueue);
    mdns_free();

    log_i("Webserver has stopped");
}

/**
 * Enqueues a request handler to be run on the loop task (the main task that arduino runs on with idle priority).
 * This must be done if the request could take longer than a few seconds, since AsyncTCP handlers run on a higher
 * priority task and will trip the watchdog if they take too long.
 */
void ConfigurationClass::deferRequest(AsyncWebServerRequest *request, std::function<void(void)> handler)
{
    // Set a longer timeout on this request since it could take a while
    request->client()->setRxTimeout(DEFERRED_REQUEST_TIMEOUT);
    if (xQueueSend(_deferredRequestQueue, &handler, (TickType_t)1000 * portTICK_PERIOD_MS) != pdPASS) {
        // Request queue is already full
        request->send(HTTP_INTERNAL_SERVER_ERROR);
    }
}

wl_status_t ConfigurationClass::connectToWifi(String ssid, String password)
{
    if (WiFi.isConnected() && ssid.equals(WiFi.SSID())) {
        return WL_CONNECTED;
    }

    wl_status_t status = WL_DISCONNECTED;
    unsigned int start = millis();
    for (unsigned int attempts = 1, elapsed = 0;
        attempts <= 5 && elapsed < WIFI_CONNECTION_TIMEOUT_MS && status != WL_CONNECTED;
        ++attempts, elapsed = millis() - start
    ) {
        log_i("Connecting to Wi-Fi network '%s' attempt %u...", ssid.c_str(), attempts);
        WiFi.begin(ssid.c_str(), password.isEmpty() ? nullptr : password.c_str());
        status = static_cast<wl_status_t>(WiFi.waitForConnectResult(WIFI_CONNECTION_TIMEOUT_MS - elapsed));
    }
    if (status == WL_CONNECTED) {
        log_i("Connected to '%s' at %s after %ums", ssid.c_str(), WiFi.localIP().toString().c_str(), millis() - start);
    } else {
        log_e("Failed to connect to '%s'", ssid.c_str());
    }
    return status;
}

bool ConfigurationClass::connectToSavedWifi()
{
    String ssid = getWifiSsid();
    if (!ssid.isEmpty()) {
        return connectToWifi(ssid, getWifiPass()) == WL_CONNECTED;
    }
    log_i("No saved Wi-Fi network to connect to");
    return false;
}

String ConfigurationClass::getWifiSsid() { return _prefs.getString(KEY_WIFI_SSID); }
String ConfigurationClass::getWifiPass() { return _prefs.getString(KEY_WIFI_PASS); }
String ConfigurationClass::getHostname() { return _prefs.getString(KEY_HOSTNAME, DEFAULT_HOSTNAME); }
bool ConfigurationClass::getShowDay() { return _prefs.getBool(KEY_SHOW_DAY, DEFAULT_SHOW_DAY); }
bool ConfigurationClass::getShowMonth() { return _prefs.getBool(KEY_SHOW_MONTH, DEFAULT_SHOW_MONTH); }
bool ConfigurationClass::getShowYear() { return _prefs.getBool(KEY_SHOW_YEAR, DEFAULT_SHOW_YEAR); }
String ConfigurationClass::getTimezoneName() { return _prefs.getString(KEY_TIMEZONE_NAME); }
String ConfigurationClass::getLocale() { return _prefs.getString(KEY_LOCALE, DEFAULT_LOCALE); }
String ConfigurationClass::getPrimaryNtpServer() { return _prefs.getString(KEY_PRIMARY_NTP_SERVER, DEFAULT_PRIMARY_NTP_SERVER); }
String ConfigurationClass::getSecondaryNtpServer() { return _prefs.getString(KEY_SECONDARY_NTP_SERVER, DEFAULT_SECONDARY_NTP_SERVER); }
String ConfigurationClass::getPrimaryTimezonedServer() { return _prefs.getString(KEY_PRIMARY_TIMEZONED_SERVER, DEFAULT_PRIMARY_TIMEZONED_SERVER); }
String ConfigurationClass::getSecondaryTimezonedServer() { return _prefs.getString(KEY_SECONDARY_TIMEZONED_SERVER, DEFAULT_SECONDARY_TIMEZONED_SERVER); }
bool ConfigurationClass::getWeatherEnabled() { return _prefs.getBool(KEY_WEATHER_ENABLED, false); }
String ConfigurationClass::getOwmApiKey() { return _prefs.getString(KEY_OWM_API_KEY); }
String ConfigurationClass::getWeatherLocationName() { return _prefs.getString(KEY_WEATHER_LOCATION_NAME); }
float ConfigurationClass::getWeatherLocationLatitude() { return _prefs.getFloat(KEY_WEATHER_LOCATION_LATITUDE, 0); }
float ConfigurationClass::getWeatherLocationLongitude() { return _prefs.getFloat(KEY_WEATHER_LOCATION_LONGITUDE, 0); }
WeatherDisplayType ConfigurationClass::getWeatherDisplayType() { return prefs_getEnum(KEY_WEATHER_DISPLAY_TYPE, DEFAULT_WEATHER_DISPLAY_TYPE); }
WeatherUnits ConfigurationClass::getWeatherUnits() { return prefs_getEnum(KEY_WEATHER_UNITS, DEFAULT_WEATHER_UNITS); }
WeatherSecondaryInfo ConfigurationClass::getWeatherSecondaryInfo() { return prefs_getEnum(KEY_WEATHER_SECONDARY_INFO, DEFAULT_WEATHER_SECONDARY_INFO); }
uint8_t ConfigurationClass::getWeatherStartHour() { return _prefs.getUChar(KEY_WEATHER_START_HOUR, DEFAULT_WEATHER_START_HOUR); }
bool ConfigurationClass::getShow24HourTime() { return _prefs.getBool(KEY_SHOW_24_HOUR_TIME, DEFAULT_USE_24H_TIME); }
float ConfigurationClass::getMaxRtcCorrectionFactor() { return _prefs.getFloat(KEY_MAX_RTC_CORRECTION_FACTOR, DEFAULT_MAX_RTC_CORRECTION_FACTOR); }
bool ConfigurationClass::getTwoNtpSyncsPerDay() { return _prefs.getFloat(KEY_2_NTP_SYNCS_PER_DAY, DEFAULT_2_NTP_SYNCS_PER_DAY); }

template <typename T>T ConfigurationClass::prefs_getEnum(const char* key, T defaultValue)
{
    return static_cast<T>(_prefs.getUChar(key, static_cast<uint8_t>(defaultValue)));
}

void ConfigurationClass::prefs_putJsonFloat(const JsonObject& json, const char* key, float min, float max)
{
    JsonVariant value = json[key];
    if (value.is<float>()) {
        float v = value.as<float>();
        if (v >= min && v <= max) {
            _prefs.putFloat(key, v);
        } else {
            log_w("Value %f is out of range (%f-%f) for %s", v, min, max, key);
        }
    } else if (!value.isNull()) {
        log_w("Value for %s cannot be converte to float", key);
    }
}

void ConfigurationClass::prefs_putJsonUChar(const JsonObject& json, const char* key, uint8_t min, uint8_t max)
{
    JsonVariant value = json[key];
    if (value.is<uint8_t>()) {
        uint8_t v = value.as<uint8_t>();
        if (v >= min && v <= max) {
            _prefs.putUChar(key, v);
        } else {
            log_w("Value %u is out of range (%u-%u) for %s", v, min, max, key);
        }
    } else if (!value.isNull()) {
        log_w("Value for %s cannot be converted to uchar", key);
    }
}

void ConfigurationClass::prefs_putJsonString(const JsonObject& json, const char* key, unsigned int minLength, unsigned int maxLength)
{
    JsonVariant value = json[key];
    if (value.is<String>()) {
        String v = value.as<String>();
        if (v.length() >= minLength && v.length() <= maxLength) {
            _prefs.putString(key, value.as<const char*>());
        } else {
            log_w("String '%s' (%u) does not meet the length constraint (%u-%u) for %s", v.c_str(), v.length(), minLength, maxLength, key);
        }
    } else if (!value.isNull()) {
        log_w("Value for %s cannot be converted to string", key);
    }
}

void ConfigurationClass::prefs_putJsonBool(const JsonObject& json, const char* key)
{
    JsonVariant value = json[key];
    if (value.is<bool>()) {
        _prefs.putBool(key, value.as<bool>());
    } else if (!value.isNull()) {
        log_w("Value for %s cannot be converted to bool", key);
    }
}

template<typename T> void ConfigurationClass::prefs_putJsonEnum(const JsonObject& json, const char* key, std::initializer_list<T> enumValues)
{
    JsonVariant value = json[key];
    if (value.is<uint8_t>()) {
        uint8_t v = value.as<uint8_t>();
        for (T enumValue : enumValues) {
            if (enumValue == static_cast<T>(v)) {
                _prefs.putUChar(key, v);
                return;
            }
        }
        log_w("Value %u is not valid for %s", v, key);
    } else if (!value.isNull()) {
        log_w("Value for %s cannot be converted to uchar", key);
    }
}

bool ConfigurationClass::isConfigured()
{
    return !(
        getWifiSsid().isEmpty() ||
        getTimezoneName().isEmpty() ||
        getPrimaryNtpServer().isEmpty() ||
        getPrimaryTimezonedServer().isEmpty() || false
        // (getWeatherEnabled() && (
        //     getOwmApiKey().isEmpty() ||
        //     getWeatherLocationLatitude() == 0.0 ||
        //     getWeatherLocationLongitude() == 0.0
        // ))
    );
}

bool ConfigurationClass::isApRequest(AsyncWebServerRequest *request)
{
    // TODO is there a better way to determine this?
    return request->client()->localIP() == WiFi.softAPIP();
}

AsyncCallbackWebHandler& ConfigurationClass::on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest)
{
    return _httpServer->on(uri, method, onRequest);
}

AsyncCallbackJsonWebHandler& ConfigurationClass::on(const char* uri, WebRequestMethodComposite method, size_t maxJsonBufferSize, ArJsonRequestHandlerFunction onRequest)
{
    AsyncCallbackJsonWebHandler* handler = new AsyncCallbackJsonWebHandler(uri, onRequest, maxJsonBufferSize);
    handler->setMethod(method);
    _httpServer->addHandler(handler);
    return *handler;
}
