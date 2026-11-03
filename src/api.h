#ifndef API_H
#define API_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "website.h"
#include "display.h"
#include <ArduinoJson.h>

typedef enum {
    API_IDLE,
    API_TRANSMITTING
} filamanApiStateType;

typedef enum {
    API_REQUEST_REGISTER,
    API_REQUEST_HEARTBEAT,
    API_REQUEST_LOCATE,
    API_REQUEST_RFID_RESULT,
    API_REQUEST_TAG_DATA
} FilamanApiRequestType;

// Result of handing a weight measurement to the delivery queue.
typedef enum {
    WEIGHT_QUEUED,          // accepted, will be sent (and retried) in the background
    WEIGHT_REPLACED,        // accepted, replaced an older pending value for the same spool
    WEIGHT_NOT_REGISTERED,  // device not registered with FilaMan, nothing to do
    WEIGHT_INVALID,         // weight <= 0 or no spool id / tag UUID, nothing to do
    WEIGHT_BUSY             // queue full or locked, try again later
} WeightEnqueueResult;

// Delivery state of a queued weight (see getWeightDeliveryStatus).
typedef enum {
    WEIGHT_STATUS_UNKNOWN,
    WEIGHT_STATUS_PENDING,
    WEIGHT_STATUS_IN_FLIGHT,
    WEIGHT_STATUS_CONFIRMED,
    WEIGHT_STATUS_FAILED,
    WEIGHT_STATUS_SUPERSEDED
} WeightDeliveryStatus;

extern volatile filamanApiStateType filamanApiState;
extern bool filamanConnected;

// FilaMan API functions
bool initFilaman();
bool registerDevice(const String& deviceCode);
bool registerDeviceAsync(const String& url, const String& deviceCode);
void sendHeartbeatAsync();
// ticket (optional) receives an id for getWeightDeliveryStatus(), 0 if not accepted.
WeightEnqueueResult sendWeightAsync(int spoolId, const String& tagUuid, float weight, uint32_t* ticket = nullptr);
WeightDeliveryStatus getWeightDeliveryStatus(uint32_t ticket);
void sendLocationAsync(int spoolId, String spoolTagUuid, int locationId, String locationTagUuid);
void sendRfidResultAsync(String tagUuid, int spoolId, int locationId, bool success, String errorMessage, float remainingWeight = 0);
void sendTagDataAsync(String tagJson);

// Internal blocking functions (used by async task)
bool sendHeartbeat();
int postWeight(int spoolId, const String& tagUuid, float weight, String* response = nullptr);
bool sendLocation(int spoolId, String spoolTagUuid, int locationId, String locationTagUuid);
bool sendRfidResult(String tagUuid, int spoolId, int locationId, bool success, String errorMessage, float remainingWeight = 0);

// Helper functions
void saveFilamanConfig();
void loadFilamanConfig();
bool checkFilamanRegistration();

#endif
