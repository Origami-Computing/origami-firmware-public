// Included after the actual coordinator bodies in the lifecycle host harness.
bool testRestartAdmission(){
 using namespace services::background_network;
 resumeAfterMusicAudio();
 beforeStart=[] {auto result=suspendForMusicAudio(1000);assert(result.uploadsReady&&result.poller==AudioSuspension::Poller::Stopped);assert(!services::net_poll::running());};
 tick();
#if NEGATIVE_CONTROL
 assert(services::net_poll::running());finish();
 puts("REPRODUCED frozen build156: stale coordinator restart defeats completed Music stop");
 return true;
#else
 assert(!services::net_poll::running());
 puts("PASS cancellation before admission prevents stale coordinator restart");
 auto count=tasks.size();tick();assert(tasks.size()==count);
 resumeAfterMusicAudio();resumeAfterMusicAudio();uploadRunning=true;
 tick();assert(!running());uploadRunning=false;
 allocationFails=true;tick();assert(!running());allocationFails=false;
 tick();assert(running()&&tasks.size()==count+1);tick();assert(tasks.size()==count+1);finish();
 puts("PASS request coalescing, upload-idle deferral and allocation-failure retry");
 // A manual start also consumes existing intent, but may not lose it on failure.
 resumeAfterMusicAudio();allocationFails=true;assert(!start());allocationFails=false;
 tick();assert(running());finish();
 allocationFails=true;assert(!start());allocationFails=false;tick();assert(!running());
 puts("PASS manual allocation failure preserves an existing request without inventing one");
 // Stop during failed allocation must not resurrect its cancelled request.
 allocationFails=true;
 {std::lock_guard<std::mutex> lock(hooksMutex);duringCreate=[] {assert(!stop(0));};}
 resumeAfterMusicAudio();tick();allocationFails=false;count=tasks.size();tick();assert(!running()&&tasks.size()==count);
 // A newer request survives a cancelled failed allocation.
 allocationFails=true;
 {std::lock_guard<std::mutex> lock(hooksMutex);duringCreate=[] {assert(!stop(0));resumeAfterMusicAudio();};}
 resumeAfterMusicAudio();tick();allocationFails=false;tick();assert(running());finish();
 puts("PASS cancelled failed admission stays cancelled; a later request survives");
 // Consume before allocation: stop+release cannot be erased by completion.
 {
 Gate gate;gate.install(duringCreate);resumeAfterMusicAudio();
 std::thread starter([]{tick();});gate.wait();
 auto result=suspendForMusicAudio(0);assert(result.poller==AudioSuspension::Poller::StopPending);
 resumeAfterMusicAudio();gate.release();starter.join();
 // It may still be Running (resume cancels stop) or Retiring (retry required).
 eventually([] {tick();return running()&&!stopRequested();});finish();
 }
 // Pending requests must also survive an irreversible retirement.
 {
 Gate gate;gate.install(duringUnsubscribe);assert(start());stop(0);gate.wait();
 resumeAfterMusicAudio();count=tasks.size();tick();assert(tasks.size()==count);
 gate.release();eventually([]{return !running();});joinAll();tick();assert(running());finish();
 }
 puts("PASS newer intent during startup and busy retirement survives for retry");
 queueReady=false;resumeAfterMusicAudio();tick();assert(running());finish();queueReady=true;
 // A new release remains a request; cancellation after it wins.
 resumeAfterMusicAudio();assert(suspendForMusicAudio(1000).uploadsReady);tick();assert(!running());
 assert(start());
 std::thread publisher([]{for(unsigned i=0;i<100000;++i)resumeAfterMusicAudio();});
 for(unsigned i=0;i<100000;++i)tick();publisher.join();tick();finish();
 puts("PASS optional queue, last-intent cancellation and 100000 concurrent requests/ticks");
 return false;
#endif
}
