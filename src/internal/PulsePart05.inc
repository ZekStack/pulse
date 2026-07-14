			PulseTimerResult preflight = impl->validateTimerCreateLocked(type, limitMessage);
			if (!preflight) {
				result = preflight;
				releaseAfterUnlock = timer;
			} else if (!impl->allocateTimerIdLocked(timer->id)) {
				result = PulseTimerResult::failure(
				    PulseStatus::InternalError,
				    "timer id exhausted"
				);
				releaseAfterUnlock = timer;
			} else {
				timer->nextDueMs = pulseNowMs() + firstDelayMs;
				if (!impl->addRegistryLocked(timer)) {
					result = PulseTimerResult::failure(
					    PulseStatus::Busy,
					    "timer storage is full"
					);
					releaseAfterUnlock = timer;
				} else {
					PulseResult queued =
					    impl->enqueueCommandLocked(PulseCommandType::Add, timer->id);
					if (!queued) {
						releaseAfterUnlock = impl->removeRegistryLocked(timer->id);
						result = PulseTimerResult::failure(
						    queued.status,
						    queued.message,
						    timer->id
						);
					} else {
						result = PulseTimerResult::success(timer->id, successMessage);
					}
				}
			}
		}
	}
	if (releaseAfterUnlock != nullptr) {
		releaseAfterUnlock->release();
	}
	return result;
}
} // namespace

PulseResult PulseResult::success(const char *message) {
	PulseResult result;
	result.result = true;
	result.status = PulseStatus::Ok;
	result.message = message != nullptr ? message : "ok";
	return result;
}

PulseResult PulseResult::failure(PulseStatus status, const char *message) {
	PulseResult result;
	result.result = false;
	result.status = status;
	result.message = message != nullptr ? message : "error";
	return result;
}

PulseTimerResult PulseTimerResult::success(PulseTimerId id, const char *message) {
	PulseTimerResult result;
	result.result = true;
	result.status = PulseStatus::Ok;
	result.message = message != nullptr ? message : "ok";
	result.id = id;
	return result;
}

PulseTimerResult PulseTimerResult::failure(
    PulseStatus status,
    const char *message,
    PulseTimerId id
) {
	PulseTimerResult result;
	result.result = false;
	result.status = status;
	result.message = message != nullptr ? message : "error";
	result.id = id;
	return result;
}

Pulse::~Pulse() {
	PulseImpl *impl = _impl.exchange(nullptr, std::memory_order_acq_rel);
	if (impl == nullptr) {
		return;
	}

	bool selfTask = false;
	{
		PulseLock lock(impl->mutex);
		if (lock) {
			selfTask = pulse_task_support::isCurrentTask(impl->taskHandle);
			if (selfTask && impl->lifecycle == PulseLifecycleState::Running) {
				impl->lifecycle = PulseLifecycleState::Stopping;
			}
		}
	}

	if (!selfTask) {
		(void)waitForEnd(impl, 0, true);
	}
	impl->release();
}

PulseResult Pulse::init(const PulseConfig &config) {
	if (!pulse_task_support::isValidStackSize(config.stackSizeBytes)) {
		return PulseResult::failure(
		    PulseStatus::InvalidArgument,
		    "stack size must be at least 1024 bytes and aligned"
		);
	}
	if (config.commandQueueSize == 0) {
		return PulseResult::failure(PulseStatus::InvalidArgument, "command queue size is required");
	}
	if (addWouldOverflow(config.maxTimeouts, config.maxIntervals) ||
	    addWouldOverflow(config.maxTimeouts + config.maxIntervals, config.maxCountdowns)) {
		return PulseResult::failure(PulseStatus::InvalidArgument, "timer capacity overflow");
	}
	const uint32_t timerCapacity =
	    config.maxTimeouts + config.maxIntervals + config.maxCountdowns;
	if (timerCapacity == 0) {
		return PulseResult::failure(
		    PulseStatus::InvalidArgument,
		    "at least one timer slot is required"
		);
	}

	PulseImpl *impl = loadImpl(_impl);
	if (impl == nullptr) {
		PulseImpl *candidate = new (std::nothrow) PulseImpl();
		if (candidate == nullptr) {
			return PulseResult::failure(
			    PulseStatus::OutOfMemory,
			    "failed to allocate pulse implementation"
			);
		}
		if (!candidate->isValid()) {
			delete candidate;
			return PulseResult::failure(
			    PulseStatus::OutOfMemory,
			    "failed to allocate pulse synchronization"
			);
		}
		PulseImpl *expected = nullptr;
		if (!_impl.compare_exchange_strong(
		        expected,
		        candidate,
		        std::memory_order_acq_rel,
		        std::memory_order_acquire
		    )) {
			candidate->release();
			impl = expected;
		} else {
			impl = candidate;
		}
	}

	bool usePsramStack = false;
	PulseStackType actualStackType = PulseStackType::Internal;
	if (config.stackType == PulseStackType::Psram) {
		if (!pulse_task_support::hasExternalStackSupport()) {
			return PulseResult::failure(
			    PulseStatus::TaskCreateFailed,
			    "PSRAM task stacks are not available"
			);
		}
		usePsramStack = true;
		actualStackType = PulseStackType::Psram;
	} else if (
	    config.stackType == PulseStackType::Auto && pulse_task_support::hasExternalStackSupport()
	) {
		usePsramStack = true;
		actualStackType = PulseStackType::Psram;
	}

	PulseLock lock(impl->mutex);
	if (!lock) {
		return PulseResult::failure(PulseStatus::InternalError, "failed to lock pulse");
	}
	if (impl->lifecycle == PulseLifecycleState::Running) {
		return PulseResult::failure(PulseStatus::AlreadyInitialized, "pulse already initialized");
	}
	if (impl->lifecycle != PulseLifecycleState::Uninitialized || impl->endWaiterCount != 0) {
		return PulseResult::failure(PulseStatus::Busy, "pulse lifecycle is busy");
	}

	xEventGroupClearBits(impl->completionEvent, kTaskStoppedBit);
	impl->config = config;
	impl->actualStackType = actualStackType;
	impl->createdWithCaps = false;
	impl->executedCallbackCount = 0;
	impl->droppedCommandCount = 0;
	impl->lateCallbackCount = 0;
