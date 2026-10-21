#pragma once

namespace services {
namespace eyes_asset {

/** Validate the canonical eyes bitmap already present on SD. Call during boot,
 *  after provisioning and before background recovery workers start. */
bool inspect();

/** One-shot background repair. Downloads only when inspect() found the asset
 *  missing or invalid. Safe to call repeatedly from the network worker. */
void service();

/** True once after a repaired asset has been installed on SD. */
bool takeInstalledNotification();

}  // namespace eyes_asset
}  // namespace services
