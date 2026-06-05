# Examples

## Basic

Minimal init, one timeout, one interval, and clearing by id.

## Countdown

Shows `PulseCountdownConfig`, tick fields, and the final `isFinished=true` callback.

## PauseResumeRestart

Schedules a timer, pauses it, resumes it, restarts it, and prints timer state.

## ConfigAndLimits

Configures stack, queue, and per-type timer limits. Demonstrates a clean limit failure.

## Diagnostics

Prints `PulseDiag` fields while timers run.

## BindableCallbacks

Uses `std::bind` to bind private class methods as callbacks.
