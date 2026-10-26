#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace dfs {

class SHA256 {
public:
    SHA256();
    void update(const uint8_t* data, size_t length);
    void update(const std::string& data);
    std::string final();
    std::vector<uint8_t> finalRaw();

    static std::string hashString(const std::string& input);
    static std::string hashBuffer(const uint8_t* buffer, size_t len);
    static std::string hashFile(const std::string& filepath);

private:
    uint32_t state_[8];
    uint64_t bit_len_;
    uint8_t buffer_[64];
    size_t buffer_len_;

    void transform(const uint8_t* chunk);
};

} // namespace dfs
