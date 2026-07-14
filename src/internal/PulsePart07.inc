	    loadImpl(_impl),
	    id,
	    PulseCommandType::Resume,
	    false,
	    PulseTimerType::Timeout,
	    ""
	);
}

PulseResult Pulse::restart(PulseTimerId id) {
	return controlTimer(
	    loadImpl(_impl),
	    id,
	    PulseCommandType::Restart,
	    false,
	    PulseTimerType::Timeout,
	    ""
	);
}

PulseTimerState Pulse::getState(PulseTimerId id) {
	PulseImpl *impl = loadImpl(_impl);
	if (impl == nullptr) {
		return PulseTimerState::NotFound;
	}
	PulseLock lock(impl->mutex);
	if (!lock) {
		return PulseTimerState::NotFound;
	}
	PulseTimerRecord *timer = impl->findTimerLocked(id);
	return timer != nullptr ? timer->state : PulseTimerState::NotFound;
}

PulseDiag Pulse::getDiagnostics() {
	PulseDiag diag;
	PulseImpl *impl = loadImpl(_impl);
	if (impl == nullptr) {
		return diag;
	}
	PulseLock lock(impl->mutex);
	if (!lock) {
		return diag;
	}

	diag.commandQueueSize = impl->config.commandQueueSize;
	diag.commandQueueUsed = impl->commandQueueUsageLocked();
	diag.executedCallbackCount = impl->executedCallbackCount;
	diag.droppedCommandCount = impl->droppedCommandCount;
	diag.lateCallbackCount = impl->lateCallbackCount;
	diag.requestedStackType = impl->config.stackType;
	diag.actualStackType = impl->actualStackType;
	diag.stackHighWaterMarkBytes = impl->taskHandle != nullptr ?
	                                       pulse_task_support::stackHighWaterMarkBytes(
	                                           impl->taskHandle
	                                       ) :
	                                       impl->finalStackHighWaterMarkBytes;

	for (uint32_t index = 0; index < impl->timerCount; index++) {
		PulseTimerRecord *timer = impl->timers[index];
		if (timer == nullptr) {
			continue;
		}
		if (timer->type == PulseTimerType::Timeout) {
			diag.timeoutCount++;
		} else if (timer->type == PulseTimerType::Interval) {
			diag.intervalCount++;
		} else {
			diag.countdownCount++;
		}
		if (timer->state == PulseTimerState::Running) {
			diag.runningCount++;
		} else if (timer->state == PulseTimerState::Paused) {
			diag.pausedCount++;
		}
	}
	return diag;
}

const char *Pulse::statusToString(PulseStatus status) const {
	switch (status) {
	case PulseStatus::Ok:
		return "ok";
	case PulseStatus::NotInitialized:
		return "notInitialized";
	case PulseStatus::AlreadyInitialized:
		return "alreadyInitialized";
	case PulseStatus::InvalidArgument:
		return "invalidArgument";
	case PulseStatus::OutOfMemory:
		return "outOfMemory";
	case PulseStatus::TaskCreateFailed:
		return "taskCreateFailed";
	case PulseStatus::QueueCreateFailed:
		return "queueCreateFailed";
	case PulseStatus::TimerNotFound:
		return "timerNotFound";
	case PulseStatus::QueueFull:
		return "queueFull";
	case PulseStatus::Busy:
		return "busy";
	case PulseStatus::Timeout:
		return "timeout";
	case PulseStatus::InternalError:
		return "internalError";
	}
	return "unknown";
}

const char *Pulse::timerStateToString(PulseTimerState state) const {
	switch (state) {
	case PulseTimerState::Running:
		return "running";
	case PulseTimerState::Paused:
		return "paused";
	case PulseTimerState::NotFound:
		return "notFound";
	}
	return "unknown";
}

const char *Pulse::timerTypeToString(PulseTimerType type) const {
	switch (type) {
	case PulseTimerType::Timeout:
		return "timeout";
	case PulseTimerType::Interval:
		return "interval";
	case PulseTimerType::Countdown:
		return "countdown";
	}
	return "unknown";
}
