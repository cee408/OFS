#include "OFS_TCode.h"
#include "OFS_Util.h"

#include "imgui.h"
#include "imgui_internal.h"

#include "SDL.h"
#include "OFS_im3d.h"


#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "glm/gtc/type_ptr.hpp"

bool TCodePlayer::openPort(const char* name) noexcept
{
    if (port != nullptr) {
        c_serial_free(port);
        port = nullptr;
    }

    if (c_serial_new(&port, NULL) < 0) {
        LOG_ERROR("ERROR: Unable to create new serial port\n");
        port = nullptr;
        status = -1;
        return false;
    }

    /*
     * The port name is the device to open(/dev/ttyS0 on Linux,
     * COM1 on Windows)
     */
    if (c_serial_set_port_name(port, name) < 0) {
        LOG_ERROR("ERROR: can't set port name\n");
        return false;
    }

    c_serial_set_baud_rate(port, CSERIAL_BAUD_115200);
    c_serial_set_data_bits(port, CSERIAL_BITS_8);
    c_serial_set_stop_bits(port, CSERIAL_STOP_BITS_1);
    c_serial_set_parity(port, CSERIAL_PARITY_NONE);
    c_serial_set_flow_control(port, CSERIAL_FLOW_NONE);

    LOGF_DEBUG("Baud rate is %d\n", c_serial_get_baud_rate(port));

    /*
    * We want to get all line flags when they change
    */
    c_serial_set_serial_line_change_flags(port, CSERIAL_LINE_FLAG_ALL);
    status = c_serial_open(port);
    if (status < 0) {
        LOG_ERROR("ERROR: Can't open serial port\n");
        return false;
    }
    return true;
}

//void TCodePlayer::tick() noexcept
//{
//    if (status < 0) return;
//
//    data_length = sizeof(data);
//    if (status >= 0) {
//        status = c_serial_read_data(port, data, &data_length, &lines);
//        if (status < 0) {
//            LOG_ERROR("Failed to read from serial port.");
//            return;
//        }
//    }
//
//    LOGF_DEBUG("Got %d bytes of data\n", data_length);
//    
//    for (int x = 0; x < data_length; x++) {
//        LOGF_DEBUG("    0x%02X (ASCII: %c)\n", data[x], data[x]);
//    }
//    LOGF_DEBUG("Serial line state: CD: %d CTS: %d DSR: %d DTR: %d RTS: %d RI: %d\n",
//        lines.cd,
//        lines.cts,
//        lines.dsr,
//        lines.dtr,
//        lines.rts,
//        lines.ri);
//
//    status = c_serial_write_data(port, data, &data_length);
//    if (status < 0) {
//        LOG_ERROR("Failed to write to serial port.");
//        return;
//    }
//}

TCodePlayer::TCodePlayer()
{
	c_serial_set_global_log_function(
		[](const char* logger_name, const struct SL_LogLocation* location, const enum SL_LogLevel level, const char* log_string) {
			LOGF_INFO("%s: %s",logger_name, log_string);
		});
}

TCodePlayer::~TCodePlayer()
{
    stop();
}

static struct TCodeThreadData {
    bool running = false;
    bool requestStop = false;
    
    SDL_atomic_t scriptTimeMs = { 0 };
    
    float speed = 1.f;
    int tickrate = 60;
    int32_t delay = 0;

    TCodePlayer* player = nullptr;
    TCodeChannels* channel = nullptr;
    TCodeProducer* producer = nullptr;
} Thread;

void TCodePlayer::DrawWindow(bool* open) noexcept
{
    if (!*open) return;

    ImGui::Begin("T-Code", open, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Combo("Port", &current_port, [](void* data, int idx, const char** out_text) -> bool {
        const char** port_list = (const char**)data;
        *out_text = port_list[idx];
        return true;
        }, port_list, port_count);
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        if (port_list != nullptr) {
            c_serial_free_serial_ports_list(port_list);
        }
        port_list = c_serial_get_serial_ports_list();
        int x = 0;
        LOG_DEBUG("Available serial ports:\n");
        while (port_list[x] != NULL) {
            LOGF_DEBUG("%s\n", port_list[x]);
            x++;
        }
        port_count = x;
    }

    if (port_list != nullptr
        && port_list[0] != nullptr
        && current_port < port_count
        && port_list[current_port] != nullptr) {
        if (ImGui::Button("Open port", ImVec2(-1.f, 0.f))) {
            openPort(port_list[current_port]);
        }
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    char buf[16];
    ImGui::TextUnformatted("Linear");
    auto& l0 = tcode.Get(TChannel::L0);
    stbsp_snprintf(buf, sizeof(buf), "%s Limit", l0.Id);
    ImGui::SliderInt2(buf, l0.limits.data(), TCodeChannel::MinChannelValue, TCodeChannel::MaxChannelValue);

    ImGui::Separator();

    ImGui::TextUnformatted("Rotation");
    auto rotationGui = [&](TCodeChannel& c) {
        stbsp_snprintf(buf, sizeof(buf), "%s Limit", c.Id);
        ImGui::SliderInt2(buf, c.limits.data(), TCodeChannel::MinChannelValue, TCodeChannel::MaxChannelValue);
    };
    rotationGui(tcode.Get(TChannel::R0));
    rotationGui(tcode.Get(TChannel::R1));
    rotationGui(tcode.Get(TChannel::R2));


    ImGui::InputInt("Delay", &Thread.delay, 10, 10);
    ImGui::SameLine();
    static bool easing = false;
    if (ImGui::Checkbox("Easing", &easing)) {
        TCodeChannel::EasingMode = easing ? TCodeEasing::Cubic : TCodeEasing::None;
    }
    ImGui::SliderInt("Tickrate", &Thread.tickrate, 60, 300, "%d", ImGuiSliderFlags_AlwaysClamp);
    
    if (!Thread.running) {
        if (ImGui::Button("Home", ImVec2(-1, 0))) {
            tcode.reset();
            auto cmd = tcode.GetCommand(0, 1);
            if (cmd != nullptr && status >= 0) {
                int len = strlen(cmd);
                status = c_serial_write_data(port, (void*)cmd, &len);
                if (status < 0) {
                    LOG_ERROR("Failed to write to serial port.");
                }
            }
        }
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
    for (int i = 0; i < tcode.channels.size(); i++) {
        if (IgnoreChannel(static_cast<TChannel>(i))) { ImGui::Spacing(); continue; }
        auto& c = tcode.channels[i];
        auto& p = prod.producers[i];
        ImGui::SliderInt(c.Id, &c.lastTcodeVal,  TCodeChannel::MinChannelValue, TCodeChannel::MaxChannelValue /*c.limits[0], c.limits[1]*/);
        ImGui::SameLine(); ImGui::Text(" -> %s", c.buf);
    }
    ImGui::PopItemFlag();

  	ImGui::End();
}


static int32_t TCodeThread(void* threadData) noexcept {
    TCodeThreadData* data = (TCodeThreadData*)threadData;

    LOG_INFO("T-Code thread started...");

    int startTicks = SDL_GetTicks();
    
    int scriptTimeMs = 0;

    data->producer->sync(SDL_AtomicGet(&Thread.scriptTimeMs));

    while (!data->requestStop) {
        int maxTicks = std::round(1000.f / data->tickrate) - 1;

        int32_t data_scriptTimeMs = SDL_AtomicGet(&Thread.scriptTimeMs);

        int32_t ticks = SDL_GetTicks();
        int32_t currentTimeMs = (((ticks - startTicks) * data->speed) + scriptTimeMs) - Thread.delay;
        int32_t syncTimeMs =  data_scriptTimeMs - Thread.delay;
        if (std::abs(currentTimeMs - syncTimeMs) >= 60) {
            LOGF_DEBUG("Resync -> %d", currentTimeMs - syncTimeMs);
            LOGF_DEBUG("prev: %d new: %d", currentTimeMs, syncTimeMs);

            scriptTimeMs = data_scriptTimeMs;
            startTicks = ticks;
            currentTimeMs = syncTimeMs;

            data->producer->sync(currentTimeMs);
        }
        else {
            // tick producers
            data->producer->tick(currentTimeMs);
        }

        
        // update channels
        const char* cmd = data->channel->GetCommand(currentTimeMs, data->tickrate);
        //if (cmd != nullptr) { LOG_DEBUG(cmd); }

        if (cmd != nullptr && data->player->status >= 0) {
            int len = strlen(cmd);
            data->player->status = c_serial_write_data(data->player->port, (void*)cmd, &len);
            if (data->player->status < 0) {
                LOG_ERROR("Failed to write to serial port.");
                break;
            }
        }

        int delay = maxTicks - (SDL_GetTicks() - ticks);
        if (delay > 0) {
            SDL_Delay(delay);
        }
    }
    data->running = false;
    data->requestStop = false;
    LOG_INFO("T-Code thread stopped.");

    return 0;
}

void TCodePlayer::play(float currentTimeMs, std::weak_ptr<Funscript>&& L0, std::weak_ptr<Funscript>&& R0, std::weak_ptr<Funscript>&& R1, std::weak_ptr<Funscript>&& R2) noexcept
{
    if (!Thread.running) {
        Thread.running = true;
        Thread.player = this;
        Thread.channel = &this->tcode;
        SDL_AtomicSet(&Thread.scriptTimeMs, std::round(currentTimeMs));
        Thread.producer = &this->prod;
        tcode.reset();
        prod.HookupChannels(&tcode, std::move(L0), std::move(R0), std::move(R1), std::move(R2));
        auto t = SDL_CreateThread(TCodeThread, "TCodePlayer", &Thread);
        SDL_DetachThread(t);
    }
}

void TCodePlayer::stop() noexcept
{
    if (Thread.running) {
        Thread.requestStop = true;
        while(!Thread.running) { /* busy wait */ }
        prod.ClearChannels();
    }
}

void TCodePlayer::sync(float currentTimeMs, float speed) noexcept
{
    Thread.speed = speed;
    SDL_AtomicSet(&Thread.scriptTimeMs, std::round(currentTimeMs));
}