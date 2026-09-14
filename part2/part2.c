#define _GNU_SOURCE
#include <sched.h>
#include <string.h>
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

/**  your part 2 user code here  **/ 
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

const int NUM_TONES = 13;

// Write ONE sample = sum of all enabled tones
static void write_chord_sample(volatile int *AUDIO_PTR, const int tone_volume[13], int nth_sample) {
    double sample_val = 0.0;
    int i;

    // Sum all active tones
    for (i = 0; i < NUM_TONES; i++) {
        if (tone_volume[i] != 0) {
            double theta = (double)nth_sample * tone_rps[i]; // radians
            sample_val += (double)tone_volume[i] * sin(theta);
        }
    }

    int sample = (int)sample_val;

    int fifospace;
    // Wait for space in both left and right FIFOs
    while (1) {
        fifospace = *(AUDIO_PTR + 1);
        int wslc = (fifospace & 0xFF000000) >> 24;
        int wsrc = (fifospace & 0x00FF0000) >> 16;

        if (wslc > 0 && wsrc > 0)
            break;
        // otherwise spin until hardware consumes some samples
    }
    *(AUDIO_PTR + 2) = sample;
    *(AUDIO_PTR + 3) = sample;
}

int main (int argc, char *argv[]) {
    int fd = -1;               // used to open /dev/mem for access to physical addresses
    void *LW_virtual;          // used to map physical addresses for the light-weight bridge
    volatile int * AUDIO_PTR;
    int tone_volume[13];
    int i;

    // Expect one 13-char argument: eg: "1000100100000"
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <13-bit chord string>\n", argv[0]);
        return 1;
    }

    char *chord = argv[1];
    if ((int)strlen(chord) != NUM_TONES) {
        fprintf(stderr, "Error: chord string must be exactly 13 characters of '0' or '1'.\n");
        return 1;
    }

    // Map each '1'/'0' to a tone volume to avoid overflow, each tone's max volume is 0x7FFFFFFF / 13
    const int BASE_VOL = 0x7FFFFFFF / NUM_TONES;

    for (i = 0; i < NUM_TONES; i++) {
        if (chord[i] == '1')
            tone_volume[i] = BASE_VOL;
        else
            tone_volume[i] = 0;
    }

    //catch SIGINT from ctrl+c, instead of having it abruptly close this program
    signal(SIGINT, catchSIGINT);

    // Create virtual memory access to the FPGA light-weight bridge
    if ((fd = open_physical (fd)) == -1)
        return (-1);
    if ((LW_virtual = map_physical (fd, LW_BRIDGE_BASE, LW_BRIDGE_SPAN)) == NULL) {
        close_physical(fd);
        return (-1);
    }
        
    // Setting pointers using virtual memory
    AUDIO_PTR = (int *) (LW_virtual + AUDIO_BASE);

    // Clear audio FIFOs (CW bit), then turn off clear
    *(AUDIO_PTR + 0) = 0b1000;  // set CW = 1
    *(AUDIO_PTR + 0) = 0x0;     // clear CW

    // Play for 1 second: SAMPLING_RATE samples
    int num_samples = SAMPLING_RATE; // 8000 samples for 1 second
    int n;
    for (n = 0; n < num_samples && !stop; n++) {
        write_chord_sample(AUDIO_PTR, tone_volume, n);
    }

    unmap_physical(LW_virtual, LW_BRIDGE_SPAN);
    close_physical(fd);
    printf ("\nExiting Part 2 program\n");
    return 0;
}


/* Function to allow clean exit of the program */
void catchSIGINT(int signum) {
	stop = 1;
}