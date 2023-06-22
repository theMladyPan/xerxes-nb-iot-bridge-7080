#ifndef __CONFIG_H__
#define __CONFIG_H__

// define the board type
#define isNBIOT true

// define preffered connection mode
// 2 Auto // 13 GSM only // 38 LTE only
#define CONNECTION_MODE 2  // Auto

const char API_URL_WELCOME[] = "/api/v1/welcome";
const char API_URL_INSERT_ONE[] = "/api/v1/insert_one";
const char server[]   = "xerxes-web-dev.eu-west-2.elasticbeanstalk.com";
const int port       = 80;
const int rand_pin = 33;  // to sample noise from the ADC for the random number generator


// convenient constants
constexpr unsigned long uS_TO_S_FACTOR = 1000000ULL;  // Conversion factor for micro seconds to seconds
// 3 min = Time ESP32 will go to sleep (in seconds) must be higher than MAX_CYCLE_TIME
constexpr int TIME_TO_SLEEP = 60; 
// 1:40 min = max time to wait for a response from the server
constexpr int MAX_CYCLE_TIME = 59; 


#endif // __CONFIG_H__