#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dfs {

constexpr uint32_t DFS_PROTOCOL_MAGIC = 0x44465331; // "DFS1"
constexpr uint16_t DFS_PROTOCOL_VERSION = 1;

enum class MessageType : uint16_t {
    HEARTBEAT = 1,
    HEARTBEAT_ACK = 2,
    STORE_CHUNK = 10,
    STORE_CHUNK_ACK = 11,
    RETRIEVE_CHUNK = 20,
    RETRIEVE_CHUNK_ACK = 21,
    DELETE_CHUNK = 30,
    DELETE_CHUNK_ACK = 31,
    VERIFY_CHUNK = 40,
    VERIFY_CHUNK_ACK = 41,
    GET_NODE_STATS = 50,
    GET_NODE_STATS_ACK = 51,
    RESPONSE_OK = 100,
    RESPONSE_ERROR = 101,
    SHUTDOWN_NODE = 200
};

#pragma pack(push, 1)
struct ProtocolHeader {
    uint32_t magic = DFS_PROTOCOL_MAGIC;
    uint16_t version = DFS_PROTOCOL_VERSION;
    uint16_t type = 0;
    uint32_t json_len = 0;
    uint64_t data_len = 0;
};
#pragma pack(pop)

struct DfsMessage {
    MessageType type = MessageType::RESPONSE_OK;
    std::string json_payload;
    std::vector<uint8_t> binary_data;
};

} // namespace dfs
