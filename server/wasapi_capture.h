#pragma once
#include <atomic>
namespace lanaudio {
class PacketSender;
void captureLoop(PacketSender& sender, const std::atomic<bool>& running, const std::atomic<bool>& paused);
void muteDefaultOutput();
void restoreDefaultOutput();
}
