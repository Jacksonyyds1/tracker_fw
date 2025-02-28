#pragma once

#include <string.h>
#include <stdlib.h>
#include <stdio.h>


int handleFotaMQTTMessage(char *topic, int topicLength, char *message, int messageLength);
void fotaSendCheckForUpdate();
int handleFotaDownloadHTTPsMessage(char *url, int sec_tag);
int fotaCancel();