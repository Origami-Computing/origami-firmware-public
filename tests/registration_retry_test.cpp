#include "../src/services/registration_retry.h"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>
using services::net_poll::RegistrationRetry;
int main() {
  RegistrationRetry p;
  assert(p.ready(100));
  p.failed(100, -1); assert(!p.ready(15099)); assert(p.ready(15100));
  p.onboarding(200); assert(p.ready(200));
  assert(p.failed(8200, -1) == 1000);
  assert(!p.ready(9199)); assert(p.ready(9200));
  assert(p.failed(17200, -1) == 2000);
  assert(!p.ready(19199)); assert(p.ready(19200));
  assert(p.failed(27200, -1) == 15000); assert(!p.priority(27200));
  for (int code : {401,403,429,400}) {
    RegistrationRetry q; q.onboarding(0);
    assert(q.failed(100,code)==15000); assert(!q.priority(100));
  }
  RegistrationRetry q; q.onboarding(0);
  assert(q.failed(100,429,60000)==60000);
  q.onboarding(200); assert(!q.ready(60099)); assert(q.ready(60100));
  assert(!q.priority(200));
  q.success(); q.onboarding(0xfffffff0u);
  assert(q.failed(0xfffffff0u,-1)==1000);
  assert(!q.ready(983)); assert(q.ready(984));
  RegistrationRetry r; r.onboarding(0); assert(!r.priority(45000));
  assert(r.failed(45000,-1)==15000);
  puts("PASS bounded retries, normal fallback, auth/rate limits, success, expiry and clock wrap");
}
