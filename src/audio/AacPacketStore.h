#pragma once

#include <string>

#include "audio/AacEncoder.h"
#include "record/PacketStore.h"

namespace olouie::audio {

bool BuildAacPacketMetadata(const EncodedAacPacket& packet,
                            record::PacketMetadata* metadata,
                            std::wstring* error) noexcept;

bool AppendAacPacket(record::PacketStore* store,
                     const EncodedAacPacket& packet,
                     std::wstring* error);

}  // namespace olouie::audio
