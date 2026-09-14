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
#include "address_map_arm.h"
#include "defines.h"

/**  your part 1 user code here  **/

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

const int num_tones = 13;
double Pi = 3.141592653589793;

// Convert frequency to radians
double freq_to_radians(int nth_sample, double rad_per_sample) {
    double theta = (double)nth_sample * rad_per_sample;
    return theta;
}

void write_to_audio_port(volatile int * AUDIO_PTR, int vol, int nth_sample, double freq) {
    int fifospace;
    double theta = freq_to_radians(nth_sample, freq);
    int sample   = (int)((double)vol * sin(theta));

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

int main (void) {
    int fd = -1;               // used to open /dev/mem for access to physical addresses
    void *LW_virtual;          // used to map physical addresses for the light-weight bridge
    volatile int * AUDIO_PTR;

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

    // Clear audio FIFOs
    *(AUDIO_PTR + 0) = 0b1000;
    *(AUDIO_PTR + 0) = 0x0;

    int samples_per_tone = (int)(0.3 * SAMPLING_RATE);   // 300 ms per tone
    int nth_sample;
    double freq;
    int vol = 0x10000000; //max volume

    //Write 8000 samples (one second’s worth) of middle C (261.63Hz)
    for (nth_sample = 0; nth_sample < num_tones; nth_sample++) {
        freq = tone_rps[nth_sample];
        int n;
        for (n = 0; n < samples_per_tone && !stop; n++) {
            write_to_audio_port(AUDIO_PTR, vol, n, freq);
        }
    }

    unmap_physical(LW_virtual, LW_BRIDGE_SPAN);
    close_physical(fd);
    printf ("\nExiting Part 1 program\n");
    return 0;
}


/* Function to allow clean exit of the program */
void catchSIGINT(int signum) {
	stop = 1;
}
