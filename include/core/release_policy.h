#pragma once

// Release-channel capability is compiled into the image, never read from NVS.
// Developer/staging builds retain their existing configurable behavior.
#ifndef ORIGAMI_PRODUCTION_BUILD
#define ORIGAMI_PRODUCTION_BUILD 0
#endif
#if ORIGAMI_PRODUCTION_BUILD != 0 && ORIGAMI_PRODUCTION_BUILD != 1
#error ORIGAMI_PRODUCTION_BUILD must be 0 or 1
#endif
#ifndef ORIGAMI_STAGING_BUILD
#define ORIGAMI_STAGING_BUILD 0
#endif
#if ORIGAMI_STAGING_BUILD != 0 && ORIGAMI_STAGING_BUILD != 1
#error ORIGAMI_STAGING_BUILD must be 0 or 1
#endif
#if ORIGAMI_STAGING_BUILD && ORIGAMI_PRODUCTION_BUILD
#error A firmware image cannot target both staging and production
#endif

#ifndef ORIGAMI_INTEGRATION_CANDIDATE
#define ORIGAMI_INTEGRATION_CANDIDATE 0
#endif
#if ORIGAMI_INTEGRATION_CANDIDATE && !ORIGAMI_PRODUCTION_BUILD
#error Integration candidates must enforce production policy
#endif

namespace core { namespace release_policy {
constexpr bool integrationCandidate = ORIGAMI_INTEGRATION_CANDIDATE == 1;
constexpr bool productionOnly = ORIGAMI_PRODUCTION_BUILD == 1;
constexpr bool stagingBuild = ORIGAMI_STAGING_BUILD == 1;
}}
