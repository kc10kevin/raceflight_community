/*
 * This file is part of RaceFlight.
 *
 * RaceFlight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RaceFlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with RaceFlight.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>


#include "platform.h"
#include "debug.h"

#include "common/maths.h"
#include "common/axis.h"

#include "drivers/sensor.h"
#include "drivers/accgyro.h"

#include "sensors/sensors.h"
#include "sensors/acceleration.h"
#include "sensors/barometer.h"
#include "sensors/sonar.h"

#include "rx/rx.h"

#include "io/rc_controls.h"
#include "io/escservo.h"

#include "flight/mixer.h"
#include "flight/pid.h"
#include "flight/imu.h"

#include "config/runtime_config.h"

int32_t setVelocity = 0;
uint8_t velocityControl = 0;
int32_t errorVelocityI = 0;
int32_t altHoldThrottleAdjustment = 0;
int32_t AltHold;
int32_t vario = 0;                      


static barometerConfig_t *barometerConfig;
static pidProfile_t *pidProfile;
static rcControlsConfig_t *rcControlsConfig;
static escAndServoConfig_t *escAndServoConfig;

void configureAltitudeHold(
        pidProfile_t *initialPidProfile,
        barometerConfig_t *intialBarometerConfig,
        rcControlsConfig_t *initialRcControlsConfig,
        escAndServoConfig_t *initialEscAndServoConfig
)
{
    pidProfile = initialPidProfile;
    barometerConfig = intialBarometerConfig;
    rcControlsConfig = initialRcControlsConfig;
    escAndServoConfig = initialEscAndServoConfig;
}

#if defined(BARO) || defined(SONAR)

static int16_t initialThrottleHold;
static int32_t EstAlt;                


#define BARO_UPDATE_FREQUENCY_40HZ (1000 * 25)

#define DEGREES_80_IN_DECIDEGREES 800

static void applyMultirotorAltHold(void)
{
    static uint8_t isAltHoldChanged = 0;
    
    if (rcControlsConfig->alt_hold_fast_change) {
        
        if (ABS(rcData[THROTTLE] - initialThrottleHold) > rcControlsConfig->alt_hold_deadband) {
            errorVelocityI = 0;
            isAltHoldChanged = 1;
            rcCommandUsed[THROTTLE] += (rcData[THROTTLE] > initialThrottleHold) ? -rcControlsConfig->alt_hold_deadband : rcControlsConfig->alt_hold_deadband;
        } else {
            if (isAltHoldChanged) {
                AltHold = EstAlt;
                isAltHoldChanged = 0;
            }
            escAndServoConfig->maxthrottle = 2000;
            rcCommandUsed[THROTTLE] = constrain(initialThrottleHold + altHoldThrottleAdjustment, escAndServoConfig->minthrottle, escAndServoConfig->maxthrottle);
        }
    } else {
        
        if (ABS(rcData[THROTTLE] - initialThrottleHold) > rcControlsConfig->alt_hold_deadband) {
            
            setVelocity = (rcData[THROTTLE] - initialThrottleHold) / 2;
            velocityControl = 1;
            isAltHoldChanged = 1;
        } else if (isAltHoldChanged) {
            AltHold = EstAlt;
            velocityControl = 0;
            isAltHoldChanged = 0;
        }
        escAndServoConfig->maxthrottle = 2000;
        rcCommandUsed[THROTTLE] = constrain(initialThrottleHold + altHoldThrottleAdjustment, escAndServoConfig->minthrottle, escAndServoConfig->maxthrottle);
    }
}

static void applyFixedWingAltHold(airplaneConfig_t *airplaneConfig)
{
    
    
    

	rcCommandUsed[PITCH] += altHoldThrottleAdjustment * airplaneConfig->fixedwing_althold_dir;
}

void applyAltHold(airplaneConfig_t *airplaneConfig)
{
    if (STATE(FIXED_WING)) {
        applyFixedWingAltHold(airplaneConfig);
    } else {
        applyMultirotorAltHold();
    }
}

void updateAltHoldState(void)
{
    
    if (!IS_RC_MODE_ACTIVE(BOXBARO)) {
        DISABLE_FLIGHT_MODE(BARO_MODE);
        return;
    }

    if (!FLIGHT_MODE(BARO_MODE)) {
        ENABLE_FLIGHT_MODE(BARO_MODE);
        AltHold = EstAlt;
        initialThrottleHold = rcData[THROTTLE];
        errorVelocityI = 0;
        altHoldThrottleAdjustment = 0;
    }
}

void updateSonarAltHoldState(void)
{
    
    if (!IS_RC_MODE_ACTIVE(BOXSONAR)) {
        DISABLE_FLIGHT_MODE(SONAR_MODE);
        return;
    }

    if (!FLIGHT_MODE(SONAR_MODE)) {
        ENABLE_FLIGHT_MODE(SONAR_MODE);
        AltHold = EstAlt;
        initialThrottleHold = rcData[THROTTLE];
        errorVelocityI = 0;
        altHoldThrottleAdjustment = 0;
    }
}

bool isThrustFacingDownwards(attitudeEulerAngles_t *attitude)
{
    return ABS(attitude->values.roll) < DEGREES_80_IN_DECIDEGREES && ABS(attitude->values.pitch) < DEGREES_80_IN_DECIDEGREES;
}

/*
* This (poorly named) function merely returns whichever is higher, roll inclination or pitch inclination.
* 
* (my best interpretation of scalar 'tiltAngle') or rename the function.
*/
int16_t calculateTiltAngle(attitudeEulerAngles_t *attitude)
{
    return MAX(ABS(attitude->values.roll), ABS(attitude->values.pitch));
}

int32_t calculateAltHoldThrottleAdjustment(int32_t vel_tmp, float accZ_tmp, float accZ_old)
{
    int32_t result = 0;
    int32_t error;
    int32_t setVel;

    if (!isThrustFacingDownwards(&attitude)) {
        return result;
    }

    

    if (!velocityControl) {
        error = constrain(AltHold - EstAlt, -500, 500);
        error = applyDeadband(error, 10); 
        setVel = constrain((pidProfile->P8[PIDALT] * error / 128), -300, +300); 
    } else {
        setVel = setVelocity;
    }
    

    
    error = setVel - vel_tmp;
    result = constrain((pidProfile->P8[PIDVEL] * error / 32), -300, +300);

    
    errorVelocityI += (pidProfile->I8[PIDVEL] * error);
    errorVelocityI = constrain(errorVelocityI, -(8192 * 200), (8192 * 200));
    result += errorVelocityI / 8192;     

    
    result -= constrain(pidProfile->D8[PIDVEL] * (accZ_tmp + accZ_old) / 512, -150, 150);

    return result;
}

void calculateEstimatedAltitude(uint32_t currentTime)
{
    static uint32_t previousTime;
    uint32_t dTime;
    int32_t baroVel;
    float dt;
    float vel_acc;
    int32_t vel_tmp;
    float accZ_tmp;
    int32_t sonarAlt = -1;
    static float accZ_old = 0.0f;
    static float vel = 0.0f;
    static float accAlt = 0.0f;
    static int32_t lastBaroAlt;

    static int32_t baroAlt_offset = 0;
    float sonarTransition;

#ifdef SONAR
    int16_t tiltAngle;
#endif

    dTime = currentTime - previousTime;
    if (dTime < BARO_UPDATE_FREQUENCY_40HZ)
        return;

    previousTime = currentTime;

#ifdef BARO
    if (!isBaroCalibrationComplete()) {
        performBaroCalibrationCycle();
        vel = 0;
        accAlt = 0;
    }

    BaroAlt = baroCalculateAltitude();
#else
    BaroAlt = 0;
#endif

#ifdef SONAR
    tiltAngle = calculateTiltAngle(&attitude);
    sonarAlt = sonarRead();
    sonarAlt = sonarCalculateAltitude(sonarAlt, tiltAngle);
#endif

    if (sonarAlt > 0 && sonarAlt < 200) {
        baroAlt_offset = BaroAlt - sonarAlt;
        BaroAlt = sonarAlt;
    } else {
        BaroAlt -= baroAlt_offset;
        if (sonarAlt > 0  && sonarAlt <= 300) {
            sonarTransition = (300 - sonarAlt) / 100.0f;
            BaroAlt = sonarAlt * sonarTransition + BaroAlt * (1.0f - sonarTransition);
        }
    }

    dt = accTimeSum * 1e-6f; 

    
    if (accSumCount) {
        accZ_tmp = (float)accSum[2] / (float)accSumCount;
    } else {
        accZ_tmp = 0;
    }
    vel_acc = accZ_tmp * accVelScale * (float)accTimeSum;

    
    accAlt += (vel_acc * 0.5f) * dt + vel * dt;                                                                 
    accAlt = accAlt * barometerConfig->baro_cf_alt + (float)BaroAlt * (1.0f - barometerConfig->baro_cf_alt);    
    vel += vel_acc;

#undef DEBUG_ALT_HOLD

#ifdef DEBUG_ALT_HOLD
    debug[1] = accSum[2] / accSumCount; 
    debug[2] = vel;                     
    debug[3] = accAlt;                  
#endif

    imuResetAccelerationSum();

#ifdef BARO
    if (!isBaroCalibrationComplete()) {
        return;
    }
#endif

    if (sonarAlt > 0 && sonarAlt < 200) {
        
        EstAlt = BaroAlt;
    } else {
        EstAlt = accAlt;
    }

    baroVel = (BaroAlt - lastBaroAlt) * 1000000.0f / dTime;
    lastBaroAlt = BaroAlt;

    baroVel = constrain(baroVel, -1500, 1500);  
    baroVel = applyDeadband(baroVel, 10);       

    
    
    vel = vel * barometerConfig->baro_cf_vel + baroVel * (1.0f - barometerConfig->baro_cf_vel);
	vel_tmp = (int32_t)(vel);

    
    vario = applyDeadband(vel_tmp, 5);

    altHoldThrottleAdjustment = calculateAltHoldThrottleAdjustment(vel_tmp, accZ_tmp, accZ_old);

    accZ_old = accZ_tmp;
}

int32_t altitudeHoldGetEstimatedAltitude(void)
{
    return EstAlt;
}

#endif

