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
#include <linux/input.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>

#include "address_map_arm.h"
#include "video.h"
#include "defines.h"
#include "stopwatch.h"
#include "KEY.h"
#include "HEX.h"
#include "LEDR.h"
#include "audio.h"

/**  Part 6 user code  **/

// For stopping program (main thread)
volatile sig_atomic_t stop = 0;
void catchSIGINT(int);

// 13 tones: middle C chromatic scale (C4 to C5)
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

#define NUM_TONES   13

// Shared between main + audio + video threads
int    tone_volume[NUM_TONES];        // current volume (0 to BASE_VOL)
int    key_down[NUM_TONES];           // 1 if key is currently pressed
double tone_fade_factor[NUM_TONES];   // per-tone fade factor

pthread_mutex_t mutex_tone_volume = PTHREAD_MUTEX_INITIALIZER;

const int    BASE_VOL    = 0x7FFFFFFF / NUM_TONES; // safe per-tone max
const double GLOBAL_FADE = 0.9995;                 // fade for released notes

// Keyboard event values (Linux EV_KEY)
#define KEY_RELEASED 0
#define KEY_PRESSED  1

// Video redraw request flag
volatile int video_request = 0;

// Recording data structure
#define MAX_EVENTS 10000  // max recorded events
#define RECORD_LIMIT_CS (59*100)  // 59 seconds in centiseconds

typedef struct {
    int time_cs;           // timestamp in centiseconds (1/100 s)
    int tone_idx;          // which tone (0..12)
    int pressed;           // 1 = key press, 0 = key release
} NoteEvent;

NoteEvent events[MAX_EVENTS];
int num_events = 0;

// Recording / playback state
int recording = 0;
int playing = 0;
int playback_index = 0;

// LED state (bit 0 = LEDR0, bit 1 = LEDR1)
int led_state = 0;

// For KEY edge detection
int prev_key_val = 0;

// Software timing (ms since epoch, via gettimeofday)
long long record_start_ms   = 0;  // start of recording (for timestamps + countdown)
long long playback_start_ms = 0;  // start of playback (for scheduling)

int last_record_display_cs = -1;  // last centisecond value shown on HEX (recording)

// Playback timing state (software timer, independent of stopwatch)
static int playback_lead_cs = 0;  // delay between record start and first note, in centiseconds

// Audio character device path
#define AUDIO_CHAR_DEV "/dev/IntelFPGAUP/audio"

// CPU affinity helper-
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

// Software timer helper (milliseconds)
static long long get_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + (long long)(tv.tv_usec / 1000LL);
}

// Keyboard mapping: keycode to tone index
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

// Helper: LEDs using LEDR wrappers
static void led_update(void) {
    // LEDR_set writes the full 10-bit LED value
    LEDR_set(led_state);
    printf("LED update: led_state = 0x%X\n", led_state);
}

// Turn LEDR0 on/off (recording)
static void led_record_on(void)  { led_state |= 0x1; led_update(); }
static void led_record_off(void) { led_state &= ~0x1; led_update(); }

// Turn LEDR1 on/off (playback)
static void led_play_on(void)    { led_state |= 0x2; led_update(); }
static void led_play_off(void)   { led_state &= ~0x2; led_update(); }

// Audio helper: write one mixed sample via AUDIO CHAR DEVICE

static void write_chord_sample_char(int audio_fd, int nth_sample) {
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

    // 1) Ask driver to wait until there is space in the FIFOs
    const char *wait_cmd = "waitw\n";
    if (write(audio_fd, wait_cmd, strlen(wait_cmd)) < 0) {
        perror("ERROR: write waitw to audio device");
        return;
    }

    // 2) Send left and right samples as text commands
    char buf[64];
    int len;

    len = snprintf(buf, sizeof(buf), "left %d\n", sample);
    if (len > 0) {
        if (write(audio_fd, buf, len) < 0) {
            perror("ERROR: write left sample");
            return;
        }
    }

    len = snprintf(buf, sizeof(buf), "right %d\n", sample);
    if (len > 0) {
        if (write(audio_fd, buf, len) < 0) {
            perror("ERROR: write right sample");
            return;
        }
    }
}

// Audio thread: uses /dev/IntelFPGAUP/audio
void *audio_thread(void *arg) {
    (void)arg;

    // Pin audio thread to core 1 (if available)
    set_processor_affinity(1);

    // Open the audio character device
    int audio_fd = open(AUDIO_CHAR_DEV, O_WRONLY);
    if (audio_fd < 0) {
        perror("ERROR: could not open /dev/IntelFPGAUP/audio");
        return NULL;
    }

    // Clear audio FIFOs via "init" command
    const char *init_cmd = "init\n";
    if (write(audio_fd, init_cmd, strlen(init_cmd)) < 0) {
        perror("ERROR: write init to audio device");
    }

    int n = 0;
    while (1) {
        // Check if this thread has been cancelled
        pthread_testcancel();

        // 1) Write one audio sample via the audio driver
        write_chord_sample_char(audio_fd, n);
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

    close(audio_fd);
    return NULL;
}

// Video: draw waveform for current chord
static void draw_waveform_for_current_chord(int cols, int rows) {
    int i;

    // Snapshot current volumes under mutex
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
        video_show();
        return;
    }

    // Draw waveform: x = sample index, y = amplitude
    int prev_x = 0;
    int prev_y = mid_y;

    int x;
    for (x = 0; x < cols; x++) {
        int n = x;
        double sample_val = 0.0;

        for (i = 0; i < NUM_TONES; i++) {
            if (local_vol[i] != 0) {
                double theta = (double)n * tone_rps[i];
                sample_val += (double)local_vol[i] * sin(theta);
            }
        }

        // Normalize to [-1, 1] using max possible amplitude
        double norm = sample_val / (double)0x7FFFFFFF;
        if (norm > 1.0)  norm = 1.0;
        if (norm < -1.0) norm = -1.0;

        int amp = (int)(norm * (rows / 2 - 2));
        int y   = mid_y - amp;

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
            usleep(5000);
            continue;
        }

        video_request = 0;

        draw_waveform_for_current_chord(cols, rows);
    }

    video_close();
    return NULL;
}

// Recording / playback helpers (using get_ms)
static void update_recording_countdown(void);  // forward decl

static void start_recording(void) {
    printf("DEBUG: start_recording() called\n");

    recording = 1;
    playing   = 0;
    num_events = 0;
    playback_index = 0;

    // Reset all tones
    pthread_mutex_lock(&mutex_tone_volume);
    int i;
    for (i = 0; i < NUM_TONES; i++) {
        tone_volume[i] = 0;
        key_down[i]    = 0;
    }
    pthread_mutex_unlock(&mutex_tone_volume);

    // Software start time for timestamps + countdown
    record_start_ms = get_ms();

    // Initialize countdown display to 00:59:00
    last_record_display_cs = RECORD_LIMIT_CS;
    int total_sec = RECORD_LIMIT_CS / 100;
    int centis    = RECORD_LIMIT_CS % 100;
    int minutes   = total_sec / 60;
    int seconds   = total_sec % 60;
    stopwatch_set(minutes, seconds, centis);
    stopwatch_display();

    led_record_on();
    printf("RECORDING STARTED\n");
}

static void stop_recording(void) {
    printf("DEBUG: stop_recording() called\n");

    recording = 0;
    led_record_off();
    last_record_display_cs = -1;

    // Turn off HEX display
    stopwatch_nodisplay();

    printf("RECORDING STOPPED, %d events\n", num_events);
}

// Return elapsed time since playback_start_ms in centiseconds (1/100 s)
static int playback_elapsed_cs(void) {
    long long now_ms = get_ms();
    long long diff_ms = now_ms - playback_start_ms;
    if (diff_ms < 0)
        diff_ms = 0;

    // 10 ms = 1 centisecond
    return (int)(diff_ms / 10LL);
}

// Convenience: write a mm:ss:dd time to the HEX via stopwatch driver,
// where cs is a number of centiseconds (0..5999 = 59.99 s).
static void stopwatch_set_from_cs(int cs) {
    if (cs < 0)
        cs = 0;
    if (cs > 5999)
        cs = 5999;  // clamp to 59.99

    int total_sec = cs / 100;
    int MM = total_sec / 60;
    int SS = total_sec % 60;
    int DD = cs % 100;

    stopwatch_set(MM, SS, DD);
}

static void start_playback(void) {
    printf("DEBUG: start_playback() called\n");

    if (num_events == 0) {
        printf("No events recorded yet.\n");
        return;
    }

    // Find time (in cs) of the FIRST key-press event.
    int first_press_cs = 0;
    int found = 0;
    int i;
    for (i = 0; i < num_events; i++) {
        if (events[i].pressed) {  // 1 = key press
            first_press_cs = events[i].time_cs;
            found = 1;
            break;
        }
    }
    if (!found) {
        // Shouldn't really happen, but be safe.
        printf("DEBUG: No 'pressed' events found; using delay 0cs\n");
        first_press_cs = 0;
    }

    playback_lead_cs = first_press_cs;

    if (playback_lead_cs > 0) {
        printf("DEBUG: playback_lead_cs = %d cs (%.2f s)\n",
               playback_lead_cs, playback_lead_cs / 100.0);
    } else {
        printf("DEBUG: playback_lead_cs = 0 cs (no pre-note delay)\n");
    }

    playing   = 1;
    recording = 0;
    playback_index = 0;

    // Reset tones before playback
    pthread_mutex_lock(&mutex_tone_volume);
    for (i = 0; i < NUM_TONES; i++) {
        tone_volume[i] = 0;
        key_down[i]    = 0;
    }
    pthread_mutex_unlock(&mutex_tone_volume);

    // Start software playback timer (independent of stopwatch device)
    playback_start_ms = get_ms();

    // Initialize HEX display for playback: show countdown starting value
    stopwatch_set_from_cs(playback_lead_cs);
    stopwatch_display(); // make sure HEX is ON

    led_play_on();

    printf("PLAYBACK STARTED\n");
}

static void stop_playback(void) {
    printf("DEBUG: stop_playback() called\n");

    // No more scheduling of playback events
    playing = 0;

    // Turn off HEX display
    stopwatch_nodisplay();

    // Turn off playback LED
    led_play_off();

    // Release all keys, but DO NOT zero out tone_volume.
    // The audio thread will see key_down == 0 and fade out any
    // remaining volume smoothly using GLOBAL_FADE and tone_fade_factor[].
    pthread_mutex_lock(&mutex_tone_volume);
    int i;
    for (i = 0; i < NUM_TONES; i++) {
        key_down[i] = 0;   // all keys released
        // tone_volume[i] stays same so it can fade naturally
    }
    pthread_mutex_unlock(&mutex_tone_volume);

    // Ask video thread to redraw so that waveform settles
    video_request = 1;

    printf("PLAYBACK STOPPED\n");
}

// Called whenever we see a piano key event while recording
static void record_event_if_needed(int tone_idx, int pressed) {
    if (!recording)
        return;
    if (tone_idx < 0 || tone_idx >= NUM_TONES)
        return;
    if (num_events >= MAX_EVENTS)
        return;

    long long now_ms = get_ms();
    int t_cs = (int)((now_ms - record_start_ms) / 10); // 10ms per centisecond

    events[num_events].time_cs  = t_cs;
    events[num_events].tone_idx = tone_idx;
    events[num_events].pressed  = pressed;
    num_events++;

    printf("DEBUG: recorded event #%d: t=%dcs tone=%d pressed=%d\n", num_events, t_cs, tone_idx, pressed);
}

// Apply recorded events according to current software time
static void handle_playback_step(void) {
    if (!playing || num_events == 0)
        return;

    // 1) Use software timer for playback scheduling
    int elapsed_cs = playback_elapsed_cs();

    // 1A: schedule audio events based on elapsed_cs
    while (playback_index < num_events &&
           events[playback_index].time_cs <= elapsed_cs) {

        NoteEvent *e = &events[playback_index];

        pthread_mutex_lock(&mutex_tone_volume);
        if (e->pressed) {
            key_down[e->tone_idx]    = 1;
            tone_volume[e->tone_idx] = BASE_VOL;
        } else {
            key_down[e->tone_idx] = 0;
        }
        pthread_mutex_unlock(&mutex_tone_volume);

        video_request = 1;

        printf("DEBUG: playback event #%d at t=%dcs: tone=%d pressed=%d\n",
               playback_index, e->time_cs, e->tone_idx, e->pressed);

        playback_index++;
    }

    // 2) Update HEX display for playback behavior
    if (elapsed_cs <= playback_lead_cs) {
        // Phase 1: count down from recorded pre-note delay to 0.
        int remaining_cs = playback_lead_cs - elapsed_cs;
        stopwatch_set_from_cs(remaining_cs);
        stopwatch_display();
    } else if (playback_index < num_events) {
        // Phase 2: after countdown hits 0, while playback is still running,
        // loop between 0.99 and 0.00 on HEX until recording finishes.

        int after_lead_cs = elapsed_cs - playback_lead_cs;

        // Toggle every 0.1 s (10 centiseconds) between 0.99 and 0.00
        if (((after_lead_cs / 10) % 2) == 0) {
            // show 0.99
            stopwatch_set_from_cs(99);   // 0.99 seconds
        } else {
            // show 0.00
            stopwatch_set_from_cs(0);    // 0.00 seconds
        }
        stopwatch_display();
    }

    // 3) Stop playback when all events consumed
    if (playback_index >= num_events) {
        stop_playback();
    }
}

// Update HEX countdown while recording; auto-stop at 0
static void update_recording_countdown(void) {
    if (!recording)
        return;

    long long now_ms = get_ms();
    long long elapsed_ms = now_ms - record_start_ms;
    if (elapsed_ms < 0) elapsed_ms = 0;

    int elapsed_cs   = (int)(elapsed_ms / 10); // centiseconds
    int remaining_cs = RECORD_LIMIT_CS - elapsed_cs;

    if (remaining_cs <= 0) {
        // Time up: stop recording, clear display
        stop_recording();
        return;
    }

    if (remaining_cs == last_record_display_cs) {
        return;
    }

    last_record_display_cs = remaining_cs;

    int total_sec = remaining_cs / 100;
    int centis    = remaining_cs % 100;
    int minutes   = total_sec / 60;
    int seconds   = total_sec % 60;

    stopwatch_set(minutes, seconds, centis);
    stopwatch_display();
}

int main(int argc, char *argv[]) {
    int i;
    pthread_t audio_tid, video_tid;
    int err;

    if (argc != 2) {
        fprintf(stderr,"Usage: %s <path-to-keyboard-device (in /dev/input/by-id/)>\n", argv[0]);
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

    // Open stopwatch device
    if (!stopwatch_open()) {
        fprintf(stderr, "ERROR: could not open stopwatch device\n");
    } else {
        printf("DEBUG: stopwatch_open() OK\n");
        stopwatch_set(0, 0, 0);
        stopwatch_nodisplay();
    }

    // Open LEDR device (wrapper)
    if (!LEDR_open()) {
        fprintf(stderr, "ERROR: LEDR_open() failed\n");
    } else {
        printf("DEBUG: LEDR_open() OK\n");
        led_state = 0;
        led_update();

        // Quick LED self-test: LEDR0+1 on for 1 sec, then off
        led_state = 0x3;
        led_update();
        sleep(1);
        led_state = 0x0;
        led_update();
        printf("DEBUG: LED self-test done (LEDR0+1 should have flashed).\n");
    }

    // Open KEY device (wrapper)
    if (!KEY_open()) {
        fprintf(stderr, "ERROR: KEY_open() failed\n");
    } else {
        printf("DEBUG: KEY_open() OK\n");
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

    // Open keyboard device (non-blocking) for piano keys
    int kbd_fd;
    if ((kbd_fd = open(kbd_path, O_RDONLY | O_NONBLOCK)) == -1) {
        perror("Could not open keyboard device");
        pthread_cancel(audio_tid);
        pthread_cancel(video_tid);
        pthread_join(audio_tid, NULL);
        pthread_join(video_tid, NULL);
        return 1;
    }

    struct input_event ev;
    int event_size = sizeof(struct input_event);

    printf("Part 6 running. Use piano keys (Q,2,W,3,E,R,5,T,6,Y,7,U,I).\n");
    printf("KEY0: start/stop recording (LEDR0, 59s countdown).\n");
    printf("KEY1: playback recording (LEDR1).\n");

    // Main loop to read keyboard + pushbuttons and handle record/playback
    while (!stop) {
        // 1) Handle keyboard piano events (PC keyboard)
        ssize_t bytes = read(kbd_fd, &ev, event_size);
        if (bytes == event_size && ev.type == EV_KEY) {
            int tone_idx = keycode_to_tone_index(ev.code);

            if (ev.value == KEY_PRESSED) {
                printf("PRESSED  0x%04x\n", (int)ev.code);
            } else if (ev.value == KEY_RELEASED) {
                printf("RELEASED 0x%04x\n", (int)ev.code);
            }

            if (tone_idx >= 0 && tone_idx < NUM_TONES) {
                // Live control of tone_volume/key_down
                if (ev.value == KEY_PRESSED) {
                    pthread_mutex_lock(&mutex_tone_volume);
                    key_down[tone_idx]    = 1;
                    tone_volume[tone_idx] = BASE_VOL;
                    pthread_mutex_unlock(&mutex_tone_volume);

                    record_event_if_needed(tone_idx, 1);
                }
                else if (ev.value == KEY_RELEASED) {
                    pthread_mutex_lock(&mutex_tone_volume);
                    key_down[tone_idx] = 0;
                    pthread_mutex_unlock(&mutex_tone_volume);

                    record_event_if_needed(tone_idx, 0);
                }

                video_request = 1;
            }
        }

        // 2) Handle pushbuttons KEY0 / KEY1 via KEY wrappers
        int key_val = 0;
        if (KEY_read(&key_val)) {
            // Edge detection: only react to 0 to 1 transitions
            int rising = key_val & ~prev_key_val;

            if (rising != 0) {
                printf("DEBUG: KEY_read() value = 0x%X (prev=0x%X, rising=0x%X)\n", key_val, prev_key_val, rising);

                // KEY0 (bit 0)
                if (rising & 0x1) {
                    printf(" -> KEY0 pressed\n");
                    if (!recording && !playing) {
                        start_recording();
                    } else if (recording) {
                        stop_recording();
                    } else if (playing) {
                        stop_playback();
                    }
                }
                // KEY1 (bit 1)
                if (rising & 0x2) {
                    printf(" -> KEY1 pressed\n");
                    if (!playing && !recording) {
                        start_playback();
                    } else if (playing) {
                        stop_playback();
                    }
                }
            }

            prev_key_val = key_val;
        }

        // 3) Handle playback timing
        if (playing) {
            handle_playback_step();
        }

        // 4) Update recording countdown on HEX (auto-stop at 0)
        if (recording) {
            update_recording_countdown();
        }

        // Small sleep
        usleep(1000);
    }

    // Clean shutdown
    pthread_cancel(audio_tid);
    pthread_cancel(video_tid);
    pthread_join(audio_tid, NULL);
    pthread_join(video_tid, NULL);

    close(kbd_fd);

    LEDR_close();
    KEY_close();

    stopwatch_nodisplay();
    stopwatch_close();

    printf("\nExiting Part 6 program\n");
    return 0;
}

/* Function to allow clean exit of the program */
void catchSIGINT(int signum) {
    (void)signum;
    stop = 1;
}