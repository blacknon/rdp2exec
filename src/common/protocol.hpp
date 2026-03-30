#pragma once

#include <cstdint>

namespace rdp2exec
{

    inline constexpr const char *kChannelName = "rdp2exec";
    inline constexpr const char *kDefaultSocketPath = "/tmp/rdp2exec-stdio.sock";

    namespace frame
    {
        inline constexpr uint8_t kInput = 0x01;
        inline constexpr uint8_t kResize = 0x02;
        inline constexpr uint8_t kClose = 0x03;

        inline constexpr uint8_t kReady = 0x81;
        inline constexpr uint8_t kOutput = 0x82;
        inline constexpr uint8_t kExit = 0x83;
        inline constexpr uint8_t kError = 0x84;
    } // namespace frame

} // namespace rdp2exec
