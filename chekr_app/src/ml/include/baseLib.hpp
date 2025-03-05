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
enum ActivityType
{
    other = 0,
    drink,
    eat,
    walk,
    trot,
    canter,
    gallop,
};

enum PetSize
{
    size_giant = 0,
    size_large,
    size_medium,
    size_small,
    size_toy,
};

struct InferResult
{
    ModelType modelType;
    ActivityType curntActivity;
    uint64_t imu_timestamp;
    float repetition;
    bool jointHealth; //
};

struct IMUData
{
    float imuValues[6];
    uint64_t timestamp;
};

bool initiate(void (*handleInferResult)(InferResult), int rep_call_interval, PetSize pSize);
bool processIMUData(IMUData imuData);
ActivityType InferActivity(std::vector<float> &buffer, float *out_buf);
bool InferJointHealth(std::vector<float> &buffer, long startTimeStamp);
float InferRepetition(std::vector<float> &buffer, int repCnt, ActivityType currentActvty, PetSize pSize);
const char *getVersion();

#endif