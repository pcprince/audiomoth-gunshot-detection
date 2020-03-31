/****************************************************************************
 * main.c
 * openacousticdevices.info
 * March 2018
 *****************************************************************************/

#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "audioMoth.h"
#include "detector.h"

/* Sleep and LED constants */

#define DEFAULT_WAIT_INTERVAL               1

#define LOW_BATTERY_LED_FLASHES             10

#define SHORT_LED_FLASH_DURATION            100
#define LONG_LED_FLASH_DURATION             500

/* Useful time constants */

#define SECONDS_IN_MINUTE                   60
#define SECONDS_IN_HOUR                     (60 * SECONDS_IN_MINUTE)

/* 32 GB storage, 64 KB files, 333 day battery */
/* Maximum of 1400 recordings will be produced per day */
/* Maximum of 31.4 GB used before battery runs out */

#define MAX_RECORDINGS_PER_HOUR             100

/* SRAM buffer constants */

#define NUMBER_OF_BUFFERS                   8
#define EXTERNAL_SRAM_SIZE_IN_SAMPLES       (AM_EXTERNAL_SRAM_SIZE_IN_BYTES / 2)
#define NUMBER_OF_SAMPLES_IN_BUFFER         (EXTERNAL_SRAM_SIZE_IN_SAMPLES / NUMBER_OF_BUFFERS)
#define NUMBER_OF_SAMPLES_IN_DMA_TRANSFER   128
#define NUMBER_OF_BUFFERS_TO_SKIP           1

/* WAV header constant */

#define PCM_FORMAT                          1
#define RIFF_ID_LENGTH                      4
#define LENGTH_OF_COMMENT                   128

/* USB configuration constant */

#define MAX_START_STOP_PERIODS              5

/* Useful macros */

#define FLASH_LED(led, duration) { \
    AudioMoth_set ## led ## LED(true); \
    AudioMoth_delay(duration); \
    AudioMoth_set ## led ## LED(false); \
}

#define RETURN_ON_ERROR(fn) { \
    bool success = (fn); \
    if (success != true) { \
        recordingCancelled = true; \
        FLASH_LED(Both, LONG_LED_FLASH_DURATION) \
        return; \
    } \
}

#define SAVE_SWITCH_POSITION_AND_POWER_DOWN(duration) { \
    *previousSwitchPosition = switchPosition; \
    AudioMoth_powerDownAndWake(duration, true); \
}

#define MAX(a,b) (((a) > (b)) ? (a) : (b))

#define MIN(a,b) (((a) < (b)) ? (a) : (b))

/* WAV header */

#pragma pack(push, 1)

typedef struct {
    char id[RIFF_ID_LENGTH];
    uint32_t size;
} chunk_t;

typedef struct {
    chunk_t icmt;
    char comment[LENGTH_OF_COMMENT];
} icmt_t;

typedef struct {
    uint16_t format;
    uint16_t numberOfChannels;
    uint32_t samplesPerSecond;
    uint32_t bytesPerSecond;
    uint16_t bytesPerCapture;
    uint16_t bitsPerSample;
} wavFormat_t;

typedef struct {
    chunk_t riff;
    char format[RIFF_ID_LENGTH];
    chunk_t fmt;
    wavFormat_t wavFormat;
    chunk_t list;
    char info[RIFF_ID_LENGTH];
    icmt_t icmt;
    chunk_t data;
} wavHeader_t;

#pragma pack(pop)

static wavHeader_t wavHeader = {
    .riff = {.id = "RIFF", .size = 0},
    .format = "WAVE",
    .fmt = {.id = "fmt ", .size = sizeof(wavFormat_t)},
    .wavFormat = {.format = PCM_FORMAT, .numberOfChannels = 1, .samplesPerSecond = 0, .bytesPerSecond = 0, .bytesPerCapture = 2, .bitsPerSample = 16},
    .list = {.id = "LIST", .size = RIFF_ID_LENGTH + sizeof(icmt_t)},
    .info = "INFO",
    .icmt = {.icmt.id = "ICMT", .icmt.size = LENGTH_OF_COMMENT, .comment = ""},
    .data = {.id = "data", .size = 0}
};

void setHeaderDetails(uint32_t sampleRate, uint32_t numberOfSamples) {

    wavHeader.wavFormat.samplesPerSecond = sampleRate;
    wavHeader.wavFormat.bytesPerSecond = 2 * sampleRate;
    wavHeader.data.size = 2 * numberOfSamples;
    wavHeader.riff.size = 2 * numberOfSamples + sizeof(wavHeader_t) - sizeof(chunk_t);

}

void setHeaderComment(uint32_t currentTime, uint8_t *serialNumber, uint32_t gain) {

    time_t rawtime = currentTime;

    struct tm *time = gmtime(&rawtime);

    char *comment = wavHeader.icmt.comment;

    AM_batteryState_t batteryState = AudioMoth_getBatteryState();

    sprintf(comment, "Recorded at %02d:%02d:%02d %02d/%02d/%04d (UTC) by AudioMoth %08X%08X at gain setting %d while battery state was ",
            time->tm_hour, time->tm_min, time->tm_sec, time->tm_mday, 1 + time->tm_mon, 1900 + time->tm_year,
            (unsigned int)(serialNumber + 8), (unsigned int)serialNumber, (unsigned int)gain);

    comment += 110;

    if (batteryState == AM_BATTERY_LOW) {

        sprintf(comment, "< 3.6V");

    } else if (batteryState >= AM_BATTERY_FULL) {

        sprintf(comment, "> 5.0V");

    } else {

        batteryState += 35;

        sprintf(comment, "%01d.%01dV", batteryState / 10, batteryState % 10);

    }

}


/* USB configuration data structure */

#pragma pack(push, 1)

typedef struct {
    uint16_t startMinutes;
    uint16_t stopMinutes;
} startStopPeriod_t;

typedef struct {
    uint32_t time;
    uint8_t gain;
    uint8_t clockBand;
    uint8_t clockDivider;
    uint8_t acquisitionCycles;
    uint8_t oversampleRate;
    uint32_t sampleRate;
    uint16_t sleepDuration;
    uint16_t recordDuration;
    uint8_t enableLED;
    uint8_t activeStartStopPeriods;
    startStopPeriod_t startStopPeriods[MAX_START_STOP_PERIODS];
    uint8_t  hourWhenMaxWritesReached;
} configSettings_t;

#pragma pack(pop)

configSettings_t defaultConfigSettings = {
    .time = 0,
    .gain = 2,
    .clockBand = AM_HFRCO_11MHZ,
    .clockDivider = 1,
    .acquisitionCycles = 8,
    .oversampleRate = 64,
    .sampleRate = 8000,
    .sleepDuration = 5,
    .recordDuration = 3600,
    .enableLED = 0,
    .activeStartStopPeriods = 2,
    .startStopPeriods = {
        /* {.startMinutes = 780, .stopMinutes = 1380}, */ /* Start: 7:00 CST (13:00 UTC), Stop: 17:00 CST (23:00 UTC) */
        /* Night time listening schedule */
        {.startMinutes = 1380, .stopMinutes = 1439},   /* Start: 17:00 CST (23:00 UTC), Stop: 18:00 CST (23:59 UTC) */
        {.startMinutes = 0, .stopMinutes = 780},  /* Start: 18:00 CST (0:00 UTC), Stop: 7:00 CST (13:00 UTC) */
        {.startMinutes = 0, .stopMinutes = 0},
        {.startMinutes = 0, .stopMinutes = 0},
        {.startMinutes = 0, .stopMinutes = 0}
    },
    .hourWhenMaxWritesReached = 0XFF,
};

uint32_t *previousSwitchPosition = (uint32_t*)AM_BACKUP_DOMAIN_START_ADDRESS;

uint32_t *timeOfNextRecording = (uint32_t*)(AM_BACKUP_DOMAIN_START_ADDRESS + 4);

uint32_t *durationOfNextRecording = (uint32_t*)(AM_BACKUP_DOMAIN_START_ADDRESS + 8);

configSettings_t *configSettings = (configSettings_t*)(AM_BACKUP_DOMAIN_START_ADDRESS + 12);

/* SRAM buffer variables */

static volatile uint8_t writeBuffer;
static volatile uint32_t writeBufferIndex;

static volatile bool recordingCancelled;

static int16_t* buffers[NUMBER_OF_BUFFERS];

/* Current recording file name and folder name */

static char fileName[21];
static char folderName[8];

/* Function prototypes */

static void flashLedToIndicateBatteryLife(void);
static void makeRecording(uint32_t currentTime, uint32_t recordDuration, bool enableLED);
static void makeRecordingIfDetected(uint32_t currentTime, int16_t* buffer1, int16_t* buffer2, bool enableLED);
static bool inListeningPeriod(uint32_t currentTime);
static void initMicrophone(void);

/* Main function */

 int main(void) {

    /* Initialise device */

    AudioMoth_initialise();

    AM_switchPosition_t switchPosition = AudioMoth_getSwitchPosition();

    if (AudioMoth_isInitialPowerUp()) {

        *timeOfNextRecording = 0;

        *durationOfNextRecording = 0;

        *previousSwitchPosition = AM_SWITCH_NONE;

        memcpy(configSettings, &defaultConfigSettings, sizeof(configSettings_t));

    } else {

        /* Indicate battery state is not initial power up and switch has been moved into USB */

        if (switchPosition != *previousSwitchPosition && switchPosition == AM_SWITCH_USB) {

            flashLedToIndicateBatteryLife();

        }

    }

    /* Handle the case that the switch is in USB position  */

    if (switchPosition == AM_SWITCH_USB) {

        AudioMoth_handleUSB();

        SAVE_SWITCH_POSITION_AND_POWER_DOWN(DEFAULT_WAIT_INTERVAL);

    }

    /* Handle the case that the switch is in CUSTOM position but the time has not been set */

    if (switchPosition == AM_SWITCH_CUSTOM && (AudioMoth_hasTimeBeenSet() == false || configSettings->activeStartStopPeriods == 0)) {

        FLASH_LED(Both, SHORT_LED_FLASH_DURATION)

        SAVE_SWITCH_POSITION_AND_POWER_DOWN(DEFAULT_WAIT_INTERVAL);

    }

    /* Calculate time */

    uint32_t currentTime = AudioMoth_getTime();

    if (switchPosition != *previousSwitchPosition) {

        if (switchPosition == AM_SWITCH_DEFAULT) {

            /* Set parameters to start recording now */

            *timeOfNextRecording = currentTime;

            *durationOfNextRecording = configSettings->recordDuration;

        }

    }

    if (switchPosition == AM_SWITCH_DEFAULT) {

        /* Make recording if appropriate */

        bool enableLED = (switchPosition == AM_SWITCH_DEFAULT) || configSettings->enableLED;

        /* Setup Microphone */

        initMicrophone();

        makeRecording(currentTime, *durationOfNextRecording, enableLED);

        /* Set parameters to start recording after sleep period */

        if (!recordingCancelled) {

            *timeOfNextRecording = currentTime + configSettings->recordDuration + configSettings->sleepDuration;

        }

    }

    /* Handle the case that the switch is in CUSTOM position */

    if (switchPosition == AM_SWITCH_CUSTOM && inListeningPeriod(currentTime)) {

        time_t rawtime = currentTime;

        struct tm *time = gmtime(&rawtime);

        /* Check that the max number of writes has happened in the current hour, sleep if max has been reached */

        if (configSettings->hourWhenMaxWritesReached == time->tm_hour) {

            /* Flash LED to indicate waiting */

            if(configSettings->enableLED){

                FLASH_LED(Green, SHORT_LED_FLASH_DURATION)

            }

            SAVE_SWITCH_POSITION_AND_POWER_DOWN(configSettings->sleepDuration);

        }

        /* Setup Microphone */

        initMicrophone();

        /* Number of files written in the current hour */

        uint16_t filesWritten = 0;

        /* Hour when loop previously ran. If different to current hour then the recording count is reset */

        uint8_t triggerHour = 0;
        uint8_t prevHour = 0;

        /* Set up buffers for detection */

        uint32_t readBuffer = 0;
        uint32_t prevreadBuffer = 0;

        /* Skip first buffer to remove microphone ramp up, only happens at the start of each day or max out hour */

        while (writeBuffer != 3);

        /* Read buffers are the two buffers preceding the current write buffer */

        readBuffer = writeBuffer - 1;
        prevreadBuffer = readBuffer - 1;

        while (!recordingCancelled && inListeningPeriod(currentTime)) {

            /* If the hour has changed since last iteration of the loop, reset the recording counter */

            if (prevHour != time->tm_hour) {

                filesWritten = 0;

            }

            prevHour = time->tm_hour;

            while (readBuffer != writeBuffer && !recordingCancelled) {

                /* Run gunshot detection, making a recording if response is positive */

                bool containsGunshot = detected(buffers[prevreadBuffer], buffers[readBuffer]);

                if (containsGunshot) {

                    triggerHour = (uint8_t) time->tm_hour;

                    makeRecordingIfDetected(currentTime, buffers[prevreadBuffer], buffers[readBuffer], configSettings->enableLED);

                    filesWritten++;

                }

                /* Check current hourly recording count */

                if (filesWritten >= MAX_RECORDINGS_PER_HOUR) {

                    configSettings->hourWhenMaxWritesReached = triggerHour;

                    /* Flash LED to indicate waiting */

                    if (configSettings->enableLED) {

                        FLASH_LED(Green, SHORT_LED_FLASH_DURATION)

                    }

                    /* Power down */

                    SAVE_SWITCH_POSITION_AND_POWER_DOWN(configSettings->sleepDuration);

                } else {

                    /* If not at max recordings, reset value which is checked upon wakeup */

                    configSettings->hourWhenMaxWritesReached = 0XFF;

                }

                /* Increment buffer counters */

                prevreadBuffer = readBuffer;

                readBuffer = (readBuffer + 1) & (NUMBER_OF_BUFFERS - 1);

                /* Update Time*/

                currentTime = AudioMoth_getTime();

                /* Check that the time is still within the listening schedule */

                if (!inListeningPeriod(currentTime)) {

                    break;

                }

                /* Feed the watch dog Timer */

                AudioMoth_feedWatchdog();

            }

            /* Sleep until next DMA transfer is complete */

            AudioMoth_sleep();

        }

    }

    /* Flash LED to indicate waiting */

    if (configSettings->enableLED) {

        FLASH_LED(Green, SHORT_LED_FLASH_DURATION)

    }

    /* Power down */

    SAVE_SWITCH_POSITION_AND_POWER_DOWN(configSettings->sleepDuration);

}

/* AudioMoth handlers */

inline void AudioMoth_handleSwitchInterrupt() {

    recordingCancelled = true;

}

inline void AudioMoth_handleMicrophoneInterrupt(int16_t sample) { }

inline void AudioMoth_handleDirectMemoryAccessInterrupt(bool isPrimaryBuffer, int16_t **nextBuffer) {

    /* Update the current buffer index and write buffer */

    writeBufferIndex += NUMBER_OF_SAMPLES_IN_DMA_TRANSFER;

    if (writeBufferIndex == NUMBER_OF_SAMPLES_IN_BUFFER) {

        writeBufferIndex = 0;

        writeBuffer = (writeBuffer + 1) & (NUMBER_OF_BUFFERS - 1);

    }

    /* Update the next buffer index and write buffer */

    int nextWriteBuffer = writeBuffer;

    int nextWriteBufferIndex = writeBufferIndex + NUMBER_OF_SAMPLES_IN_DMA_TRANSFER;

    if (nextWriteBufferIndex == NUMBER_OF_SAMPLES_IN_BUFFER) {

        nextWriteBufferIndex = 0;

        nextWriteBuffer = (nextWriteBuffer + 1) & (NUMBER_OF_BUFFERS - 1);

    }

    /* Re-activate the DMA */

    *nextBuffer = buffers[nextWriteBuffer] + nextWriteBufferIndex;

}

/* Initialise audio circuitry */

static void initMicrophone(void) {

    /* Initialise buffers */

    writeBuffer = 0;

    writeBufferIndex = 0;

    recordingCancelled = false;

    /* Initialise microphone for recording or listening */

    buffers[0] = (int16_t*)AM_EXTERNAL_SRAM_START_ADDRESS;

    for (int i = 1; i < NUMBER_OF_BUFFERS; i += 1) {

        buffers[i] = buffers[i - 1] + NUMBER_OF_SAMPLES_IN_BUFFER;

    }

    /* Switch to HFRCO */

    if (configSettings->clockBand < AM_HFXO) {

        AudioMoth_enableHFRCO(configSettings->clockBand);

        uint32_t clockFrequency = AudioMoth_getClockFrequency(configSettings->clockBand);

        uint32_t actualSampleRate = AudioMoth_calculateSampleRate(clockFrequency, configSettings->clockDivider, configSettings->acquisitionCycles, configSettings->oversampleRate);

        uint32_t targetFrequency = (float)clockFrequency * (float)configSettings->sampleRate / (float)actualSampleRate;

        AudioMoth_calibrateHFRCO(targetFrequency);

        AudioMoth_selectHFRCO();

    }

    /* Initialise microphone for recording */

    AudioMoth_enableExternalSRAM();

    AudioMoth_enableMicrophone(configSettings->gain, configSettings->clockDivider, configSettings->acquisitionCycles, configSettings->oversampleRate);

    AudioMoth_initialiseDirectMemoryAccess(buffers[0], buffers[0] + NUMBER_OF_SAMPLES_IN_DMA_TRANSFER, NUMBER_OF_SAMPLES_IN_DMA_TRANSFER);

    AudioMoth_startMicrophoneSamples();

}

/* Save recording to SD card after listen*/

static void makeRecordingIfDetected(uint32_t currentTime, int16_t* buffer1, int16_t* buffer2, bool enableLED) {

    /* Initialise file system and open a new file */

    RETURN_ON_ERROR(AudioMoth_enableFileSystem());

    /* Open a file with the name as a UNIX time stamp in HEX */

    time_t rawtime = currentTime;

    struct tm *time = gmtime(&rawtime);

    /* Create a folder for current month */

    sprintf(folderName, "%02d_%04d", 1 + time->tm_mon, 1900 + time->tm_year);

    sprintf(fileName, "%s\\%08X.WAV", folderName, (unsigned int)currentTime);

    if (!AudioMoth_folderExists(folderName)) {

        RETURN_ON_ERROR(AudioMoth_makeSDfolder(folderName));

        RETURN_ON_ERROR(AudioMoth_openFile(fileName));

    }

    RETURN_ON_ERROR(AudioMoth_openFile(fileName));

    /* Initialise the WAV header */

    setHeaderDetails(configSettings->sampleRate, (NUMBER_OF_SAMPLES_IN_BUFFER*2));

    setHeaderComment(currentTime, (uint8_t*)AM_UNIQUE_ID_START_ADDRESS, configSettings->gain);

    /* Write the file to SD card */

    if (enableLED) {

        AudioMoth_setRedLED(true);

    }

    AudioMoth_seekInFile(0);

    AudioMoth_writeToFile(&wavHeader, sizeof(wavHeader));

    AudioMoth_writeToFile(buffer1, 2 * NUMBER_OF_SAMPLES_IN_BUFFER);

    AudioMoth_writeToFile(buffer2, 2 * NUMBER_OF_SAMPLES_IN_BUFFER);

    AudioMoth_setRedLED(false);

    /* Close the file */

    AudioMoth_closeFile();

    AudioMoth_disableFileSystem();

}

static bool inListeningPeriod(uint32_t currentTime) {

    /* Check number of active state stop periods */

    if (configSettings->activeStartStopPeriods > MAX_START_STOP_PERIODS) {

        configSettings->activeStartStopPeriods = MAX_START_STOP_PERIODS;

    }

    /* No active periods */

    if (configSettings->activeStartStopPeriods == 0) {

        return false;

    }

    /* Calculate the number of seconds of this day */

    time_t rawtime = currentTime;

    struct tm *time = gmtime(&rawtime);

    uint32_t currentSeconds = SECONDS_IN_HOUR * time->tm_hour + SECONDS_IN_MINUTE * time->tm_min + time->tm_sec;

    /* Check each active start stop period */

    for (uint32_t i = 0; i < configSettings->activeStartStopPeriods; i += 1) {

        startStopPeriod_t *period = configSettings->startStopPeriods + i;

        /* Calculate the start and stop time of the current period */

        uint32_t startSeconds = SECONDS_IN_MINUTE * period->startMinutes;

        uint32_t stopSeconds = SECONDS_IN_MINUTE * period->stopMinutes;

        /* If current time inside selected period */

        if (currentSeconds > startSeconds && currentSeconds < stopSeconds) {

            return true;

        }

    }

    return false;

}

/* Save recording to SD card */

static void makeRecording(uint32_t currentTime, uint32_t recordDuration, bool enableLED) {

    /* Calculate recording parameters */

    uint32_t numberOfSamplesInHeader = sizeof(wavHeader) >> 1;

    uint32_t numberOfSamples = configSettings->sampleRate * recordDuration;

    /* Initialise file system and open a new file */

    RETURN_ON_ERROR(AudioMoth_enableFileSystem());

    /* Open a file with the name as a UNIX time stamp in HEX */

    time_t rawtime = currentTime;

    struct tm *time = gmtime(&rawtime);

    /* Create a folder for current month */

    sprintf(folderName, "%02d_%04d", 1 + time->tm_mon, 1900 + time->tm_year);

    sprintf(fileName, "%s\\%08X.WAV", folderName, (unsigned int)currentTime);

    if (AudioMoth_folderExists(folderName)) {

        RETURN_ON_ERROR(AudioMoth_openFile(fileName));

    } else {

        RETURN_ON_ERROR(AudioMoth_makeSDfolder(folderName));

        RETURN_ON_ERROR(AudioMoth_openFile(fileName));

    }

    /* Main record loop */

    uint32_t samplesWritten = 0;

    uint32_t buffersProcessed = 0;

    uint32_t readBuffer = writeBuffer;

    while (samplesWritten < numberOfSamples + numberOfSamplesInHeader && !recordingCancelled) {

        while (readBuffer != writeBuffer && samplesWritten < numberOfSamples + numberOfSamplesInHeader && !recordingCancelled) {

            /* Light LED during SD card write if appropriate */

            if (enableLED) {

                AudioMoth_setRedLED(true);

            }

            /* Write the appropriate number of bytes to the SD card */

            uint32_t numberOfSamplesToWrite = 0;

            if (buffersProcessed >= NUMBER_OF_BUFFERS_TO_SKIP) {

                numberOfSamplesToWrite = MIN(numberOfSamples + numberOfSamplesInHeader - samplesWritten, NUMBER_OF_SAMPLES_IN_BUFFER);

            }

            AudioMoth_writeToFile(buffers[readBuffer], 2 * numberOfSamplesToWrite);

            /* Increment buffer counters */

            readBuffer = (readBuffer + 1) & (NUMBER_OF_BUFFERS - 1);

            samplesWritten += numberOfSamplesToWrite;

            buffersProcessed += 1;

            /* Clear LED */

            AudioMoth_setRedLED(false);

        }

        /* Sleep until next DMA transfer is complete */

        AudioMoth_sleep();

    }

    /* Initialise the WAV header */

    samplesWritten = MAX(numberOfSamplesInHeader, samplesWritten);

    setHeaderDetails(configSettings->sampleRate, samplesWritten - numberOfSamplesInHeader);

    setHeaderComment(currentTime, (uint8_t*)AM_UNIQUE_ID_START_ADDRESS, configSettings->gain);


    /* Write the header */

    if (enableLED) {

        AudioMoth_setRedLED(true);

    }

    AudioMoth_seekInFile(0);

    AudioMoth_writeToFile(&wavHeader, sizeof(wavHeader));

    AudioMoth_setRedLED(false);

    /* Close the file */

    AudioMoth_closeFile();

}

static void flashLedToIndicateBatteryLife(void){

    uint32_t numberOfFlashes = LOW_BATTERY_LED_FLASHES;

    AM_batteryState_t batteryState = AudioMoth_getBatteryState();

    /* Set number of flashes according to battery state */

    if (batteryState > AM_BATTERY_LOW) {

        numberOfFlashes = (batteryState >= AM_BATTERY_4V6) ? 4 : (batteryState >= AM_BATTERY_4V4) ? 3 : (batteryState >= AM_BATTERY_4V0) ? 2 : 1;

    }

    /* Flash LED */

    for (uint32_t i = 0; i < numberOfFlashes; i += 1) {

        FLASH_LED(Red, SHORT_LED_FLASH_DURATION)

        if (numberOfFlashes == LOW_BATTERY_LED_FLASHES) {

            AudioMoth_delay(SHORT_LED_FLASH_DURATION);

        } else {

            AudioMoth_delay(LONG_LED_FLASH_DURATION);

        }

    }

}
