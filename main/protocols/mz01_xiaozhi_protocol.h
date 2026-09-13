#ifndef _MZ01_XIAOZHI_PROTOCOL_H_
#define _MZ01_XIAOZHI_PROTOCOL_H_

#include "protocol.h"
#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <memory>
#include <string>

#define MZ01_XWS_SERVER_HELLO_EVENT (1 << 0)

// Xiaozhi WebSocket protocol (hello / listen / abort / iot / mcp + binary opus),
// ported from xiaozhi-esp32 main/protocols/websocket_protocol.cc. This fork's
// built-in WebsocketProtocol speaks the Coze event protocol, which the
// self-hosted xiaozhi server (xiaozhi-server-go) does not implement — it made
// the MZ01 connect and do MCP but never stream voice audio.
class Mz01XiaozhiProtocol : public Protocol {
public:
    Mz01XiaozhiProtocol() = default;
    ~Mz01XiaozhiProtocol() override;

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;
    bool IsAudioCanEnterSleepMode() const override;
    bool SendAudio(const AudioStreamPacket& packet) override;

    // Xiaozhi-protocol framings; the base class impls send Coze-shaped JSON
    // or are commented out in this fork, so all are overridden here.
    void SendAbortSpeaking(AbortReason reason) override;
    void SendWakeWordDetected(const std::string& wake_word) override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendIotDescriptors(const std::string& descriptors) override;
    void SendIotStates(const std::string& states) override;
    void SendMcpMessage(const std::string& payload) override;

protected:
    bool SendText(const std::string& text) override;

private:
    std::unique_ptr<WebSocket> websocket_;
    EventGroupHandle_t event_group_handle_ = nullptr;
    int version_ = 1;
    bool intentional_close_ = false;  // suppress the net-error alert on planned close

    std::string GetHelloMessage();
    void ParseServerHello(const cJSON* root);
};

#endif // _MZ01_XIAOZHI_PROTOCOL_H_
