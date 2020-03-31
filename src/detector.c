/****************************************************************************
 * detector.c
 * openacousticdevices.info
 * March 2018
 *****************************************************************************/

#include "detector.h"
#include "audioMoth.h"
#include "hmm.h"

/* Samples are stored as two buffers, each containing 16,000 samples at 8kHz (2 seconds) */

#define BUFFER_SIZE             16000

#define SAMPLE_COUNT            32000

/* Each Goertzel filter turns 128 samples into an amplitude */

#define WINDOW_LENGTH  			128

/* The number of Goertzel amplitudes which can be produced from SAMPLE_COUNT samples */
/* 32000 / 128 = 250 windows */

#define WINDOW_COUNT (SAMPLE_COUNT/WINDOW_LENGTH)

/* 128 Hamming factors / 2^14 */

static float scaledHammingFactors[] = {1.7789363971e-05,1.6648398702e-05,1.55467270626e-05,1.44870030762e-05,1.34717797107e-05,1.25035027281e-05,1.15845047925e-05,1.07169998503e-05,9.90307779708e-06,9.14469944225e-06,8.4436917857e-06,7.80174361628e-06,7.22040144337e-06,6.70106577117e-06,6.24498772481e-06,5.85326603625e-06,5.52684439735e-06,5.26650918643e-06,5.07288757381e-06,4.94644601092e-06,4.88748910656e-06,4.89615889307e-06,4.97243448418e-06,5.11613212531e-06,5.32690563625e-06,5.60424724516e-06,5.94748881181e-06,6.3558034372e-06,6.82820745563e-06,7.36356280444e-06,7.96057976572e-06,8.61782007329e-06,9.33370037771e-06,1.01064960606e-05,1.09343453895e-05,1.18152540028e-05,1.27470997146e-05,1.37276376269e-05,1.47545055379e-05,1.5825229633e-05,1.6937230444e-05,1.80878290636e-05,1.92742535989e-05,2.04936458492e-05,2.17430681919e-05,2.30195106591e-05,2.4319898189e-05,2.56410980343e-05,2.69799273088e-05,2.83331606554e-05,2.96975380165e-05,3.10697724876e-05,3.24465582356e-05,3.3824578463e-05,3.52005133985e-05,3.65710482942e-05,3.79328814115e-05,3.92827319753e-05,4.06173480771e-05,4.19335145101e-05,4.32280605138e-05,4.44978674137e-05,4.57398761336e-05,4.69510945658e-05,4.8128604779e-05,4.9269570048e-05,5.03712416874e-05,5.14309656738e-05,5.24461890393e-05,5.34144660219e-05,5.43334639575e-05,5.52009688997e-05,5.60148909529e-05,5.67732693078e-05,5.74742769643e-05,5.81162251337e-05,5.86975673066e-05,5.92169029788e-05,5.96729810252e-05,6.00647027137e-05,6.03911243526e-05,6.06514595636e-05,6.08450811762e-05,6.09715227391e-05,6.10304796434e-05,6.10218098569e-05,6.09455342658e-05,6.08018366247e-05,6.05910631137e-05,6.03137215048e-05,5.99704799382e-05,5.95621653128e-05,5.90897612944e-05,5.85544059456e-05,5.79573889843e-05,5.73001486767e-05,5.65842683723e-05,5.58114726894e-05,5.49836233605e-05,5.41027147472e-05,5.31708690354e-05,5.21903311231e-05,5.11634632121e-05,5.0092739117e-05,4.8980738306e-05,4.78301396864e-05,4.66437151511e-05,4.54243229008e-05,4.41749005581e-05,4.28984580909e-05,4.1598070561e-05,4.02768707157e-05,3.89380414412e-05,3.75848080946e-05,3.62204307335e-05,3.48481962624e-05,3.34714105144e-05,3.2093390287e-05,3.07174553515e-05,2.93469204558e-05,2.79850873385e-05,2.66352367747e-05,2.53006206729e-05,2.39844542399e-05,2.26899082362e-05,2.14201013363e-05,2.01780926164e-05,1.89668741842e-05};

/* Goertzel filter feature constants */

#define w1 1.924911f    /* 2*cos(2*pi*350/8000) */
#define w2 1.044997f    /* 2*cos(2*pi*1300/8000) */
#define w3 -1.847759f   /* 2*cos(2*pi*3500/8000) */

/* Max HMM response to consider a gunshot, given upper limit of gunshot lengths in dataset is 1.5 seconds */
/* (1.5 SECONDS * SAMPLE_RATE) / WINDOW_COUNT = 93.75 */

#define DETECTION_MAX 93

/* Goertzel responses for each of the three features used by the model */

static float goertzelValues1[WINDOW_COUNT];
static float goertzelValues2[WINDOW_COUNT];
static float goertzelValues3[WINDOW_COUNT];

/* Main detection function, accepts two pointers to buffers containing two seconds of audio each */
/* Returns true if the HMM detects a gunshot */

bool detected(int16_t* buffer1, int16_t* buffer2){

    float y_1 = 0.0f;
    float y_2 = 0.0f;
    float y_3 = 0.0f;

    float d1_1 = 0.0f;
    float d1_2 = 0.0f;
    float d1_3 = 0.0f;

    float d2_1 = 0.0f;
    float d2_2 = 0.0f;
    float d2_3 = 0.0f;

    /* Index in a single window of samples */

    uint8_t j = 0 ;

    uint8_t window_index = 0;

    for (uint16_t i = 0; i < SAMPLE_COUNT; i++) {

        int16_t sample = 0;

        if (i < BUFFER_SIZE) {

            sample = buffer1[i];

        } else {

            sample = buffer2[i-BUFFER_SIZE];

        }

        /* Scale and apply Hamming window to sample */

        float scaledSample = (float) sample * scaledHammingFactors[j];

        y_1 = scaledSample + w1 * d1_1 - d2_1;

        d2_1 = d1_1;
        d1_1 = y_1;

        y_2 = scaledSample + w2 * d1_2 - d2_2;
        d2_2 = d1_2;
        d1_2 = y_2;

        y_3 = scaledSample + w3 * d1_3 - d2_3;
        d2_3 = d1_3;
        d1_3 = y_3;

        j++;

        /* Once Goertzel window has been filled */

        if (j == WINDOW_LENGTH) {

            arm_sqrt_f32(d1_1 * d1_1 + d2_1 * d2_1 - d1_1 * d2_1 * w1, &goertzelValues1[window_index]);
            arm_sqrt_f32(d1_2 * d1_2 + d2_2 * d2_2 - d1_2 * d2_2 * w2, &goertzelValues2[window_index]);
            arm_sqrt_f32(d1_3 * d1_3 + d2_3 * d2_3 - d1_3 * d2_3 * w3, &goertzelValues3[window_index]);

            y_1 = 0.0f;
            y_2 = 0.0f;
            y_3 = 0.0f;

            d1_1 = 0.0f;
            d1_2 = 0.0f;
            d1_3 = 0.0f;

            d2_1 = 0.0f;
            d2_2 = 0.0f;
            d2_3 = 0.0f;
            j = 0;

            window_index++;

        }

    }

    int16_t p_gunshot = calculate(goertzelValues1, goertzelValues2, goertzelValues3, WINDOW_COUNT);

    return (p_gunshot > 0 && p_gunshot <= DETECTION_MAX);

}
