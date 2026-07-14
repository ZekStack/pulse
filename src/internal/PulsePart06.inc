	impl->finalStackHighWaterMarkBytes = 0;
	impl->nextTimerId = 1;
	impl->timerCapacity = timerCapacity;
	impl->timerCount = 0;
	impl->activeTimerCount = 0;
	impl->timers.reset(new (std::nothrow) PulseTimerRecord *[timerCapacity]());
	impl->activeTimers.reset(new (std::nothrow) PulseTimerRecord *[timerCapacity]());
	if (!impl->timers || !impl->activeTimers) {
		impl->timers.reset();
		impl->activeTimers.reset();
		impl->timerCapacity = 0;
		return PulseResult::failure(PulseStatus::OutOfMemory, "failed to allocate timer storage");
	}

	impl->commandQueue = xQueueCreate(config.commandQueueSize, sizeof(PulseCommand));
	if (impl->commandQueue == nullptr) {
		impl->timers.reset();
		impl->activeTimers.reset();
		impl->timerCapacity = 0;
		return PulseResult::failure(PulseStatus::QueueCreateFailed, "failed to create command queue");
	}

	impl->lifecycleGeneration++;
	if (impl->lifecycleGeneration == 0) {
		impl->lifecycleGeneration = 1;
	}
	impl->lifecycle = PulseLifecycleState::Running;
	impl->retain();

	TaskHandle_t handle = nullptr;
	bool createdWithCaps = false;
	const BaseType_t created = pulse_task_support::createTask(
	    &PulseImpl::taskEntry,
	    config.taskName != nullptr ? config.taskName : "pulse-task",
	    config.stackSizeBytes,
	    impl,
	    config.priority,
	    &handle,
	    config.coreId,
	    usePsramStack,
	    createdWithCaps
	);
	if (created != pdPASS || handle == nullptr) {
		impl->lifecycle = PulseLifecycleState::Uninitialized;
		impl->release();
		vQueueDelete(impl->commandQueue);
		impl->commandQueue = nullptr;
		impl->timers.reset();
		impl->activeTimers.reset();
		impl->timerCapacity = 0;
		return PulseResult::failure(PulseStatus::TaskCreateFailed, "failed to create pulse task");
	}

	impl->taskHandle = handle;
	impl->createdWithCaps = createdWithCaps;
	return PulseResult::success("pulse initialized");
}

PulseResult Pulse::end(uint32_t timeoutMs) {
	return waitForEnd(loadImpl(_impl), timeoutMs, false);
}

PulseTimerResult Pulse::setTimeout(PulseCallback callback, uint32_t delayMs) {
	if (!callback) {
		return PulseTimerResult::failure(PulseStatus::InvalidArgument, "callback is required");
	}
	if (delayMs == 0) {
		return PulseTimerResult::failure(PulseStatus::InvalidArgument, "delay is required");
	}

	PulseTimerRecord *timer = new (std::nothrow) PulseTimerRecord();
	if (timer != nullptr) {
		timer->type = PulseTimerType::Timeout;
		timer->callback = std::move(callback);
		timer->delayMs = delayMs;
	}
	return scheduleTimer(
	    loadImpl(_impl),
	    timer,
	    PulseTimerType::Timeout,
	    "timeout limit reached",
	    delayMs,
	    "timeout scheduled"
	);
}

PulseTimerResult Pulse::setInterval(PulseCallback callback, uint32_t intervalMs) {
	if (!callback) {
		return PulseTimerResult::failure(PulseStatus::InvalidArgument, "callback is required");
	}
	if (intervalMs == 0) {
		return PulseTimerResult::failure(PulseStatus::InvalidArgument, "interval is required");
	}

	PulseTimerRecord *timer = new (std::nothrow) PulseTimerRecord();
	if (timer != nullptr) {
		timer->type = PulseTimerType::Interval;
		timer->callback = std::move(callback);
		timer->intervalMs = intervalMs;
	}
	return scheduleTimer(
	    loadImpl(_impl),
	    timer,
	    PulseTimerType::Interval,
	    "interval limit reached",
	    intervalMs,
	    "interval scheduled"
	);
}

PulseTimerResult Pulse::setCountdown(
    const PulseCountdownConfig &config,
    PulseCountdownCallback callback
) {
	if (!callback) {
		return PulseTimerResult::failure(PulseStatus::InvalidArgument, "callback is required");
	}
	if (config.durationMs == 0) {
		return PulseTimerResult::failure(PulseStatus::InvalidArgument, "duration is required");
	}
	if (config.tickMs == 0) {
		return PulseTimerResult::failure(PulseStatus::InvalidArgument, "tick is required");
	}
	if (config.tickMs > config.durationMs) {
		return PulseTimerResult::failure(
		    PulseStatus::InvalidArgument,
		    "tick must be less than or equal to duration"
		);
	}

	PulseTimerRecord *timer = new (std::nothrow) PulseTimerRecord();
	if (timer != nullptr) {
		timer->type = PulseTimerType::Countdown;
		timer->countdownCallback = std::move(callback);
		timer->durationMs = config.durationMs;
		timer->tickMs = config.tickMs;
	}
	return scheduleTimer(
	    loadImpl(_impl),
	    timer,
	    PulseTimerType::Countdown,
	    "countdown limit reached",
	    config.tickMs,
	    "countdown scheduled"
	);
}

PulseResult Pulse::clear(PulseTimerId id) {
	return controlTimer(
	    loadImpl(_impl),
	    id,
	    PulseCommandType::Clear,
	    false,
	    PulseTimerType::Timeout,
	    ""
	);
}

PulseResult Pulse::clearTimeout(PulseTimerId id) {
	return controlTimer(
	    loadImpl(_impl),
	    id,
	    PulseCommandType::Clear,
	    true,
	    PulseTimerType::Timeout,
	    "timer is not a timeout"
	);
}

PulseResult Pulse::clearInterval(PulseTimerId id) {
	return controlTimer(
	    loadImpl(_impl),
	    id,
	    PulseCommandType::Clear,
	    true,
	    PulseTimerType::Interval,
	    "timer is not an interval"
	);
}

PulseResult Pulse::clearCountdown(PulseTimerId id) {
	return controlTimer(
	    loadImpl(_impl),
	    id,
	    PulseCommandType::Clear,
	    true,
	    PulseTimerType::Countdown,
	    "timer is not a countdown"
	);
}

PulseResult Pulse::pause(PulseTimerId id) {
	return controlTimer(
	    loadImpl(_impl),
	    id,
	    PulseCommandType::Pause,
	    false,
	    PulseTimerType::Timeout,
	    ""
	);
}

PulseResult Pulse::resume(PulseTimerId id) {
	return controlTimer(
