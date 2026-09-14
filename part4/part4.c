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
#include <pthread.h>
#include <string.h>

#include "address_map_arm.h"
#include "video.h"
#include "defines.h"

/**  your part 4 user code here  **/

// For stopping program (main thread)
volatile sig_atomic_t stop = 0;
void catchSIGINT(int);

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

// Shared between main + audio + video threads
int    tone_volume[NUM_TONES];        // current volume (0 .. BASE_VOL)
int    key_down[NUM_TONES];           // 1 if key is currently pressed
double tone_fade_factor[NUM_TONES];   // per-tone fade factor

pthread_mutex_t mutex_tone_volume = PTHREAD_MUTEX_INITIALIZER;

const int BASE_VOL = 0x7FFFFFFF / NUM_TONES; // so all 13 maxed still fit 32-bit signed
const double GLOBAL_FADE = 0.9995;                 // fade factor for released keys

// Keyboard event values (Linux input)
#define KEY_RELEASED 0
#define KEY_PRESSED  1

// Video redraw request flag
volatile int video_request = 0;

// Setting processor affinity
int set_processor_affinity(unsigned int core) {
    cpu_set_t cpuset;
    pthread_t current_thread = pthread_self();

    if (core >= (unsigned int)sysconf(_SC_NPROCESSORS_ONLN)) {
        printf("CPU Core %u does not exist!\n", core);
        return -1;
    }

    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);

    return pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
}

// Mapping keyboard to tone
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

    // Sum up all active tones (protected by mutex)
    pthread_mutex_lock(&mutex_tone_volume);
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

    // Write same sample to left and right
    *(AUDIO_PTR + 2) = sample;
    *(AUDIO_PTR + 3) = sample;
}

// Audio thread
void *audio_thread(void *arg) {
    (void)arg;

    // Pin audio thread to core 1 (if available)
    set_processor_affinity(1);

    int fd = -1;
    void *LW_virtual;
    volatile int *AUDIO_PTR;

    // Map LW bridge
    if ((fd = open_physical(fd)) == -1)
        return NULL;

    if ((LW_virtual = map_physical(fd, LW_BRIDGE_BASE, LW_BRIDGE_SPAN)) == NULL) {
        close_physical(fd);
        return NULL;
    }

    AUDIO_PTR = (int *)(LW_virtual + AUDIO_BASE);

    // Clear audio FIFOs (CW bit)
    *(AUDIO_PTR + 0) = 0b1000; // set CW=1
    *(AUDIO_PTR + 0) = 0x0;    // clear CW

    int n = 0;
    while (1) {
        // Check if this thread has been cancelled
        pthread_testcancel();

        // 1) Write one audio sample
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
        }
        pthread_mutex_unlock(&mutex_tone_volume);
    }

    unmap_physical(LW_virtual, LW_BRIDGE_SPAN);
    close_physical(fd);
    return NULL;
}

// Drawing waveform
static void draw_waveform_for_current_chord(int cols, int rows) {
    int i;

    // Snapshot current volumes under mutex to avoid holding it for full draw
    int local_vol[NUM_TONES];
    pthread_mutex_lock(&mutex_tone_volume);
    for (i = 0; i < NUM_TONES; i++)
        local_vol[i] = tone_volume[i];
    pthread_mutex_unlock(&mutex_tone_volume);

    // Check if all tones are silent
    int all_zero = 1;
    for (i = 0; i < NUM_TONES; i++) {
        if (local_vol[i] != 0) {
            all_zero = 0;
            break;
        }
    }

    video_clear();

    int mid_y = rows / 2;

    // Draw horizontal midline
    //video_line(0, mid_y, cols - 1, mid_y, video_WHITE);

    if (all_zero) {
        // Nothing playing; just show flat line
        video_show();
        return;
    }

    // Draw waveform: x = sample index, y = amplitude
    int prev_x = 0;
    int prev_y = mid_y;

    int x;
    for (x = 0; x < cols; x++) {
        int n = x;  // treat each column as one audio sample
        double sample_val = 0.0;

        for (i = 0; i < NUM_TONES; i++) {
            if (local_vol[i] != 0) {
                double theta = (double)n * tone_rps[i];
                sample_val += (double)local_vol[i] * sin(theta);
            }
        }

        // Normalize to [-1, 1] using max possible amplitude 0x7FFFFFFF
        double norm = sample_val / (double)0x7FFFFFFF;
        if (norm > 1.0)  norm = 1.0;
        if (norm < -1.0) norm = -1.0;

        int amp = (int)(norm * (rows / 2 - 2));
        int y = mid_y - amp;

        if (y < 0) y = 0;
        if (y >= rows) y = rows - 1;

        if (x == 0)
            video_pixel(x, y, video_GREEN);
        else
            video_line(prev_x, prev_y, x, y, video_GREEN);

        prev_x = x;
        prev_y = y;
    }

    video_show();
}

// Video thread
void *video_thread(void *arg) {
    (void)arg;

    // Pin video thread to core 0 (same as main)
    set_processor_affinity(0);

    if (!video_open()) {
        fprintf(stderr, "ERROR: could not open video device\n");
        return NULL;
    }

    int cols, rows, tcols, trows;
    if (!video_read(&cols, &rows, &tcols, &trows)) {
        fprintf(stderr, "ERROR: video_read failed\n");
        video_close();
        return NULL;
    }

    video_clear();
    video_show();

    while (1) {
        pthread_testcancel();

        if (!video_request) {
            // No redraw requested, sleep briefly to avoid busy-spin
            usleep(5000);
            continue;
        }

        // Consume the request
        video_request = 0;

        // Draw waveform for the current chord
        draw_waveform_for_current_chord(cols, rows);
    }

    video_close();
    return NULL;
}

int main(int argc, char *argv[]) {
    int i;
    pthread_t audio_tid, video_tid;
    int err;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <path-to-keyboard-device (in /dev/input/by-id/)>\n", argv[0]);
        return 1;
    }
    char *kbd_path = argv[1];

    // Pin main thread to core 0
    set_processor_affinity(0);

    // Init shared state
    for (i = 0; i < NUM_TONES; i++) {
        tone_volume[i] = 0;
        key_down[i] = 0;
        tone_fade_factor[i] = GLOBAL_FADE;
    }

    signal(SIGINT, catchSIGINT);

    // Spawn audio thread
    if ((err = pthread_create(&audio_tid, NULL, &audio_thread, NULL)) != 0) {
        printf("pthread_create (audio) failed: [%s]\n", strerror(err));
        return 1;
    }

    // Spawn video thread
    if ((err = pthread_create(&video_tid, NULL, &video_thread, NULL)) != 0) {
        printf("pthread_create (video) failed: [%s]\n", strerror(err));
        pthread_cancel(audio_tid);
        pthread_join(audio_tid, NULL);
        return 1;
    }

    // Open keyboard device (non-blocking)
    int kfd;
    if ((kfd = open(kbd_path, O_RDONLY | O_NONBLOCK)) == -1) {
        perror("Could not open keyboard device");
        pthread_cancel(audio_tid);
        pthread_cancel(video_tid);
        pthread_join(audio_tid, NULL);
        pthread_join(video_tid, NULL);
        return 1;
    }

    struct input_event ev;
    int event_size = sizeof(struct input_event);

    // Main loop to read keyboard and adjust tone_volume[]
    while (!stop) {
        ssize_t bytes = read(kfd, &ev, event_size);
        if (bytes < event_size) {
            // No event
            usleep(1000);
            continue;
        }

        if (ev.type == EV_KEY) {
            int tone_idx = keycode_to_tone_index(ev.code);

            if (ev.value == KEY_PRESSED) {
                printf("PRESSED  0x%04x\n", (int)ev.code);
            } else if (ev.value == KEY_RELEASED) {
                printf("RELEASED 0x%04x\n", (int)ev.code);
            }

            if (tone_idx < 0 || tone_idx >= NUM_TONES)
                continue; // not a piano key

            if (ev.value == KEY_PRESSED) {
                pthread_mutex_lock(&mutex_tone_volume);
                key_down[tone_idx]   = 1;
                tone_volume[tone_idx] = BASE_VOL;
                pthread_mutex_unlock(&mutex_tone_volume);
            }
            else if (ev.value == KEY_RELEASED) {
                pthread_mutex_lock(&mutex_tone_volume);
                key_down[tone_idx] = 0;
                pthread_mutex_unlock(&mutex_tone_volume);
            }

            // Request waveform redraw on every press/release
            video_request = 1;
        }
    }

    // Clean shutdown
    pthread_cancel(audio_tid);
    pthread_cancel(video_tid);
    pthread_join(audio_tid, NULL);
    pthread_join(video_tid, NULL);

    close(kfd);

    printf("\nExiting Part 4 program\n");
    return 0;
}

/* Function to allow clean exit of the program */
void catchSIGINT(int signum) {
    (void)signum;
    stop = 1;
}