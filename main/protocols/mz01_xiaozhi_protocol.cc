#include "mz01_xiaozhi_protocol.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>

#define TAG "Mz01Xws"

Mz01XiaozhiProtocol::~Mz01XiaozhiProtocol() {
    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
        event_group_handle_ = nullptr;
    }
}

bool Mz01XiaozhiProtocol::Start() {
    // Connection is opened on demand by OpenAudioChannel()
    return true;
}

bool Mz01XiaozhiProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }
    return websocket_->Send(text);
}

bool Mz01XiaozhiProtocol::SendAudio(const AudioStreamPacket& packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected() || packet.payload.empty()) {
        return false;
    }
    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet.payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet.timestamp);
        bp2->payload_size = htonl(packet.payload.size());
        memcpy(bp2->payload, packet.payload.data(), packet.payload.size());
        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet.payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet.payload.size());
        memcpy(bp3->payload, packet.payload.data(), packet.payload.size());
        return websocket_->Send(serialized.data(), serialized.size(), true);
    }
    return websocket_->Send(packet.payload.data(), packet.payload.size(), true);
}

void Mz01XiaozhiProtocol::SendAbortSpeaking(AbortReason reason) {
    // Same 1s server-audio ignore window as the base class expects
    abort_speaking_timestamp_ = std::chrono::steady_clock::now();
    abort_speaking_recorded_ = true;
    std::string reason_str = (reason == kAbortReasonWakeWordDetected) ? "wake_word_detected" : "none";
    SendText("{\"session_id\":\"" + session_id_ + "\",\"type\":\"abort\",\"reason\":\"" + reason_str + "\"}");
}

void Mz01XiaozhiProtocol::SendWakeWordDetected(const std::string& wake_word) {
    SendText("{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + wake_word + "\"}");
}

void Mz01XiaozhiProtocol::SendStartListening(ListeningMode mode) {
    const char* mode_str = "manual";
    if (mode == kListeningModeRealtime) {
        mode_str = "realtime";
    } else if (mode == kListeningModeAutoStop) {
        mode_str = "auto";
    }
    SendText("{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"" + mode_str + "\"}");
}

void Mz01XiaozhiProtocol::SendStopListening() {
    SendText("{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"stop\"}");
}

void Mz01XiaozhiProtocol::SendIotDescriptors(const std::string& descriptors) {
    SendText("{\"session_id\":\"" + session_id_ + "\",\"type\":\"iot\",\"descriptors\":" + descriptors + "}");
}

void Mz01XiaozhiProtocol::SendIotStates(const std::string& states) {
    SendText("{\"session_id\":\"" + session_id_ + "\",\"type\":\"iot\",\"states\":" + states + "}");
}

void Mz01XiaozhiProtocol::SendMcpMessage(const std::string& payload) {
    SendText("{\"session_id\":\"" + session_id_ + "\",\"type\":\"mcp\",\"payload\":" + payload + "}");
}

bool Mz01XiaozhiProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

bool Mz01XiaozhiProtocol::IsAudioCanEnterSleepMode() const {
    return !IsAudioChannelOpened();
}

void Mz01XiaozhiProtocol::CloseAudioChannel() {
    intentional_close_ = true;
    if (websocket_) {
        // Graceful WebSocket close (sends the CLOSE frame) so both the server
        // and the Application see a clean shutdown instead of a TCP reset.
        websocket_->Close();
        websocket_.reset();
    }
}

bool Mz01XiaozhiProtocol::OpenAudioChannel() {
    if (event_group_handle_ == nullptr) {
        event_group_handle_ = xEventGroupCreate();
    }
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }
    if (url.empty()) {
        ESP_LOGE(TAG, "No websocket url configured (NVS namespace 'websocket')");
        return false;
    }

    error_occurred_ = false;
    intentional_close_ = false;

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    if (!token.empty()) {
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            if (on_incoming_audio_ != nullptr) {
                if (version_ == 2 && len >= (int)sizeof(BinaryProtocol2)) {
                    BinaryProtocol2* bp2 = (BinaryProtocol2*)data;
                    bp2->version = ntohs(bp2->version);
                    bp2->type = ntohs(bp2->type);
                    bp2->timestamp = ntohl(bp2->timestamp);
                    bp2->payload_size = ntohl(bp2->payload_size);
                    on_incoming_audio_(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = bp2->timestamp,
                        .payload = std::vector<uint8_t>((uint8_t*)bp2->payload, (uint8_t*)bp2->payload + bp2->payload_size)});
                } else if (version_ == 3 && len >= (int)sizeof(BinaryProtocol3)) {
                    BinaryProtocol3* bp3 = (BinaryProtocol3*)data;
                    uint16_t payload_size = ntohs(bp3->payload_size);
                    on_incoming_audio_(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)bp3->payload, (uint8_t*)bp3->payload + payload_size)});
                } else {
                    on_incoming_audio_(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)});
                }
            } else {
                static bool cb_missing_logged = false;
                if (!cb_missing_logged) {
                    ESP_LOGE(TAG, "on_incoming_audio_ callback NOT set!");
                    cb_missing_logged = true;
                }
            }
        } else {
            auto root = cJSON_ParseWithLength(data, len);
            if (root == nullptr) {
                ESP_LOGE(TAG, "Invalid JSON: %.*s", (int)len, data);
                return;
            }
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else if (strcmp(type->valuestring, "mcp") == 0) {
                    // Server wraps jsonrpc in {"type":"mcp","payload":{...}};
                    // McpServer::ParseMessage expects the bare jsonrpc object.
                    auto payload = cJSON_GetObjectItem(root, "payload");
                    if (payload != nullptr) {
                        McpServer::GetInstance().ParseMessage(payload);
                    }
                } else if (on_incoming_json_ != nullptr) {
                    on_incoming_json_(root);
                }
            } else {
                ESP_LOGE(TAG, "Missing message type: %.*s", (int)len, data);
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this](bool is_clean) {
        if (intentional_close_) {
            // Planned close (BOOT while listening): report clean so the
            // Application skips its net-error alert ("Jaringan bermasalah").
            ESP_LOGI(TAG, "WS closed by request");
            if (on_audio_channel_closed_ != nullptr) {
                on_audio_channel_closed_(true);
            }
            return;
        }
        ESP_LOGW(TAG, "WS disconnected (clean=%d)", is_clean);
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_(is_clean);
        }
    });

    ESP_LOGI(TAG, "Connecting to %s (version %d)", url.c_str(), version_);
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server");
        SetError("SERVER_NOT_CONNECTED");
        return false;
    }

    if (!SendText(GetHelloMessage())) {
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, MZ01_XWS_SERVER_HELLO_EVENT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & MZ01_XWS_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError("SERVER_TIMEOUT");
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

std::string Mz01XiaozhiProtocol::GetHelloMessage() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", 60);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void Mz01XiaozhiProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport != nullptr && cJSON_IsString(transport) &&
        strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }
    ESP_LOGI(TAG, "Server hello: opus/%d/%dms", server_sample_rate_, server_frame_duration_);

    if (event_group_handle_ != nullptr) {
        xEventGroupSetBits(event_group_handle_, MZ01_XWS_SERVER_HELLO_EVENT);
    }
}
