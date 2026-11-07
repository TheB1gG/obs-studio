#include "StreamDestinationRuntime.hpp"

const char *StreamDestinationRuntime::stateName() const
{
	switch (state) {
	case State::Preparing:
		return "preparing";
	case State::Connecting:
		return "connecting";
	case State::DelayStarting:
		return "delay-starting";
	case State::Live:
		return "live";
	case State::Stopping:
		return "stopping";
	case State::DelayStopping:
		return "delay-stopping";
	case State::Reconnecting:
		return "reconnecting";
	case State::Stopped:
	default:
		return "stopped";
	}
}
