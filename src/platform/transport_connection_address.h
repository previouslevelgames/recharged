#pragma once
#include <cstdint>
class TransportConnection;
namespace mscharged::platform {
// The real source pool owns every accepted slot. No pointer ID or owner lease.
std::uint32_t EncodeTransportConnectionAddress(const TransportConnection* connection);
TransportConnection* DecodeTransportConnectionAddress(std::uint32_t word);
}
