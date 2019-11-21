#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>    // don't forget to compile with -lpthread
#include <sys/time.h>
#include <X11/Xlib.h>   // don't forget to compile with -lX11

struct CoasterConf {
  int triggerDistance;
  float friction;
};

/**
 *
 * Consts
 *
 */
int const coastingDelayUsec = 35000; // just a tad longer than once every two frames
int const coastingIntervalUsec = 8333; // about twice per frame

/**
 *
 * Global variables
 *
 */
struct timeval lastPointerMoveTime, lastScrollTime;
float xPointerSpeedRemaining, yPointerSpeedRemaining, pointerSpeedMultiplier = 10.0f;
float currentPositionX, currentPositionY;
int currentPositionX_i, currentPositionY_i;
float scrollSpeed, scrollSpeedMultiplier = 10.0f;
pthread_t threadBin;

// valuator data:
// other variables
int valuatorSampleSize = 4;
int valuator;
int valuatorIndex[] = {0, 0, 0, 0};
float amount;
float movementSample[][4] = {
  {0.0f, 0.0f, 0.0f, 0.0f},
  {0.0f, 0.0f, 0.0f, 0.0f},
  {0.0f, 0.0f, 0.0f, 0.0f},
  {0.0f, 0.0f, 0.0f, 0.0f}
};

// x display stuff
Display* dpy;
int scr;
Window rootWindow;

      

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

  printf("differnce in seconds: %d; maxDiff: %d\n", currentTime->tv_sec - mostRecentTime->tv_sec, maxDiffUsec*0.000001);
  printf("differnce in useconds: %d; maxDiff: %d\n", currentTime->tv_usec - mostRecentTime->tv_usec, maxDiffUsec);

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


void* pointerCoaster(int* xSample, int xSampleSize, int* ySample, int ySampleSize) {
  usleep(coastingDelayUsec);

  struct timeval currentTime;

  // stuff for X that we don't need
  Window rw;
  Window window_returned;
  int winx, winy;
  unsigned int mask_return;
  //

  gettimeofday(&currentTime, NULL);
  
  pthread_mutex_lock(&pointerCoasterDataLock);

  if (isTimeTooRecent(&currentTime, &lastPointerMoveTime, coastingIntervalUsec)) {
    printf("Touchpad was moved too recently to start coasting!\n");
    pthread_mutex_unlock(&pointerCoasterDataLock);
    return;
  }
  printf("———————————— touchpad was moved just right! ————————\n");

  // "first time setup" — initialize coasting direction
  XQueryPointer(dpy, rootWindow, &window_returned, &rw, &currentPositionX_i, &currentPositionY_i, &winx, &winy, &mask_return);
  printf("initial position: %d,%d\n", currentPositionX_i, currentPositionY_i);

  pthread_mutex_unlock(&pointerCoasterDataLock);

  // while (true) {
    if (isTimeTooRecent(&currentTime, &lastPointerMoveTime, coastingIntervalUsec)) {
      printf("Touchpad was moved while scrolling. We'll stop scrolling.\n");
      return;
    }

    // XWarpPointer(dpy, None, rootWindow, 0, 0, 0, 0, f2i(currentPositionX), f2i(currentPositionY));
    // XFlush(dpy);

    usleep(coastingIntervalUsec);
  // }
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

    // read one line from pipe
    while ((charactersRead = getline(&buffer, &bufsize, fstream)) != -1) {

      trimmedBuffer = trim(buffer);

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

            // start thread that'll eventually start moving mouse
            // we don't want to wait for threads to end, either, so 
            // we don't do pthread_join()
            if (pthread_create(&threadBin, NULL, &pointerCoaster, NULL) != 0) {
              perror("error creating thread!");
            }
          } else if (valuator < 4) {
            gettimeofday(&lastScrollTime, NULL);

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

        
        continue;
      }

      if (startsWith("EVENT type", trimmedBuffer)) {
        if (startsWith("EVENT type 17 (RawMotion)", trimmedBuffer)) {
          isCorrectEvent = 1;
          continue;
        } else {
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

    };
    printf("Program ended.\n");
  }
}

int main(void) {
  initXorg();

  start("12");
}

