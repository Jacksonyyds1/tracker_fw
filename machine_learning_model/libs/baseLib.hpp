#ifndef BASE_LIB_H_
#define BASE_LIB_H_

#include <stdint.h>
#include <iostream>
#include <vector>
#include <queue>

#define PRINT_DEBUG_ML_LOGS

#define ROLLING_WINDOW_FOR_ACTVTY 3 // TODO: every 3 sec
#define IMU_SAMPLE_FREQUENCY 15

enum ModelType
{
    Activity = 1,
    Repetition,
    JointHealth
};
// Nida sent sequence 0_others, 1_drink, 2_eat, 3_walk, 4_trot, 5_canter, 6_gallop
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

enum PetSize
{
    size_giant = 0,
    size_large,
    size_medium,
    size_small,
    size_toy,
};

// struct InferResult
// {
//     ModelType modelType;
//     ActivityType curntActivity;
//     uint64_t imu_timestamp;
//     float repetition;
//     bool jointHealth; //
// };

struct InferResult
{
ActivityType pred_class;
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

bool initiate(void (*handleInferResult)(InferResult));
bool processIMUData(IMUData imuData);
ActivityType InferActivity(std::vector<float> &buffer, float *out_buf);
bool InferJointHealth(std::vector<float> &buffer, long startTimeStamp);
float InferRepetition(std::vector<float> &buffer, int repCnt, ActivityType currentActvty, PetSize pSize);
const char *getVersion();
bool resetActivityWindowCounter();

#endif