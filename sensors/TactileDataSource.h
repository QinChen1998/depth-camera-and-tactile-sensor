#pragma once
#include "TactileFrame.h"
namespace tactile {
enum class SourceState { Stopped, Running, Paused };
enum class Scenario { NoContact, PressRelease, Tangential, MovingContact };
// GUI-thread pull interface: latest() produces at most one current sample, never a queue.
class TactileDataSource {
public:
    virtual ~TactileDataSource() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void setPaused(bool paused) = 0;
    virtual SourceState state() const = 0;
    virtual QUuid sessionId() const = 0;
    virtual quint64 generation() const = 0;
    virtual bool latest(Frame &frame) = 0;
};
}
