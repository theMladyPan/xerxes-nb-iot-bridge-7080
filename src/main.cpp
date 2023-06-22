/**
 * @file      MinimalModemNBIOTExample.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2022  Shenzhen Xin Yuan Electronic Technology Co., Ltd
 * @date      2022-09-16
 *
 */
#include <Arduino.h>
#include "XPowersLib.h"
#include "utils.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "power.h"
#include "config.h"
#include "HttpClient.h"
#include "ArduinoJson.h"


#define TINY_GSM_MODEM_SIM7080
#include <TinyGsmClient.h>

XPowersPMU  PMU;

// See all AT commands, if wanted
// #define DUMP_AT_COMMANDS

#define TINY_GSM_RX_BUFFER 1024

#undef USE_GPS // do not use GPS for now

// Set serial for debug console (to the Serial Monitor, default speed 115200)
#define SerialMon Serial
// Set serial for AT commands (to the module)
#define SerialAT Serial1

#ifdef DUMP_AT_COMMANDS
#include <StreamDebugger.h>
StreamDebugger debugger(Serial1, Serial);
TinyGsm        modem(debugger);
#else
TinyGsm        modem(SerialAT);
#endif

TinyGsmClient client(modem, 0);

HttpClient http_client = HttpClient(client, server, port);
DynamicJsonDocument data(1024*4);
auto payload = data["payload"];
auto meta = payload["meta"];
auto power = meta["power"];

String jsonString;

const char *register_info[] = {
    "Not registered, MT is not currently searching an operator to register to.The GPRS service is disabled, the UE is allowed to attach for GPRS if requested by the user.",
    "Registered, home network.",
    "Not registered, but MT is currently trying to attach or searching an operator to register to. The GPRS service is enabled, but an allowable PLMN is currently not available. The UE will start a GPRS attach as soon as an allowable PLMN is available.",
    "Registration denied, The GPRS service is disabled, the UE is not allowed to attach for GPRS if it is requested by the user.",
    "Unknown.",
    "Registered, roaming.",
};

enum {
    MODEM_CATM = 1,
    MODEM_NB_IOT,
    MODEM_CATM_NBIOT,
};

static int64_t cycleStart = 0;

// RTC_DATA_ATTR is used to store data in RTC memory, so it will be retained
// after deep sleep, start with -1 so first boot will be 0
RTC_DATA_ATTR int bootCount = 0;  

static const char* TAG = "Main";


/**
 * @brief disable all power domains except for DC1 - CPU
 * 
 */
void disableAllPower()
{

    // Turn off other unused power domains
    PMU.disableDC2();
    //Turn off modem power
    PMU.disableDC3();
    PMU.disableDC4();
    PMU.disableDC5();
    PMU.disableALDO1();
    PMU.disableALDO2();
    PMU.disableALDO3();
    PMU.disableALDO4();
    //Turn off the power supply for level conversion
    PMU.disableBLDO1();
    //Turn off gps power
    PMU.disableBLDO2();
    PMU.disableCPUSLDO();
    PMU.disableDLDO1();
    PMU.disableDLDO2();

    //Turn off chg led
    PMU.setChargingLedMode(XPOWERS_CHG_LED_OFF);
}


/**
 * @brief Enable minimal power domains for modem and level converter * 
 */
void enableMinimalPower()
{
    // Set the voltage of CPU back to to 3.3V
    PMU.setDC1Voltage(3300);

    //Set the working voltage of the modem, please do not modify the parameters
    PMU.setDC3Voltage(3000);    //SIM7080 Modem main power channel 2700~ 3400V
    PMU.enableDC3();

    //Set the working voltage of the level conversion
    PMU.setBLDO1Voltage(3300);  //SIM7080 Modem level conversion power channel 2700~3400V
    PMU.enableBLDO1();

    #ifdef USE_GPS
    ESP_LOGI(TAG, "Enabling GPS antenna power channel...");
    //Modem GPS Power channel
    PMU.setBLDO2Voltage(3300);
    PMU.enableBLDO2();      //The antenna power must be turned on to use the GPS function
    #endif // USE_GPS

    // TS Pin detection must be disable, otherwise it cannot be charged
    PMU.disableTSPinMeasure();
    PMU.setChargingLedMode(XPOWERS_CHG_LED_ON);
}


/**
 * @brief Get the current cycle time in us
 * 
 * @return uint32_t 
 */
uint32_t currentCycleTimeUs()
{
    return esp_timer_get_time() - cycleStart;    
}


uint32_t remainingCycleTimeUs(uint64_t sleep_for_us = TIME_TO_SLEEP * uS_TO_S_FACTOR)
{
    return sleep_for_us - currentCycleTimeUs();
}


/**
 * @brief Enter deep sleep for a given time
 * 
 * @param sleep_for_us - time to sleep in us
 * @param modem_off - true if modem should be turned off as well
 */
void enterDeepSleep(uint64_t sleep_for_us = TIME_TO_SLEEP * uS_TO_S_FACTOR)
{
    modem.sleepEnable(false);
    modem.poweroff();

    setCpuFrequencyMhz(10); // Set the CPU clock to 10MHz
    PMU.setDC1Voltage(3000); // lower CPU voltage to 3V
    ESP_LOGI(TAG, "CPU freq: %d MHz, voltage: %d mV", getCpuFrequencyMhz(), PMU.getDC1Voltage());
    disableAllPower();

    Wire.end();

    Serial1.end();

    digitalWrite(BOARD_MODEM_PWR_PIN, HIGH);

    ESP_LOGI(TAG, "All systems off, entering deep sleep");

    auto cycleDuration = currentCycleTimeUs();
    ESP_LOGI(TAG, "Cycle duration: %.1fs, sleeping for: %.1fs", (float)(cycleDuration / 1e6), (float)((sleep_for_us-cycleDuration) / 1e6));
    Serial.flush();

    // setup wake up timer to wake up after rest of the cycle duration
    esp_sleep_enable_timer_wakeup(sleep_for_us - currentCycleTimeUs());
    // enter deep sleep 

    esp_deep_sleep_start();
    Serial.println("This will never be printed");
}


/**
 * @brief Callback function for the one-shot timer
 * 
 * @param arg - not used
 */
static void oneshot_timer_callback(void* arg)
{
    ESP_LOGI(TAG, "Deadline reached, entering deep sleep");
    enterDeepSleep();
}


/// @brief Setup deep sleep timer to enter deep sleep in case of failure after MAX_CYCLE_TIME
void enableDeadline()
{
    // Setup deep-sleep timer to enter deep sleep in 60 seconds - in case of failure
    ESP_LOGD(TAG, "Setting up timer to enter deep sleep in %d seconds", MAX_CYCLE_TIME);
    const esp_timer_create_args_t oneshot_timer_args = {
        callback : &oneshot_timer_callback,
        arg : NULL,
        dispatch_method : ESP_TIMER_TASK,
        /* argument specified here will be passed to timer callback function */
        name : "one-shot"
    };
    esp_timer_handle_t oneshot_timer;
    ESP_ERROR_CHECK(esp_timer_create(&oneshot_timer_args, &oneshot_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(oneshot_timer, MAX_CYCLE_TIME * uS_TO_S_FACTOR));
    ESP_LOGD(TAG, "Timer set up! Tik tok, tik tok...");
}


bool welcomeRequest();
bool sendData(String &_jsonString);

void modemRestart()
{
    // Pull down PWRKEY for more than 1 second according to manual requirements
    digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
    delay(100);
    digitalWrite(BOARD_MODEM_PWR_PIN, HIGH);
    delay(1000);
    digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
}

void setup()
{
    setCpuFrequencyMhz(80); //Set the CPU clock to 80MHz
    cycleStart = esp_timer_get_time();
    bool level = false;
    Serial.begin(115200);
    // get the start time of the cycle in us since boot
    bootCount++;
    enableDeadline();

    data["dbname"] = __DB_NAME;
    data["token"] = __API_TOKEN;
    data["uuid"] = __UUID;
    // define colname as first 8 bytes of __UUID
    String strValue(__UUID);  // Convert const char[] to String
    data["colname"] = strValue.substring(0, 8);


    /*********************************
     *  step 1 : Initialize power chip,
     *  turn on modem and gps antenna power channel
    ***********************************/
    ESP_LOGI(TAG, "Initializing power chip...");
    if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL)) {
        ESP_LOGE(TAG, "Failed to initialize power.....");
        while (1) {
            delay(5000);
        }
    }
    enableMinimalPower();
    pinMode(BOARD_MODEM_PWR_PIN, OUTPUT);
    pinMode(BOARD_MODEM_DTR_PIN, OUTPUT);
    pinMode(BOARD_MODEM_RI_PIN, INPUT);
    modemRestart();
    delay(2000);

    ESP_LOGI(TAG, "Boot number: %d", bootCount);
    ESP_LOGI(TAG, "CPU freq: %d MHz, voltage: %d mV", getCpuFrequencyMhz(), PMU.getDC1Voltage());    
    ESP_LOGI(TAG, "Power chip initialized, modem enabled.");

    /*********************************
     * step 2 : start modem
    ***********************************/
    ESP_LOGI(TAG, "Starting modem...");
    Serial1.begin(115200, SERIAL_8N1, BOARD_MODEM_RXD_PIN, BOARD_MODEM_TXD_PIN);


    int retry = 0;
    while (!modem.testAT(1000)) {
        Serial.print(".");
        if (retry++ > 6) {
            modemRestart();
            retry = 0;
            ESP_LOGW(TAG, "Retry start modem.");
        }
    }
    ESP_LOGI(TAG, "Modem start success!");
    

    /*********************************
     * step 3 : Check if the SIM card is inserted
    ***********************************/
    String result ;

    ESP_LOGI(TAG, "Check SIM card status");
    if (modem.getSimStatus() != SIM_READY) {
        ESP_LOGE(TAG, "SIM Card is not insert!!!");
        // restart esp
        return;
    }
    ESP_LOGI(TAG, "SIM Card is ready!");
    
    // enable RF 1 = full functionality
    ESP_LOGI(TAG, "Enable RF");
    modem.sendAT("+CFUN=1");
    if (modem.waitResponse(20000UL) != 1) {
        ESP_LOGE(TAG, "Enable RF Failed!");
    }
    ESP_LOGI(TAG, "Enable RF Success!");
        
    /*********************************
     * step 4 : Set the network mode to NB-IOT
    ***********************************/
    ESP_LOGI(TAG, "Set the network mode to NB-IOT");
    modem.setNetworkMode(2);    //use automatic

    modem.setPreferredMode(MODEM_NB_IOT);

    uint8_t pre = modem.getPreferredMode();

    uint8_t mode = modem.getNetworkMode();

    ESP_LOGI(TAG, "getNetworkMode:%u getPreferredMode:%u", mode, pre);

    PMU.setChargingLedMode(XPOWERS_CHG_LED_BLINK_1HZ);
    // GPRS connection parameters are usually set after network registration
    ESP_LOGI(TAG, "Connecting to: %s as %s", __LTE_APN_NAME, __LTE_APN_USER);
    while (!modem.gprsConnect(__LTE_APN_NAME, __LTE_APN_USER, __LTE_APN_PASS)) 
    {
        ESP_LOGW(TAG, "Not connected. Retrying in 2 seconds...");
        delay(2000);
    }
    ESP_LOGI(TAG, "Connected!");
    
    /*********************************
    * step 5 : Wait for the network registration to succeed
    ***********************************/
    RegStatus s;
    do {
        s = modem.getRegistrationStatus();
        if (s != REG_OK_HOME && s != REG_OK_ROAMING) {
            Serial.print(".");
            PMU.setChargingLedMode(level ? XPOWERS_CHG_LED_ON : XPOWERS_CHG_LED_OFF);
            level ^= 1;
            delay(1000);
        }

    } while (s != REG_OK_HOME && s != REG_OK_ROAMING) ;

    ESP_LOGI(TAG, "Network register info:%s", register_info[s]);


    bool res = modem.isGprsConnected();
    ESP_LOGI(TAG, "GPRS status:%s", res ? "connected" : "not connected");

    String ccid = modem.getSimCCID();
    ESP_LOGI(TAG, "CCID:%s", ccid.c_str());

    String imei = modem.getIMEI();
    ESP_LOGI(TAG, "IMEI:%s", imei.c_str());

    String imsi = modem.getIMSI();
    ESP_LOGI(TAG, "IMSI:%s", imsi.c_str());

    String cop = modem.getOperator();
    ESP_LOGI(TAG, "Operator:%s", cop.c_str());

    IPAddress local = modem.localIP();
    ESP_LOGI(TAG, "Local IP:%s", local.toString().c_str());

    int csq = modem.getSignalQuality();
    ESP_LOGI(TAG, "Signal quality:%d", csq);

    String gsmTime = modem.getGSMDateTime(DATE_FULL);
    ESP_LOGI(TAG, "GSM Time:%s", gsmTime.c_str());
    payload["time"]["gsm"]["local"] = gsmTime;

    if (bootCount % 100 == 1)
    {
        meta["modem"]["CCID"] = ccid;
        meta["modem"]["IMEI"] = imei;
        meta["modem"]["IMSI"] = imsi;
        meta["modem"]["Operator"] = cop;
        meta["modem"]["LocalIP"] = local.toString();
        meta["modem"]["SignalQuality"] = csq;
    }
    meta["bootCount"] = bootCount;
    meta["power"]["battery"]["V"] = PMU.getBattVoltage()/1000.0;
    meta["power"]["battery"]["percent"] = PMU.getBatteryPercent();


    /*********************************
    * step 6 : Send HTTP request
    ***********************************/
    
    // http_client.connectionKeepAlive();  // Currently, this is needed for HTTPS

    serializeJson(data, jsonString);
    ESP_LOGI(TAG, "JSON: %s", jsonString.c_str());
    
}


void loop()
{
    // retry 3 times:
    for(int i=0; i<3; i++)
    {
        if (sendData(jsonString))
        {
            break;
        }
        else
        {
            ESP_LOGE(TAG, "Failed to send data, retrying...");
        }
    }

    http_client.stop();

    // blink PMU led thrice to indicate success
    for(int i=0; i<3; i++)
    {
        PMU.setChargingLedMode(XPOWERS_CHG_LED_ON);
        delay(100);
        PMU.setChargingLedMode(XPOWERS_CHG_LED_OFF);
        delay(100);
    }

    enterDeepSleep();
}

bool sendData(String &_jsonString)
{
    ESP_LOGI(TAG, "Performing HTTPS POST request");
 
    // send JSON
    int err = http_client.post(String(API_URL_INSERT_ONE), "application/json", _jsonString);
    ESP_LOGI(TAG, "POST request sent");

    if (err != 0) {
        ESP_LOGE(TAG, "HTTP POST request failed, errno: %d", err);
    }
    else
    {
        ESP_LOGI(TAG, "HTTP POST request success");
        int responseCode = http_client.responseStatusCode();
        ESP_LOGI(TAG, "Response #: %d", responseCode);
        ESP_LOGI(TAG, "Response body: %s", http_client.responseBody().c_str());
        if (responseCode == 201)  // 201 = Created
        {
            return true;
        }
    }   
    return false;
}

bool welcomeRequest()
{
    ESP_LOGI(TAG, "Performing HTTPS GET request");
 
    // send JSON
    int err = http_client.get(String(API_URL_WELCOME));
    ESP_LOGI(TAG, "GET request sent");

    if (err != 0) {
        ESP_LOGE(TAG, "HTTP GET request failed, errno: %d", err);
        return false;
    }
    else
    {
        ESP_LOGI(TAG, "HTTP GET request success");
        ESP_LOGI(TAG, "Response #: %d", http_client.responseStatusCode());
        ESP_LOGI(TAG, "Response body: %s", http_client.responseBody().c_str());
        return true;
    }   
}