#include <math.h>       // don't forget to compile with -lm
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>    // don't forget to compile with -lpthread
#include <sys/time.h>
#include <X11/Xlib.h>   // don't forget to compile with -lX11

#define PI 3.14159265

struct CoasterConf {
  int triggerDistance;
  float friction;
};

/**
 *
 * Consts
 *
 */
int const COASTING_DELAY_USEC = 50000; // just a tad longer than once every two frames
int const COASTING_INTERVAL_USEC = 8333; // about twice per frame

// imagine pointer is coasting. The new touch doesn't provide enough events for full
// coast, so program will add new samples to existing [x|y]PointerSpeedRemaining
// that is, unless it's been this long since last pointer/scroll coast thing
int const COASTING_RESTART_AFTER_USEC = 500000;
double const MAX_COAST_ANGLE = 0.1;

/**
 *
 * Global variables
 *
 */
struct timeval lastPointerMoveTime, lastScrollTime;
double xPointerSpeedRemaining, yPointerSpeedRemaining, pointerSpeedMultiplier = 10.0, pointerFriction = 0.007, minMomentum = 42.0;
double currentPositionX, currentPositionY;
int currentPositionX_i, currentPositionY_i;
double scrollSpeed, scrollSpeedMultiplier = 10.0;
pthread_t threadBin;

// valuator data:
int valuatorSampleSize = 8;
int valuator;
int valuatorIndex[] = {0, 0, 0, 0};
float amount;
float movementSample[][8] = {
  {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
  {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
  {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
  {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}
};

// x display stuff
Display* dpy;
int scr;
Window rootWindow;

// preventScrollCoasting, preventPointerCoasting
int preventScrollCoasting = 1, preventPointerCoasting = 1;

// locks:
pthread_mutex_t pointerCoasterDataLock;
pthread_mutex_t scrollCoasterDataLock;

/**
 * 
 *   Utility functions
 *
 */

// Return a pointer to the (shifted) trimmed string
// stolen from https://stackoverflow.com/a/26984026/5117388
char *trim(char *s) {
  char *original = s;
  size_t len = 0;

  while (isspace((unsigned char) *s)) {
    s++;
  } 
  if (*s) {
    char *p = s;
    while (*p) p++;
    while (isspace((unsigned char) *(--p)));
    p[1] = '\0';
    len = (size_t) (p - s + 1);
  }

  return (s == original) ? s : memmove(original, s, len + 1);
}

// stolen from https://stackoverflow.com/a/4771038/5117388
int startsWith(const char *pre, const char *str) {
    size_t lenpre = strlen(pre),
           lenstr = strlen(str);
    return lenstr < lenpre ? 0 : memcmp(pre, str, lenpre) == 0;
}

int isTimeTooRecent(struct timeval* currentTime, struct timeval* mostRecentTime, int maxDiffUsec) {
  // returns '1' if the difference between times is less than usec

  if (currentTime->tv_sec - mostRecentTime->tv_sec > maxDiffUsec * 0.000001) {
    return 0;
  }
  if (currentTime->tv_usec - mostRecentTime->tv_usec > maxDiffUsec) {
    return 0;
  }

  return 1;
}

void initXorg() {
  dpy = XOpenDisplay(0);
  scr = XDefaultScreen(dpy);
  rootWindow = XRootWindow(dpy, scr);
}

// converts float to int
int f2i(float x) {
  return (int)(x+0.5);
}

/**
 * 
 *   Real stuff
 *
 */


// all angles use radians
int detectDirectionChange(double oldAvgX, double oldAvgY, double avgX, double avgY, double acceptableDeviationAngle) {
  double oldAngle = atan(oldAvgY / oldAvgX) + PI * 0.5;  // keep those values on [0,PI] instead
  double currentAngle = atan(avgY / avgX) + PI * 0.5;    // same here

  double thresholdMin = oldAngle - acceptableDeviationAngle;
  double thresholdMax = oldAngle + acceptableDeviationAngle;

  if (thresholdMin > 0) {
    if (thresholdMax < PI) {
      return currentAngle >= thresholdMin && currentAngle <= thresholdMax;
    } else {
      return currentAngle >= thresholdMin || currentAngle <= thresholdMax - PI;
    }
  } else {
    if (thresholdMax < PI) {
      return currentAngle >= thresholdMin + PI || currentAngle <= thresholdMax;
    } else {
      // this case really shoulnd't happen
      return currentAngle >= thresholdMin + PI || currentAngle <= thresholdMax - PI;
    }
  }
}

void* pointerCoaster(int* xSample, int xSampleSize, int* ySample, int ySampleSize) {
  preventPointerCoasting = 0;
  usleep(COASTING_DELAY_USEC);

  if (preventPointerCoasting) {
    printf("something is preventing pointer coasting. doing nothing");
  }

  struct timeval currentTime;
  double averageX = 0, averageY = 0;

  // stuff for X that we don't need
  Window rw;
  Window window_returned;
  int winx, winy;
  unsigned int mask_return;
  //

  gettimeofday(&currentTime, NULL);
  
  pthread_mutex_lock(&pointerCoasterDataLock);

  if (isTimeTooRecent(&currentTime, &lastPointerMoveTime, COASTING_INTERVAL_USEC)) {
    printf("Touchpad was moved too recently to start coasting!\n");
    pthread_mutex_unlock(&pointerCoasterDataLock);
    return;
  }
  printf("———————————— touchpad was moved just right! ————————\n");

  // let's calculate our speed
  int xs = 0, ys = 0;
  for (int i = 0; i < valuatorSampleSize; i++) {
    if (movementSample[0][i] != INFINITY) {
      averageX += (double)movementSample[0][i];
      ++xs;
    }
    if (movementSample[1][i] != INFINITY) {
      averageY += (double)movementSample[1][i];
      ++ys;
    }
  }

  // if we didn't get all samples, we only start coasting if direction is maintained
  // or if not enough time has passed.
  // if direction changes too much or too much time has passed, we either do nothing
  // or coast with incomplete data
  if (xs < 8 || ys < 8) {
    printf("check direction? %;", detectDirectionChange(xPointerSpeedRemaining, yPointerSpeedRemaining, averageX, averageY, MAX_COAST_ANGLE));
    if (isTimeTooRecent(&currentTime, &lastPointerMoveTime, COASTING_RESTART_AFTER_USEC) && !detectDirectionChange(xPointerSpeedRemaining, yPointerSpeedRemaining, averageX, averageY, MAX_COAST_ANGLE)) {
      printf("direction preserved: continuing coasting and preservign direction");
      int xSpeedWeight = valuatorSampleSize - xs;
      int ySpeedWeight = valuatorSampleSize - ys;
      
      averageX += xPointerSpeedRemaining * (double)xSpeedWeight;
      averageY += yPointerSpeedRemaining * (double)ySpeedWeight;

      xs = valuatorSampleSize;
      ys = valuatorSampleSize;
    }
  }

  if (!xs || !ys) {
    printf("Not enough samples to start coasting!\n");
    pthread_mutex_unlock(&pointerCoasterDataLock);
    return;
  }
  xPointerSpeedRemaining = averageX / (double)xs;
  yPointerSpeedRemaining = averageY / (double)ys;

  // Calculate the speed at which the pointer was last moving and turn it
  // into 'remaining speed'. 
  

  double momentum = sqrt((pow(averageX, 2) + pow(averageY, 2)));

  printf("Speed calculated:\n  <> averageX: %f\n  <> averageY: %f\n  <> momentum: %f\n  <> cutoff:  %f\n\n",averageX, averageY, momentum, minMomentum);

  if (momentum < minMomentum) {
    printf("Not enough momentum to start coasting!\n");
    pthread_mutex_unlock(&pointerCoasterDataLock);
    return;
  }

  // "first time setup" — initialize coasting direction
  XQueryPointer(dpy, rootWindow, &window_returned, &rw, &currentPositionX_i, &currentPositionY_i, &winx, &winy, &mask_return);
  printf("Enough momentum to scroll. initial position: %d,%d\n", currentPositionX_i, currentPositionY_i);

  // convert current position into something more accurate
  currentPositionX = (double) currentPositionX_i;
  currentPositionY = (double) currentPositionY_i;

  // clear sample only now that it's apparent we aren't going to scroll
  for (int i = 0; i < valuatorSampleSize; i++) {
    movementSample[0][i] = INFINITY;
    movementSample[1][i] = INFINITY;
  }

  pthread_mutex_unlock(&pointerCoasterDataLock);

  // Continue moving mouse until all the 'remaining speed' is consumed
  while (!preventPointerCoasting && (fabs(xPointerSpeedRemaining) > 0.1 || fabs(yPointerSpeedRemaining) > 0.1)) {
    if (isTimeTooRecent(&currentTime, &lastPointerMoveTime, COASTING_DELAY_USEC)) {
      printf("Touchpad was moved while scrolling. We'll stop scrolling.\n");
      return;
    }

    // calculate new position
    currentPositionX += xPointerSpeedRemaining;
    currentPositionY += yPointerSpeedRemaining;

    // move cursor to the new position. While we're moving the cursor, nobody else is allowed to 
    // interact with x server
    pthread_mutex_lock(&pointerCoasterDataLock);
    XWarpPointer(dpy, None, rootWindow, 0, 0, 0, 0, f2i(currentPositionX), f2i(currentPositionY));
    XFlush(dpy);
    pthread_mutex_unlock(&pointerCoasterDataLock);

    // apply friction
    xPointerSpeedRemaining -= xPointerSpeedRemaining * pointerFriction;
    yPointerSpeedRemaining -= yPointerSpeedRemaining * pointerFriction;

    usleep(COASTING_INTERVAL_USEC);
  }
  printf("——— [ not enough movement speed remaining to sustain scroll ] ———\nxPointerSpeedRemaining: %f\nyPointerSpeedRemaining: %f\n", xPointerSpeedRemaining, yPointerSpeedRemaining);
  return;
}

void* scrollCoaster(int* xSample, int xSampleSize, int* ySample, int ySampleSize) {

  return NULL;
}

void xinputChild(int* fds, char* deviceId) {
  close(fds[0]);     // we won't be needing parent read end of the pipe
  dup2(fds[1], 1);   // we connect stdout of this fork to the pipe
  close(fds[1]);     // we close the pipe from this end. 
  
  // We no longer have a fd to work with, but stdout should still get redirected to the pipe
  char* xinputArgs[] = {"~", "test-xi2", "--root", deviceId, NULL};
  execv("/usr/bin/xinput", xinputArgs);
}

void start(char* xinputDeviceId) {
  pthread_mutex_init(&pointerCoasterDataLock, NULL);
  pthread_mutex_init(&scrollCoasterDataLock, NULL);

  int fds[] = {-1, -1}; // [0] — parent read, [1] — child write
  pid_t childpid;  

  if (pipe(fds) < 0) {
    perror("Failed to create a pipe! Will prolly quit.\n");
    return;
  }

  childpid = fork();

  if (childpid < 0) {
    perror("Forking failed. Will prolly quit.\n");
    return;
  }

  if (childpid == 0) {
    xinputChild(fds, xinputDeviceId);
  } else {
    close(fds[1]);
    FILE* fstream = fdopen(fds[0], "r"); 

    // prepare input buffer that will hold one line. In general, we could get away with
    // _way_ smaller buffer, but with gigabytes of ram in our computers in $current_year
    // us overcompensating for that doesn't really matter
    char* buffer;
    char* trimmedBuffer;
    size_t bufsize = 256;
    size_t charactersRead; // how many characters did we have in the current line. -1 on eof

    buffer = (char*) malloc(bufsize * sizeof(char));
    if (buffer == NULL) {
      perror("Can't allocate character buffer. This is problematic enough to warrant program crash.");
      exit(1);
    }

    // variables that we'll use to track whether we're getting the event we want to get
    int isCorrectEvent = 0;
    int isValuator = 0;
    int mustFreeTrimmedBuffer = 0;

    // read one line from pipe
    while ((charactersRead = getline(&buffer, &bufsize, fstream)) != -1) {

      if (mustFreeTrimmedBuffer != 0) {
        free(trimmedBuffer);
        mustFreeTrimmedBuffer = 0;
      }
      trimmedBuffer = trim(buffer);
      mustFreeTrimmedBuffer = 1;

      // if we're inside of the 'valuator' section of the output, we don't need
      // to process anything else.
      if (isValuator) {
        if (!strlen(trimmedBuffer)) {    
          printf("valuator section ended. valuator? %d", valuator);
          // this means 'isValuator' section has ended. 
          isValuator = 0;

          // Let's write data and spin a thread for coaster
          // since valuators 0&1 and 2&3 never appear at the 
          // same time, we can look at the last valuator to 
          // determine whether we're scrolling or moving.
          if (valuator < 2) {
            gettimeofday(&lastPointerMoveTime, NULL);
            preventPointerCoasting = 1;

            // start thread that'll eventually start moving mouse
            // we don't want to wait for threads to end, either, so 
            // we don't do pthread_join()
            if (pthread_create(&threadBin, NULL, &pointerCoaster, NULL) != 0) {
              perror("error creating thread!");
            }
          } else if (valuator < 4) {
            gettimeofday(&lastScrollTime, NULL);
            preventScrollCoasting = 1;

            // start thread that'll eventually start moving mouse
            // we don't want to wait for threads to end, either, so 
            // we don't do pthread_join()
            if (pthread_create(&threadBin, NULL, &scrollCoaster, NULL) != 0) {
              perror("error creating thread!");
            }
          }

          // we keep the lock on until the all valuators are read
          pthread_mutex_unlock(&pointerCoasterDataLock);
          pthread_mutex_unlock(&scrollCoasterDataLock);

          continue;
        }

        // numbers in valuator lines appear to be ordered like this:
        //    valuator[int]: corrected movement[float] (raw movement[float])
        // we'll ignore raw movement
        sscanf(trimmedBuffer, "%d: %f (%*f)%*s", &valuator, &amount);

        printf("movement along valutaor %d: %f\n", valuator, amount);
        
        movementSample[valuator][valuatorIndex[valuator]] = amount;
        valuatorIndex[valuator] = ++valuatorIndex[valuator] % valuatorSampleSize;
        
        continue;
      }

      if (startsWith("EVENT type", trimmedBuffer)) {
        if (startsWith("EVENT type 17 (RawMotion)", trimmedBuffer)) {
          isCorrectEvent = 1;
          continue;
        } else {
          if (startsWith("EVENT type 15 (RawButtonPress)", trimmedBuffer)) {
            // this is a very special kind of "not correct" event
            // we kill coasting on touchpad tap
            preventPointerCoasting = 1;
            preventScrollCoasting = 1;
          }
          isCorrectEvent = 0;
          continue;
        }
      }

      // if we didn't get the correct event, we don't process any lines until we do
      if (!isCorrectEvent) {
        continue;
      }

      // if we do have the correct event, check if the line is 'valuator:'
      if (startsWith("valuators:", trimmedBuffer)) {
        isValuator = 1;

        // nothing can touch valuator data while we're in the 'valuator' section
        pthread_mutex_lock(&pointerCoasterDataLock);
        pthread_mutex_lock(&scrollCoasterDataLock);
      }

      free(trimmedBuffer);
    };
    printf("Program ended.\n");
  }
}

int main(int argc, char* argv[]) {
  initXorg();

  start(argv[1]);
}

