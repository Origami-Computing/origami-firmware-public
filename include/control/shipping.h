#pragma once
namespace control {
// Factory request on UI task. Uses terminal navigation teardown before service
// shutdown; no app or screen imports in the service. Returns only on refusal.
bool enterShipping(bool freshTutorial = false);
}
