#pragma once
#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "BaseTelemetryModule.h"
#include "NodeDB.h"
#include "ProtobufModule.h"
#include "Channels.h"
#include "MeshService.h"
#include "PowerStatus.h"
#include "concurrency/Periodic.h"
#include <cstring>
#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>

class DeviceTelemetryModule : private concurrency::OSThread,
                              public BaseTelemetryModule,
                              public ProtobufModule<meshtastic_Telemetry>
{
    CallbackObserver<DeviceTelemetryModule, const meshtastic::Status *> nodeStatusObserver =
        CallbackObserver<DeviceTelemetryModule, const meshtastic::Status *>(this, &DeviceTelemetryModule::handleStatusUpdate);

  public:
    DeviceTelemetryModule()
        : concurrency::OSThread("DeviceTelemetry"),
          ProtobufModule("DeviceTelemetry", meshtastic_PortNum_TELEMETRY_APP, &meshtastic_Telemetry_msg),
          chargeNotifyPeriodic("ChargeNotify", [this]() { return this->runChargeNotify(); })
    {
        uptimeWrapCount = 0;
        uptimeLastMs = millis();
        nodeStatusObserver.observe(&nodeStatus->onNewStatus);
        setIntervalFromNow(setStartDelay()); // Wait until NodeInfo is sent
    }
    virtual bool wantUIFrame() { return false; }

  protected:
    /** Called to handle a particular incoming message
    @return true if you've guaranteed you've handled this message and no other handlers should be considered for it
    */
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Telemetry *p) override;
    virtual meshtastic_MeshPacket *allocReply() override;
    virtual int32_t runOnce() override;
    /**
     * Send our Telemetry into the mesh
     */
    bool sendTelemetry(NodeNum dest = NODENUM_BROADCAST, bool phoneOnly = false);

    /**
     * Get the uptime in seconds
     * Loses some accuracy after 49 days, but that's fine
     */
    uint32_t getUptimeSeconds() { return (0xFFFFFFFF / 1000) * uptimeWrapCount + (uptimeLastMs / 1000); }

  private:
    meshtastic_Telemetry getDeviceTelemetry();
    meshtastic_Telemetry getLocalStatsTelemetry();

    void sendLocalStatsToPhone();

    static constexpr uint8_t CHARGE_COMPLETE_PERCENT = 100;
    static constexpr uint32_t CHARGE_COMPLETE_STABLE_MS = 5 * SECONDS_IN_MINUTE * 1000;
    static constexpr uint32_t CHARGE_NOTIFY_CHECK_MS = 30 * 1000;

    concurrency::Periodic chargeNotifyPeriodic;
    bool chargeCompleteNotified = false;
    uint32_t chargeFullSinceMs = 0;

    int32_t runChargeNotify()
    {
        if (!powerStatus) {
            return CHARGE_NOTIFY_CHECK_MS;
        }

        const bool usbConnected = powerStatus->getHasUSB();
        const bool batteryPresent = powerStatus->getHasBattery();
        const uint8_t batteryPercent = powerStatus->getBatteryChargePercent();

        // A new charging cycle is armed only after USB has been removed.
        if (!usbConnected) {
            chargeCompleteNotified = false;
            chargeFullSinceMs = 0;
            return CHARGE_NOTIFY_CHECK_MS;
        }

        if (!batteryPresent || chargeCompleteNotified) {
            chargeFullSinceMs = 0;
            return CHARGE_NOTIFY_CHECK_MS;
        }

        if (batteryPercent < CHARGE_COMPLETE_PERCENT) {
            chargeFullSinceMs = 0;
            return CHARGE_NOTIFY_CHECK_MS;
        }

        const uint32_t now = millis();
        if (chargeFullSinceMs == 0) {
            chargeFullSinceMs = now;
            LOG_INFO("[ChargeNotify] Battery reached %u%%, confirming for 5 minutes", batteryPercent);
            return CHARGE_NOTIFY_CHECK_MS;
        }

        if ((uint32_t)(now - chargeFullSinceMs) < CHARGE_COMPLETE_STABLE_MS) {
            return CHARGE_NOTIFY_CHECK_MS;
        }

        sendChargeCompleteNotification();
        return CHARGE_NOTIFY_CHECK_MS;
    }

    void sendChargeCompleteNotification()
    {
        meshtastic_MeshPacket *p = allocDataPacket();
        if (!p) {
            LOG_WARN("[ChargeNotify] Packet allocation failed");
            return;
        }

        static const char message[] = "Аккумулятор полностью заряжен";

        p->to = NODENUM_BROADCAST;
        p->channel = channels.getPrimaryIndex();
        p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        p->want_ack = false;
        p->decoded.want_response = false;

        size_t len = strlen(message);
        if (len > sizeof(p->decoded.payload.bytes)) {
            len = sizeof(p->decoded.payload.bytes);
        }
        p->decoded.payload.size = len;
        memcpy(p->decoded.payload.bytes, message, len);

        LOG_INFO("[ChargeNotify] Sending full-charge notification to Primary");
        service->sendToMesh(p, RX_SRC_LOCAL, true);
        chargeCompleteNotified = true;
    }
    uint32_t sendToPhoneIntervalMs = SECONDS_IN_MINUTE * 1000;           // Send to phone every minute
    uint32_t sendStatsToPhoneIntervalMs = 15 * SECONDS_IN_MINUTE * 1000; // Send stats to phone every 15 minutes
    uint32_t lastSentStatsToPhone = 0;

    void refreshUptime()
    {
        auto now = millis();
        // If we wrapped around (~49 days), increment the wrap count
        if (now < uptimeLastMs)
            uptimeWrapCount++;

        uptimeLastMs = now;
    }

    uint32_t uptimeWrapCount;
    uint32_t uptimeLastMs;
};