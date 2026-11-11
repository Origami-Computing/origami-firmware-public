#pragma once
#include "core/release_policy.h"

// Build-channel destinations. Credentials remain per-account NVS data.
// Official CI derives ORIGAMI_STAGING_BUILD from the publishing branch.
#define KAMI_PRODUCTION_API_URL "https://api.kami.so"
#define KAMI_STAGING_API_URL "https://kami2-staging.up.railway.app"
#if ORIGAMI_STAGING_BUILD
#define KAMI_HOSTED_API_URL KAMI_STAGING_API_URL
#define KAMI_ONBOARDING_WEB_URL "https://vivacious-courtesy-staging.up.railway.app/device/manage"
#define KAMI_ONBOARDING_HOST_LABEL "vivacious-courtesy-staging.up.railway.app"
#define KAMI_APP_STORE_URL "https://kami-app-store-staging.up.railway.app"
#else
#define KAMI_HOSTED_API_URL KAMI_PRODUCTION_API_URL
#define KAMI_ONBOARDING_WEB_URL "https://kami.so/device/manage"
#define KAMI_ONBOARDING_HOST_LABEL "kami.so"
// Package bytes are pinned by signature and hash; the store only resolves codes.
#define KAMI_APP_STORE_URL "https://kami-device-store-production-production.up.railway.app"
#endif
