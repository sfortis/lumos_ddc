#ifndef CAPTURE_H
#define CAPTURE_H

#include <windows.h>

/* TRUE while any application is holding the microphone or the camera open,
   which is how a video call looks to us: somebody is at the machine, watching
   and talking, without a single keystroke or mouse move arriving. */
BOOL Capture_InUse(void);

#endif /* CAPTURE_H */
