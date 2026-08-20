#pragma once

namespace jam2::application {

// Observable state for deterministic private-automation completion gates.
// System backends report Unsupported; only synthetic test backends implement
// the arm/active/release lifecycle.
enum class AutomationCompletionGateState {
    Unsupported,
    Idle,
    Armed,
    Active,
};

} // namespace jam2::application
