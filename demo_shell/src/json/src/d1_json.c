
#include "d1_json.h"
#include <cJSON_os.h>

#define D1_JSON_MESSAGE_BUFFER_SIZE 2048
#define XTR_BUFF_SIZE 1024
#define MAX_RF_OBJS 8
#define MAX_NUM_EV_OBJS 1

#define TOP_LEVEL_MID "manu_nrf9160_test\0"
#define TOP_LEVEL_MK "US\0"
#define TOP_LEVEL_P 5
#define TOP_LEVEL_T 1
#define TOP_LEVEL_B 35

#define EV_MOD "ModelA\0"
#define EV_HWR "0.1.0\0"
#define EV_FWR "0.8.1\0"
#define EV_TZ "Europe/Rome\0"

// define C structs for every part of the final JSON message
// define C structs that combine those structs/arrays for each level of the JSON message.



// define zephyr-JSON-macro structs for each of the structs defined above.



char json_message_buffer[D1_JSON_MESSAGE_BUFFER_SIZE];
char* json_message_buffer2;


char* json_batterylevel(float battLevel) 
{
    cJSON_Init();

    cJSON *topLevel = cJSON_CreateObject();
    cJSON_AddNumberToObject(topLevel, "P", TOP_LEVEL_P);
    cJSON_AddStringToObject(topLevel, "MID", TOP_LEVEL_MID);
    cJSON_AddStringToObject(topLevel, "MK", TOP_LEVEL_MK);
    cJSON_AddNumberToObject(topLevel, "T", TOP_LEVEL_T);
    cJSON_AddNumberToObject(topLevel, "B", TOP_LEVEL_B);
    cJSON *evArray = cJSON_AddArrayToObject(topLevel, "EV");
    cJSON *ev0 = cJSON_CreateObject(); // EV is an array of objects
        cJSON_AddStringToObject(ev0, "MOD", EV_MOD);
        cJSON_AddStringToObject(ev0, "HWR", EV_HWR);
        cJSON_AddStringToObject(ev0, "FWR", EV_FWR);
        cJSON_AddStringToObject(ev0, "TZ", EV_TZ);
        cJSON *xtrObj = cJSON_AddObjectToObject(ev0, "XTR");
            cJSON_AddNumberToObject(xtrObj, "BATT", battLevel);  // replace with real batt
    cJSON_AddItemToArray(evArray, ev0);

    json_message_buffer2 = cJSON_Print(topLevel);
    cJSON_Delete(topLevel);

    // THIS MUST BE FREEd BY CALLER
    return json_message_buffer2;
}


char* json_wifi_data(wifi_arr_t* data, float battLevel)
{
    cJSON_Init();

    cJSON* wifiObjs[MAX_WIFI_OBJS];

    cJSON *topLevel = cJSON_CreateObject();
    cJSON_AddNumberToObject(topLevel, "P", TOP_LEVEL_P);
    cJSON_AddStringToObject(topLevel, "MID", TOP_LEVEL_MID);
    cJSON_AddStringToObject(topLevel, "MK", TOP_LEVEL_MK);
    cJSON_AddNumberToObject(topLevel, "T", TOP_LEVEL_T);
    cJSON_AddNumberToObject(topLevel, "B", TOP_LEVEL_B);
    cJSON *evArray = cJSON_AddArrayToObject(topLevel, "EV");
    cJSON *ev0 = cJSON_CreateObject(); // EV is an array of objects
        cJSON_AddStringToObject(ev0, "MOD", EV_MOD);
        cJSON_AddStringToObject(ev0, "HWR", EV_HWR);
        cJSON_AddStringToObject(ev0, "FWR", EV_FWR);
        cJSON_AddStringToObject(ev0, "TZ", EV_TZ);
        cJSON *xtrObj = cJSON_AddObjectToObject(ev0, "XTR");
            if (battLevel > 0) {
                cJSON_AddNumberToObject(xtrObj, "BATT", battLevel);
            }
            // RF is not clear in doc
            cJSON *wifiArray = cJSON_AddArrayToObject(xtrObj, "WIFI");
                for(int i=0;i<data->count;i++) {
                    wifiObjs[i] = cJSON_CreateObject(); 
                    cJSON_AddStringToObject(wifiObjs[i], "ssid", data->wifi[i].ssid);
                    cJSON_AddNumberToObject(wifiObjs[i], "rssi", data->wifi[i].rssi);
                    cJSON_AddNumberToObject(wifiObjs[i], "channel", data->wifi[i].channel);
                    cJSON_AddItemToArray(wifiArray, wifiObjs[i]);
                }
    cJSON_AddItemToArray(evArray, ev0);

    //json_message_buffer2 = cJSON_Print(topLevel);
    cJSON_PrintPreallocated(topLevel, json_message_buffer, D1_JSON_MESSAGE_BUFFER_SIZE, false);
    cJSON_Delete(topLevel);

    // THIS MUST BE FREEd BY CALLER
    return json_message_buffer;
}

