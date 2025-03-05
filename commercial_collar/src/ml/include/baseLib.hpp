#ifndef BASE_LIB_H_
#define BASE_LIB_H_

#include <stdint.h>
#include <iostream>
#include <vector>
#include <queue>

#define ML_LIBRARY_VERSION_3_1_0    0
#define ML_LIBRARY_VERSION_3_1_1    1
#define ML_LIBRARY_VERSION_3_2_0    2
#define ML_LIBRARY_VERSION_3_2_1    3
#define ML_LIBRARY_VERSION_3_3_1    4

#define ML_LIBRARY_VERSION ML_LIBRARY_VERSION_3_3_1

#define PRINT_DEBUG_ML_LOGS

#define ROLLING_WINDOW_FOR_ACTVTY 3
#define IMU_SAMPLE_FREQUENCY 15
#define ACTIVITY_CLASS_COUNT      8
#define IMU_AXIS_COUNT            6

// Nida sent sequence:
//'0_others', '1_drink', '2_eat', '3_walk', '4_trot', '5_canter', '6_gallop', "7_resting"
enum ActivityType
{
    other = 0,
    drink,
    eat,
    walk,
    trot,
    canter,
    gallop,
    resting,
};
#define ACTIVITY_STR(a) (a==0?"OTHER":(a==1?"DRINK":(a==2?"EAT":(a==3?"WALK":a==4?"TROT":(a==5?"CANTER":(a==6?"GALLOP":"RESTING"))))))


struct InferResult
{
    ActivityType pred_class;
    float pred_probability;
    float pred_reps;
    uint64_t start_timestamp;
    uint64_t end_timestamp;
    float accel_mag_mean_val;
    float gyro_mag_mean_val;
    float accel_mag_sd_val;
    float gyro_mag_sd_val;
};

struct IMUData
{
    float imuValues[6];
    uint64_t timestamp;
};

bool initiate(void (*handleInferResult)(InferResult), bool debugLog);
bool processIMUData(IMUData imuData);
const char *getVersion();
bool resetActivityWindowCounter();

#endif
