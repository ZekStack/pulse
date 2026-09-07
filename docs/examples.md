# Examples

## Basic

Minimal init, one timeout, one interval, and clearing by id.

## Countdown

Shows `PulseCountdownConfig`, tick fields, and the final `isFinished=true` callback.

## PauseResumeRestart

Schedules a timer, pauses it, resumes it, restarts it, and prints timer state.

## ConfigAndLimits

Configures `Strata::MemoryPolicy`, scheduler task settings, command queue size, and per-type timer limits. It also demonstrates a clean timer-limit failure.

## Diagnostics

Prints runtime `PulseDiag` counters together with requested Strata placement and observed stack/command-queue storage regions.

## BindableCallbacks

Uses `std::bind` to bind private class methods as callbacks.
