/****************************************************************************
 * hmm.c
 * openacousticdevices.info
 * March 2018
 *****************************************************************************/

#include "hmm.h"
#include "arm_math.h"
#include <stdint.h>

#define NUM_FEATURES    3

#define NUM_STATES      4

#define MAX_T           250

/* Features: 350 Hz, 1300 Hz, 3500 Hz */
/* States: Silence, Impulse, Tail, Noise */

static const float EMISSION_MEAN[NUM_STATES][NUM_FEATURES] = {
    {-3.254631f, -4.244978f, -4.455339f},
    {-0.314364f, -0.511267f, -1.409444f},
    {-2.002476f, -2.556155f, -3.690385f},
    {-3.109867f, -3.689082f, -3.476363f},
};

static const float ONE_OVER_EMISSION_VARIANCE[NUM_STATES][NUM_FEATURES] = {
    {2.607228f, 1.108950f, 1.083559f},
    {0.227855f, 0.218091f, 0.140690f},
    {0.534408f, 0.632945f, 0.722583f},
    {1.886675f, 1.096767f, 0.771746f},
};

/* ONE_OVER_SQRT_2PI / SQRT_EMISSION_VARIANCE */

static const float NORMALISATION_FACTORS[NUM_STATES][NUM_FEATURES] = {
    {0.644169f, 0.420113f, 0.415276f},
    {0.190432f, 0.186307f, 0.149638f},
    {0.291640f, 0.317390f, 0.339120f},
    {0.547972f, 0.417799f, 0.350467f},
};

static const float TRANSITION_MATRIX[NUM_STATES][NUM_STATES] = {
    {0.98f, 0.01f, 0.00f, 0.01f},
    {0.00f, 0.69f, 0.31f, 0.00f},
    {0.07f, 0.00f, 0.92f, 0.01f},
    {0.01f, 0.01f, 0.00f, 0.98f}
};

static const float INITIAL[NUM_STATES] = {0.86f, 0.07f, 0.00f, 0.07f};

static float* data[3];

static float max_prob[NUM_STATES][MAX_T];
static uint8_t edges[NUM_STATES][MAX_T];

static float col_max[NUM_STATES] = {0.0f,0.0f,0.0f,0.0f};
static uint8_t col_argmax[NUM_STATES] = {0,0,0,0};

static uint8_t mpe[MAX_T] = {0}; /* Most likely states */

static float emit[NUM_STATES];

float lognormalpdf(float X, float mu, float p1, float one_over_variance) {

    float mean_diff = log(X) - mu;

    float e = (-1.0f * mean_diff * mean_diff) * 0.5f * one_over_variance;

    float fe = expf(e);

    float pdf = p1 * fe;

    return pdf;

}

int16_t calculate(float freq1[], float freq2[], float freq3[], int16_t T) {

    data[0] = freq1;
    data[1] = freq2;
    data[2] = freq3;

    if (T > MAX_T) {

        T = MAX_T;

    }

    for (uint16_t t = 0; t < T; t++) {

        float max_emit = -1.0f;

        for (uint8_t i = 0; i < NUM_STATES; i++) {

            float value = 1.0f;

            for (uint8_t j = 0; j < NUM_FEATURES; j++) {

                value *= lognormalpdf(data[j][t], EMISSION_MEAN[i][j], NORMALISATION_FACTORS[i][j], ONE_OVER_EMISSION_VARIANCE[i][j]);

            }

            emit[i] = value;

            if (max_emit < value) {

                max_emit = value;

            }

        }

        max_emit *= 0.05f;

        for (uint8_t i = 0; i < NUM_STATES; i++) {

            if (emit[i] < max_emit) {

                emit[i] = max_emit;

            }

        }

        if (t == 0) {

            for(uint8_t k = 0; k < NUM_STATES; k++) {

                max_prob[k][t] = INITIAL[k] * emit[k];

            }

        } else {

            for (uint8_t i = 0; i < NUM_STATES; i++) {

                col_max[i] = 0.0f;

                col_argmax[i] = 0;

            }

            for (uint8_t i = 0; i < NUM_STATES; i++) {

                for (uint8_t j = 0; j < NUM_STATES; j++) {

                    float product = max_prob[j][t-1] * TRANSITION_MATRIX[j][i] * emit[i];

                    if (product > col_max[i]) {

                        col_max[i] = product;

                        col_argmax[i] = j;

                    }

                }

            }

            for (uint8_t i = 0; i < NUM_STATES; i++) {

                max_prob[i][t] = col_max[i];

                edges[i][t] = col_argmax[i];

            }

        }

        float max_prob_sum = 0.0f;

        for (uint8_t i = 0; i < NUM_STATES; i++) {

            max_prob_sum += max_prob[i][t];

        }

        for (uint8_t i = 0; i < NUM_STATES; i++) {

            max_prob[i][t] = max_prob[i][t] / max_prob_sum;

            if(isnan(max_prob[i][t])) {

                max_prob[i][t] = max_prob[i][t-1];

            }

        }

    }

    float current_max_prob = 0.0f;

    uint8_t current_max_prob_arg = 0;

    for (uint8_t i = 0; i < NUM_STATES; i++) {

        if(max_prob[i][T-1] > current_max_prob) {

            current_max_prob = max_prob[i][T-1];

            current_max_prob_arg = i;

        }

    }

    mpe[T-1] = current_max_prob_arg;

    for (uint8_t t = T-1; t > 0; t--) {

        mpe[t-1] = edges[mpe[t]][t];

    }

    int16_t p_gunshot = 0;

    for (uint8_t t = 0; t < T; t++) {

        if (mpe[t] == 1 || mpe[t] == 2) {

            p_gunshot++;

        }

    }

    return p_gunshot;

}
