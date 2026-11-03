#include "api.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "commonFS.h"
#include <Preferences.h>
#include "debug.h"
#include "scale.h"
#include "nfc.h"
#include "config.h"
#include <WiFi.h>
#include "display.h"
#include "lang.h"
#include "wlan.h"
#include "weight_delivery.h"

volatile filamanApiStateType filamanApiState = API_IDLE;
bool filamanConnected = false;

struct ApiRequest {
    FilamanApiRequestType type;
    int id1;
    int id2;
    String str1;
    String str2;
    float val;
    bool bool1; // success
    String str3; // error message
    float remainingWeight; // remaining weight from rfid-result
    bool active = false;
};

#define MAX_API_QUEUE 10
ApiRequest apiQueue[MAX_API_QUEUE];
SemaphoreHandle_t queueMutex;

// Weight measurements have their own queue: they stay queued until the
// backend acknowledges them (see lib/WeightDelivery).
static weightdelivery::DeliveryQueue weightQueue;
static SemaphoreHandle_t weightMutex = nullptr;

void saveFilamanConfig() {
    Preferences preferences;
    preferences.begin(NVS_NAMESPACE_API, false);
    preferences.putString(NVS_KEY_FILAMAN_URL, filamanUrl);
    preferences.putString(NVS_KEY_FILAMAN_TOKEN, filamanToken);
    preferences.putBool(NVS_KEY_FILAMAN_REGISTERED, filamanRegistered);
    preferences.end();
}

void loadFilamanConfig() {
    Preferences preferences;
    preferences.begin(NVS_NAMESPACE_API, true);
    filamanUrl = preferences.getString(NVS_KEY_FILAMAN_URL, "");
    filamanToken = preferences.getString(NVS_KEY_FILAMAN_TOKEN, "");
    filamanRegistered = preferences.getBool(NVS_KEY_FILAMAN_REGISTERED, false);
    preferences.end();
}

bool checkFilamanRegistration() {
    return filamanRegistered && filamanToken.length() > 0;
}

bool registerDevice(const String& deviceCode) {
    if (filamanUrl.length() == 0) return false;
    HTTPClient http;
    http.setTimeout(5000);
    http.begin(filamanUrl + "/api/v1/devices/register");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Device-Code", deviceCode);
    int httpCode = http.POST("{}");
    if (httpCode == 200 || httpCode == 201) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
            if (doc["token"].is<String>()) {
                filamanToken = doc["token"].as<String>();
                filamanRegistered = true;
                saveFilamanConfig();
                http.end();
                return true;
            }
        }
    }
    http.end();
    return false;
}

bool sendHeartbeat() {
    if (!checkFilamanRegistration() || !networkConnected()) return false;
    HTTPClient http;
    http.setTimeout(10000);  // 10s statt 3s - wichtig für instabile Verbindungen
    http.setReuse(true);     // Keep-Alive aktivieren für bessere Performance
    http.begin(filamanUrl + "/api/v1/devices/heartbeat");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Device " + filamanToken);
    http.addHeader("Connection", "keep-alive");  // Explizit Keep-Alive anfordern
    JsonDocument doc;
    doc["ip_address"] = getLocalIP();
    String payload;
    serializeJson(doc, payload);
    int httpCode = http.POST(payload);
    filamanConnected = (httpCode == 200);
    http.end();
    return filamanConnected;
}

// Heartbeat mit Retry-Logik für mehr Stabilität
bool sendHeartbeatWithRetry(int maxRetries = 2) {
    for (int attempt = 0; attempt <= maxRetries; attempt++) {
        if (sendHeartbeat()) {
            return true;
        }
        if (attempt < maxRetries) {
            Serial.printf("Heartbeat failed, retry %d/%d...\n", attempt + 1, maxRetries);
            vTaskDelay(pdMS_TO_TICKS(1000));  // 1s warten vor Retry
        }
    }
    Serial.println("Heartbeat failed after all retries");
    return false;
}

// Posts one weight measurement. Returns the HTTP status code, or a negative
// HTTPClient error code. No display output; the caller decides what to show.
int postWeight(int spoolId, const String& tagUuid, float measuredWeight, String* response) {
    Serial.printf("postWeight: sending to API - spoolId=%d, tagUuid=%s, weight=%.1f\n", spoolId, tagUuid.c_str(), measuredWeight);
    HTTPClient http;
    http.setTimeout(10000);  // 10s Timeout
    http.setReuse(true);
    http.begin(filamanUrl + "/api/v1/devices/scale/weight");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Device " + filamanToken);
    http.addHeader("Connection", "keep-alive");
    JsonDocument doc;
    // Only add spool_id if it's > 0 (for NTAG tags with spool ID)
    // For Bambu tags (spoolId == 0), only send tag_uuid
    if (spoolId > 0) doc["spool_id"] = spoolId;
    // Always add tag_uuid if available (this is what we want for Bambu tags)
    if (tagUuid.length() > 0) doc["tag_uuid"] = tagUuid;
    doc["measured_weight_g"] = measuredWeight;
    String payload;
    serializeJson(doc, payload);
    Serial.printf("API payload: %s\n", payload.c_str());
    int httpCode = http.POST(payload);
    Serial.printf("API response code: %d\n", httpCode);
    if (response && httpCode >= 200 && httpCode < 300) *response = http.getString();
    http.end();
    return httpCode;
}

static weightdelivery::SendOutcome classifyWeightResponse(int httpCode) {
    using weightdelivery::SendOutcome;
    if (httpCode >= 200 && httpCode < 300) return SendOutcome::Success;
    // Connection errors, timeouts, rate limiting and server errors may succeed later.
    if (httpCode < 0 || httpCode == 408 || httpCode == 429 || httpCode >= 500) return SendOutcome::Transient;
    // 400, 401, 403, 404, 422, ...: retrying the same request will not help.
    return SendOutcome::Permanent;
}

static void showWeightApiError() {
    oledShowProgressBar(1, 1, tr(STR_FAILURE), tr(STR_API_ERROR));
    oledSetPriority(DISPLAY_PRIORITY_WARNING, 2000);
    vTaskDelay(pdMS_TO_TICKS(2000));
    oledClearPriority();
}

// Sends the next due weight measurement, if any, and records the result.
static void processWeightQueue() {
    using namespace weightdelivery;
    if (!weightMutex) return;

    Measurement m;
    bool hasWork = false;
    if (xSemaphoreTake(weightMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        hasWork = weightQueue.beginNext(millis(), m);
        xSemaphoreGive(weightMutex);
    }
    if (!hasWork) return;

    SendOutcome outcome;
    int httpCode = 0;
    String response;
    if (!checkFilamanRegistration()) {
        outcome = SendOutcome::Permanent;
    } else if (!networkConnected()) {
        outcome = SendOutcome::NoNetwork;
    } else {
        filamanApiState = API_TRANSMITTING;
        httpCode = postWeight(m.spoolId, String(m.tagUuid.c_str()), m.weight, &response);
        filamanApiState = API_IDLE;
        outcome = classifyWeightResponse(httpCode);
    }

    // Always record the result, otherwise the entry would stay "in flight".
    xSemaphoreTake(weightMutex, portMAX_DELAY);
    Completion c = weightQueue.complete(m.seq, outcome, millis());
    xSemaphoreGive(weightMutex);

    switch (c.status) {
        case DeliveryStatus::Confirmed: {
            Serial.printf("Weight #%u confirmed (HTTP %d)\n", (unsigned)m.seq, httpCode);
            JsonDocument responseDoc;
            DeserializationError error = deserializeJson(responseDoc, response);
            if (!error && responseDoc["remaining_weight_g"].is<float>()) {
                int remaining = (int)responseDoc["remaining_weight_g"].as<float>();
                oledShowRemainingWeight(remaining);
                oledSetPriority(DISPLAY_PRIORITY_ACTION, 3000);
                vTaskDelay(pdMS_TO_TICKS(3000));
                oledClearPriority();
            }
            break;
        }
        case DeliveryStatus::Pending:
            Serial.printf("Weight #%u not delivered (HTTP %d, attempt %u), retry in %u ms\n",
                          (unsigned)m.seq, httpCode, (unsigned)c.attempts, (unsigned)c.retryInMs);
            // Show the error once, not on every retry.
            if (c.attempts == 1) showWeightApiError();
            break;
        case DeliveryStatus::Failed:
            Serial.printf("Weight #%u failed permanently (HTTP %d, registered=%d)\n",
                          (unsigned)m.seq, httpCode, (int)checkFilamanRegistration());
            showWeightApiError();
            break;
        case DeliveryStatus::Superseded:
            Serial.printf("Weight #%u superseded by a newer measurement\n", (unsigned)m.seq);
            break;
        default:
            break;
    }
}

bool sendLocation(int spoolId, String spoolTagUuid, int locationId, String locationTagUuid) {
    if (!checkFilamanRegistration() || !networkConnected()) return false;
    HTTPClient http;
    http.setTimeout(10000);  // 10s Timeout
    http.setReuse(true);
    http.begin(filamanUrl + "/api/v1/devices/scale/locate");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Device " + filamanToken);
    http.addHeader("Connection", "keep-alive");
    JsonDocument doc;
    if (spoolId > 0) doc["spool_id"] = spoolId;
    if (spoolTagUuid.length() > 0) doc["spool_tag_uuid"] = spoolTagUuid;
    if (locationId > 0) doc["location_id"] = locationId;
    if (locationTagUuid.length() > 0) doc["location_tag_uuid"] = locationTagUuid;
    String payload;
    serializeJson(doc, payload);
    int httpCode = http.POST(payload);
    http.end();
    return (httpCode == 200);
}

bool sendRfidResult(String tagUuid, int spoolId, int locationId, bool success, String errorMessage, float remainingWeight) {
    if (!checkFilamanRegistration() || !networkConnected()) return false;
    HTTPClient http;
    http.setTimeout(10000);  // 10s Timeout
    http.setReuse(true);
    http.begin(filamanUrl + "/api/v1/devices/rfid-result");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Device " + filamanToken);
    http.addHeader("Connection", "keep-alive");

    JsonDocument doc;
    doc["success"] = success;
    if (tagUuid.length() > 0) doc["tag_uuid"] = tagUuid;
    if (spoolId > 0) doc["spool_id"] = spoolId;
    if (locationId > 0) doc["location_id"] = locationId;
    if (errorMessage.length() > 0) doc["error_message"] = errorMessage;
    if (remainingWeight > 0) doc["remaining_weight_g"] = remainingWeight;

    String payload;
    serializeJson(doc, payload);
    int httpCode = http.POST(payload);
    http.end();
    return (httpCode == 200);
}

bool sendTagData(String tagJson) {
    if (!checkFilamanRegistration() || !networkConnected()) return false;
    HTTPClient http;
    http.setTimeout(10000);
    http.setReuse(true);
    http.begin(filamanUrl + "/api/v1/devices/tag-data");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Device " + filamanToken);
    http.addHeader("Connection", "keep-alive");
    JsonDocument doc;
    doc["tag_json"] = tagJson;
    String payload;
    serializeJson(doc, payload);
    int httpCode = http.POST(payload);
    http.end();
    Serial.printf("sendTagData: HTTP %d\n", httpCode);
    return (httpCode == 200);
}

void filamanApiTask(void* pvParameters) {
    for (;;) {
        ApiRequest req;
        bool hasReq = false;

        if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for(int i=0; i<MAX_API_QUEUE; i++) {
                if(apiQueue[i].active) {
                    req = apiQueue[i];
                    apiQueue[i].active = false;
                    hasReq = true;
                    break;
                }
            }
            xSemaphoreGive(queueMutex);
        }

        if (hasReq) {
            filamanApiState = API_TRANSMITTING;
            switch (req.type) {
                case API_REQUEST_REGISTER: {
                    // URL übernehmen und persistieren, dann blockierend
                    // registrieren - hier unkritisch, da eigener Task.
                    if (req.str2.length() > 0) filamanUrl = req.str2;
                    saveFilamanConfig();
                    bool ok = registerDevice(req.str1);
                    Serial.printf("registerDevice: %s\n", ok ? "success" : "failed");
                    sendRegisterResult(ok);
                    break;
                }
                case API_REQUEST_HEARTBEAT: sendHeartbeatWithRetry(2); break;
                case API_REQUEST_LOCATE: sendLocation(req.id1, req.str1, req.id2, req.str2); break;
                case API_REQUEST_RFID_RESULT: sendRfidResult(req.str1, req.id1, req.id2, req.bool1, req.str3, req.remainingWeight); break;
                case API_REQUEST_TAG_DATA: sendTagData(req.str1); break;
                default: break;
            }
            filamanApiState = API_IDLE;
        }
        processWeightQueue();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Reiht die Registrierung ein, statt sie im AsyncTCP-Kontext auszuführen.
// str2 = FilaMan-URL, str1 = Device-Code.
bool registerDeviceAsync(const String& url, const String& deviceCode) {
    bool queued = false;
    if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for(int i=0; i<MAX_API_QUEUE; i++) if(!apiQueue[i].active) {
            apiQueue[i].type = API_REQUEST_REGISTER;
            apiQueue[i].id1 = 0;
            apiQueue[i].id2 = 0;
            apiQueue[i].str1 = deviceCode;
            apiQueue[i].str2 = url;
            apiQueue[i].val = 0.0f;
            apiQueue[i].active = true;
            queued = true;
            break;
        }
        xSemaphoreGive(queueMutex);
    }
    // Ohne Queue-Platz kaeme nie ein "registerResult" - der Aufrufer muss
    // dem Frontend dann sofort einen Fehler zurueckgeben.
    return queued;
}

void sendHeartbeatAsync() {
    if (!checkFilamanRegistration()) return;
    if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        // Prevent duplicate heartbeats
        for(int i=0; i<MAX_API_QUEUE; i++) if(apiQueue[i].active && apiQueue[i].type == API_REQUEST_HEARTBEAT) {
            xSemaphoreGive(queueMutex);
            return;
        }
        for(int i=0; i<MAX_API_QUEUE; i++) if(!apiQueue[i].active) {
            apiQueue[i].type = API_REQUEST_HEARTBEAT;
            apiQueue[i].id1 = 0;
            apiQueue[i].id2 = 0;
            apiQueue[i].str1 = "";
            apiQueue[i].str2 = "";
            apiQueue[i].val = 0.0f;
            apiQueue[i].active = true;
            break;
        }
        xSemaphoreGive(queueMutex);
    }
}

WeightEnqueueResult sendWeightAsync(int spoolId, const String& tagUuid, float weight, uint32_t* ticket) {
    using weightdelivery::EnqueueResult;
    if (ticket) *ticket = 0;
    Serial.printf("sendWeightAsync: spoolId=%d, tagUuid=%s, weight=%.1f\n", spoolId, tagUuid.c_str(), weight);
    if (!checkFilamanRegistration()) {
        Serial.println("ERROR: Not registered with FilaMan, cannot send weight");
        return WEIGHT_NOT_REGISTERED;
    }
    if (!weightMutex || xSemaphoreTake(weightMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        Serial.println("Weight queue busy");
        return WEIGHT_BUSY;
    }
    EnqueueResult r = weightQueue.enqueue(spoolId, std::string(tagUuid.c_str()), weight, millis(), ticket);
    size_t pending = weightQueue.pendingCount();
    xSemaphoreGive(weightMutex);

    switch (r) {
        case EnqueueResult::Queued:
            Serial.printf("Weight queued for API (%u pending)\n", (unsigned)pending);
            return WEIGHT_QUEUED;
        case EnqueueResult::Replaced:
            Serial.printf("Weight replaced older pending value (%u pending)\n", (unsigned)pending);
            return WEIGHT_REPLACED;
        case EnqueueResult::Full:
            Serial.println("Weight queue full");
            return WEIGHT_BUSY;
        default:
            Serial.println("ERROR: Invalid weight or no spool id / tag UUID, cannot send");
            return WEIGHT_INVALID;
    }
}

WeightDeliveryStatus getWeightDeliveryStatus(uint32_t ticket) {
    using weightdelivery::DeliveryStatus;
    if (!weightMutex || xSemaphoreTake(weightMutex, pdMS_TO_TICKS(50)) != pdTRUE) return WEIGHT_STATUS_UNKNOWN;
    DeliveryStatus st = weightQueue.status(ticket);
    xSemaphoreGive(weightMutex);
    switch (st) {
        case DeliveryStatus::Pending:    return WEIGHT_STATUS_PENDING;
        case DeliveryStatus::InFlight:   return WEIGHT_STATUS_IN_FLIGHT;
        case DeliveryStatus::Confirmed:  return WEIGHT_STATUS_CONFIRMED;
        case DeliveryStatus::Failed:     return WEIGHT_STATUS_FAILED;
        case DeliveryStatus::Superseded: return WEIGHT_STATUS_SUPERSEDED;
        default:                         return WEIGHT_STATUS_UNKNOWN;
    }
}

void sendLocationAsync(int spoolId, String spoolTagUuid, int locationId, String locationTagUuid) {
    if (!checkFilamanRegistration()) return;
    if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for(int i=0; i<MAX_API_QUEUE; i++) if(!apiQueue[i].active) {
            apiQueue[i].type = API_REQUEST_LOCATE;
            apiQueue[i].id1 = spoolId;
            apiQueue[i].id2 = locationId;
            apiQueue[i].str1 = spoolTagUuid;
            apiQueue[i].str2 = locationTagUuid;
            apiQueue[i].val = 0.0f;
            apiQueue[i].active = true;
            break;
        }
        xSemaphoreGive(queueMutex);
    }
}

void sendRfidResultAsync(String tagUuid, int spoolId, int locationId, bool success, String errorMessage, float remainingWeight) {
    if (!checkFilamanRegistration()) return;
    if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for(int i=0; i<MAX_API_QUEUE; i++) if(!apiQueue[i].active) {
            apiQueue[i].type = API_REQUEST_RFID_RESULT;
            apiQueue[i].str1 = tagUuid;
            apiQueue[i].id1 = spoolId;
            apiQueue[i].id2 = locationId;
            apiQueue[i].bool1 = success;
            apiQueue[i].str3 = errorMessage;
            apiQueue[i].remainingWeight = remainingWeight;
            apiQueue[i].active = true;
            break;
        }
        xSemaphoreGive(queueMutex);
    }
}

void sendTagDataAsync(String tagJson) {
    if (!checkFilamanRegistration()) return;
    if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for(int i=0; i<MAX_API_QUEUE; i++) if(!apiQueue[i].active) {
            apiQueue[i].type = API_REQUEST_TAG_DATA;
            apiQueue[i].str1 = tagJson;
            apiQueue[i].id1 = 0;
            apiQueue[i].id2 = 0;
            apiQueue[i].str2 = "";
            apiQueue[i].val = 0.0f;
            apiQueue[i].active = true;
            break;
        }
        xSemaphoreGive(queueMutex);
    }
}

bool initFilaman() {
    oledShowProgressBar(3, NUM_SETUP_STEPS, DISPLAY_BOOT_TEXT, tr(STR_API_INIT));
    loadFilamanConfig();
    queueMutex = xSemaphoreCreateMutex();
    weightMutex = xSemaphoreCreateMutex();
    // Move to Core 1 (Hardware Core) to free up Core 0 for WiFi/Webserver
    // Set priority to 1 (same as Scale/NFC) to ensure fair scheduling
    xTaskCreatePinnedToCore(filamanApiTask, "FilaManApi", 6144, NULL, 1, NULL, 1);
    if (checkFilamanRegistration()) sendHeartbeatAsync();
    return true;
}
