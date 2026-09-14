#define _GNU_SOURCE
#include <sched.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <physical.h>
#include <linux/input.h>
#include "address_map_arm.h"
#include "defines.h"
#include <pthread.h>

/**  your part 3 user code here  **/ 
// For stopping program
volatile sig_atomic_t stop = 0;
void catchSIGINT(int);

/*Global constants*/
// 13 tones: middle C chromatic scale (C4 .. C5)
const double tone_rps[13] = {
    MIDC,   // C
    DFLAT,  // C# / Db
    DNAT,   // D
    EFLAT,  // D# / Eb
    ENAT,   // E
    FNAT,   // F
    GFLAT,  // F# / Gb
    GNAT,   // G
    AFLAT,  // G# / Ab
    ANAT,   // A
    BFLAT,  // A# / Bb
    BNAT,   // B
    HIC     // high C
};

#define NUM_TONES 13

// Shared between main + audio thread
int tone_volume[NUM_TONES];       // current volume (0 to BASE_VOL)
int key_down[NUM_TONES];          // 1 if key is currently pressed
double tone_fade_factor[NUM_TONES]; // per-tone fade factor

pthread_mutex_t mutex_tone_volume = PTHREAD_MUTEX_INITIALIZER;
const int BASE_VOL = 0x7FFFFFFF / NUM_TONES;     // Map each '1'/'0' to a tone volume to avoid overflow, each tone's max volume is 0x7FFFFFFF / 13
const double GLOBAL_FADE = 0.9995; // Fade factor: how fast notes decay when key released

// Keyboard event values
#define KEY_RELEASED 0
#define KEY_PRESSED  1

static int keycode_to_tone_index(unsigned short code) {
    switch (code) {
        case KEY_Q: return 0;  // C
        case KEY_2: return 1;  // C#
        case KEY_W: return 2;  // D
        case KEY_3: return 3;  // D#
        case KEY_E: return 4;  // E
        case KEY_R: return 5;  // F
        case KEY_5: return 6;  // F#
        case KEY_T: return 7;  // G
        case KEY_6: return 8;  // G#
        case KEY_Y: return 9;  // A
        case KEY_7: return 10; // A#
        case KEY_U: return 11; // B
        case KEY_I: return 12; // high C
        default:    return -1;
    }
}

// Write ONE sample = sum of all enabled tones
static void write_chord_sample(volatile int *AUDIO_PTR, int nth_sample) {
    double sample_val = 0.0;
    int i;

    pthread_mutex_lock(&mutex_tone_volume);
    // Sum all active tones
    for (i = 0; i < NUM_TONES; i++) {
        if (tone_volume[i] != 0) {
            double theta = (double)nth_sample * tone_rps[i]; // radians
            sample_val += (double)tone_volume[i] * sin(theta);
        }
    }
    pthread_mutex_unlock(&mutex_tone_volume);

    int sample = (int)sample_val;

    // Wait for space in both left and right FIFOs
    while (1) {
        int fifospace = *(AUDIO_PTR + 1);
        int wslc = (fifospace & 0xFF000000) >> 24;
        int wsrc = (fifospace & 0x00FF0000) >> 16;

        if (wslc > 0 && wsrc > 0)
            break;
        // otherwise spin until hardware consumes some samples
    }
    *(AUDIO_PTR + 2) = sample; // left
    *(AUDIO_PTR + 3) = sample; // right
}

void *audio_thread(void *arg) {

    (void)arg;
    
    int fd = -1;               // used to open /dev/mem for access to physical addresses
    void *LW_virtual;          // used to map physical addresses for the light-weight bridge
    volatile int * AUDIO_PTR;

    // Create virtual memory access to the FPGA light-weight bridge
    if ((fd = open_physical (fd)) == -1)
        return NULL;
    if ((LW_virtual = map_physical (fd, LW_BRIDGE_BASE, LW_BRIDGE_SPAN)) == NULL) {
        close_physical(fd);
        return NULL;
    }
        
    // Setting pointers using virtual memory
    AUDIO_PTR = (int *) (LW_virtual + AUDIO_BASE);

    // Clear audio FIFOs (CW bit), then turn off clear
    *(AUDIO_PTR + 0) = 0b1000;  // set CW = 1
    *(AUDIO_PTR + 0) = 0x0;     // clear CW

    int n = 0;
    while(1) {
        // Check if this thread has been cancelled
        pthread_testcancel();
        
        // 1) Write one audio sample based on current tone_volume
        write_chord_sample(AUDIO_PTR, n);
        n++;

        // 2) Apply fade to any tones whose key is NOT currently pressed
        pthread_mutex_lock(&mutex_tone_volume);
        int i;
        for (i = 0; i < NUM_TONES; i++) {
            if (!key_down[i] && tone_volume[i] > 0) {
                double v = (double)tone_volume[i] * tone_fade_factor[i];
                if (v < 1.0)
                    tone_volume[i] = 0;
                else
                    tone_volume[i] = (int)v;
            }
            // If key_down[i] == 1, we leave volume as set by main thread
        }
        pthread_mutex_unlock(&mutex_tone_volume);
    }

    unmap_physical(LW_virtual, LW_BRIDGE_SPAN);
    close_physical(fd);
    return NULL;
}


int main (int argc, char *argv[]) {
    int i;
    pthread_t tid;
    int err;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <path-to-keyboard-device(in /dev/input/by-id/)>\n", argv[0]);
        return 1;
    }

    char *kbd_path = argv[1];

    // Init shared state
    for (i = 0; i < NUM_TONES; i++) {
        tone_volume[i] = 0;
        key_down[i] = 0;
        tone_fade_factor[i] = GLOBAL_FADE;
    }

    //catch SIGINT from ctrl+c, instead of having it abruptly close this program
    signal(SIGINT, catchSIGINT);

    // Spawn audio thread
    if ((err = pthread_create(&tid, NULL, &audio_thread, NULL)) != 0) {
        printf("pthread_create failed: [%s]\n", strerror(err));
        return 1;
    }

    // Open keyboard device (non-blocking)
    int kfd;
    if ((kfd = open(kbd_path, O_RDONLY | O_NONBLOCK)) == -1) {
        perror("Could not open keyboard device");
        // Cancel audio thread and exit
        pthread_cancel(tid);
        pthread_join(tid, NULL);
        return 1;
    }

    struct input_event ev;
    int event_size = sizeof(struct input_event);

    // Main loop to read keyboard and adjust tone_volume[]
    while (!stop) {
        ssize_t bytes = read(kfd, &ev, event_size);
        if (bytes < event_size) {
            // No event, just continue
            usleep(1000); // small sleep to avoid busy spinning
            continue;
        }

        if (ev.type == EV_KEY) {
            int tone_idx = keycode_to_tone_index(ev.code);
            if (tone_idx < 0 || tone_idx >= NUM_TONES)
                continue; // not one of the piano keys

            if (ev.value == KEY_PRESSED) {
                printf("PRESSED  0x%04x\n", (int)ev.code);
                fflush(stdout);
                // Key pressed: set volume to max, mark key_down
                pthread_mutex_lock(&mutex_tone_volume);
                key_down[tone_idx] = 1;
                tone_volume[tone_idx] = BASE_VOL;
                pthread_mutex_unlock(&mutex_tone_volume);
            }
            else if (ev.value == KEY_RELEASED) {
                printf("RELEASED 0x%04x\n", (int)ev.code);
                fflush(stdout);
                // Key released: mark key_down = 0, audio thread will fade
                pthread_mutex_lock(&mutex_tone_volume);
                key_down[tone_idx] = 0;
                pthread_mutex_unlock(&mutex_tone_volume);
            }
        }
    }

    // Clean shutdown that cancel + join audio thread
    pthread_cancel(tid);
    pthread_join(tid, NULL);

    close(kfd);

    printf ("\nExiting Part 3 program\n");
    return 0;
}


/* Function to allow clean exit of the program */
void catchSIGINT(int signum) {
    (void)signum;
	stop = 1;
}