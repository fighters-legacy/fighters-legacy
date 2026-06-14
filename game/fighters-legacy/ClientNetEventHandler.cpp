// SPDX-License-Identifier: GPL-3.0-or-later
#include "ClientNetEventHandler.h"
#include "ServerNotice.h"

#include "ILogger.h"
#include "INetwork.h"
#include "console/GameConsole.h"
#include "entity/EntityDef.h"
#include "entity/EntityTypeRegistry.h"
#include "net/GameProtocol.h"
#include "render/RenderSnapshot.h"
#include "render/SimRenderBridge.h"
#include "weather/WeatherController.h"

#include <cstring>
#include <glm/gtc/quaternion.hpp>
#include <sstream>

void ClientNetEventHandler::onConnect(uint32_t /*peerId*/) {
    m_connected = true;
    logger.log(LogLevel::Info, __FILE__, __LINE__, "connected to local fl-server");
}

void ClientNetEventHandler::onDisconnect(uint32_t /*peerId*/) {
    logger.log(LogLevel::Info, __FILE__, __LINE__, "disconnected from local fl-server");
    if (m_connected && assignedEntityIdx == 0 && connectFailMsg) {
        const char* expected = nullptr;
        connectFailMsg->compare_exchange_strong(expected, "Connection refused by server.", std::memory_order_release,
                                                std::memory_order_relaxed);
    }
}

void ClientNetEventHandler::onReceive(uint32_t /*peerId*/, const void* data, std::size_t size) {
    if (size < 1)
        return;
    const uint8_t msgId = *static_cast<const uint8_t*>(data);

    if (msgId == static_cast<uint8_t>(fl::MsgId::Hello)) {
        if (size < sizeof(fl::MsgHello))
            return;
        fl::MsgHello hello;
        std::memcpy(&hello, data, sizeof(hello));
        if (hello.protocolVersion != fl::kProtocolVersion) {
            logger.log(LogLevel::Error, __FILE__, __LINE__, "server protocol version mismatch — disconnecting");
            if (connectFailMsg) {
                const char* expected = nullptr;
                connectFailMsg->compare_exchange_strong(expected, "Server version mismatch.", std::memory_order_release,
                                                        std::memory_order_relaxed);
            }
            net.disconnect();
        }
        return;
    }

    if (msgId == static_cast<uint8_t>(fl::MsgId::ConnectAck)) {
        if (size < sizeof(fl::MsgConnectAck))
            return;
        fl::MsgConnectAck ack;
        std::memcpy(&ack, data, sizeof(ack));
        assignedEntityIdx = ack.assignedEntityIdx;
        assignedEntityGen = ack.assignedEntityGen;
        const uint8_t* typeData = static_cast<const uint8_t*>(data) + sizeof(ack);
        for (uint16_t i = 0; i < ack.typeCount; ++i) {
            if ((typeData - static_cast<const uint8_t*>(data)) + sizeof(fl::MsgEntityTypeDef) > size)
                break;
            fl::MsgEntityTypeDef td;
            std::memcpy(&td, typeData, sizeof(td));
            typeData += sizeof(td);
            if (registry.findById(td.id))
                continue; // already registered
            fl::EntityDef def;
            def.id = td.id;
            def.mesh = td.mesh;
            def.classicDamageMesh = td.dmgMesh;
            def.maxHp = 100.0f;
            registry.registerType(std::move(def));
        }
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::WorldSnapshot)) {
        if (size < sizeof(fl::MsgWorldSnapshotHeader))
            return;
        fl::MsgWorldSnapshotHeader hdr;
        std::memcpy(&hdr, data, sizeof(hdr));
        const std::size_t expected = sizeof(fl::MsgWorldSnapshotHeader) + hdr.entityCount * sizeof(fl::MsgEntityEntry);
        if (size < expected)
            return;

        fl::RenderSnapshot snap;
        snap.tickIndex = hdr.tickIndex;
        snap.entries.reserve(hdr.entityCount);

        const uint8_t* entryData = static_cast<const uint8_t*>(data) + sizeof(hdr);
        for (uint16_t i = 0; i < hdr.entityCount; ++i) {
            fl::MsgEntityEntry e;
            std::memcpy(&e, entryData + i * sizeof(e), sizeof(e));

            fl::EntityRenderEntry re;
            re.entityIdx = e.entityIdx;
            re.entityGen = e.entityGen;
            re.typeIndex = e.typeIndex;
            re.position = {e.pos[0], e.pos[1], e.pos[2]};
            re.velocity = {e.vel[0], e.vel[1], e.vel[2]};
            // Wire format: x,y,z,w — glm::quat constructor: (w,x,y,z)
            re.orientation = glm::quat(e.ori[3], e.ori[0], e.ori[1], e.ori[2]);
            re.damageLevel = e.damageLevel;
            re.playerOwned = (e.flags & 1u) != 0;
            re.throttle = e.throttle;
            re.fuelPct = e.fuelPct;
            re.abEngaged = e.abEngaged != 0;
            re.engineFailFlags = e.engineFailFlags;
            snap.entries.push_back(re);
        }
        char traceBuf[80];
        std::snprintf(traceBuf, sizeof(traceBuf), "WorldSnapshot: hdr.entityCount=%u, built=%zu", hdr.entityCount,
                      snap.entries.size());
        logger.log(LogLevel::Trace, __FILE__, __LINE__, traceBuf);
        bridge.publishExternal(std::move(snap));
        tickAlpha.markNewTick();
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::WeatherState)) {
        if (size < sizeof(fl::MsgWeatherState))
            return;
        fl::MsgWeatherState ws;
        std::memcpy(&ws, data, sizeof(ws));
        float tod = static_cast<float>(ws.timeOfDayTenths) / 10.f;
        env.fogDensity = ws.fogDensity;
        env.fogStartDist = ws.fogStartDist;
        env.timeOfDay = tod;
        fl::WeatherController::applyPresetToEnv(static_cast<fl::WeatherPreset>(ws.preset), tod, env);
        env.windX = ws.windX;
        env.windZ = ws.windZ;
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::ServerNotice)) {
        if (size < sizeof(fl::MsgServerNotice))
            return;
        fl::MsgServerNotice sn;
        std::memcpy(&sn, data, sizeof(sn));
        sn.text[59] = '\0';
        char noticeBuf[72];
        std::snprintf(noticeBuf, sizeof(noticeBuf), "[server] %s", sn.text);
        if (console)
            console->print(std::string(noticeBuf));
        if (notice)
            notice->setNotice(noticeBuf, sn.secondsRemaining);
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::AdminResponse)) {
        if (size < sizeof(fl::MsgAdminResponse))
            return;
        fl::MsgAdminResponse resp;
        std::memcpy(&resp, data, sizeof(resp));
        resp.text[sizeof(resp.text) - 1] = '\0';
        if (console && resp.text[0] != '\0')
            console->print(std::string("[admin] ") + resp.text);
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::Motd)) {
        if (size < 4)
            return;
        uint16_t wireSecs = 0;
        std::memcpy(&wireSecs, static_cast<const uint8_t*>(data) + 1, sizeof(wireSecs));
        const uint32_t effectiveSecs = wireSecs > 0 ? static_cast<uint32_t>(wireSecs) : motdDisplaySeconds;
        const std::size_t textLen = std::min(size - 3, fl::kMaxMotdBytes);
        std::string text(static_cast<const char*>(data) + 3, textLen);
        while (!text.empty() && text.back() == '\0')
            text.pop_back();
        std::istringstream stream(text);
        std::string line;
        bool first = true;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            std::string prefixed = std::string("[server] ") + line;
            if (console)
                console->print(prefixed);
            if (notice && first)
                notice->setNotice(prefixed, 0, effectiveSecs);
            first = false;
        }
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::ConnectRefusal)) {
        if (size < sizeof(fl::MsgConnectRefusal) || !connectFailMsg)
            return;
        fl::MsgConnectRefusal ref{};
        std::memcpy(&ref, data, sizeof(ref));
        ref.reason[sizeof(ref.reason) - 1] = '\0';
        std::memcpy(m_connectRefusalReason, ref.reason, sizeof(ref.reason));
        m_connectRefusalReason[sizeof(m_connectRefusalReason) - 1] = '\0';
        const char* expected = nullptr;
        connectFailMsg->compare_exchange_strong(expected, m_connectRefusalReason, std::memory_order_release,
                                                std::memory_order_relaxed);
    }
    // Unknown msgIds: silently discard
}
