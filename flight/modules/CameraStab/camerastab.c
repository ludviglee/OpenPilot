/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup CameraStab Camera Stabilization Module
 * @brief Camera stabilization module
 * Updates accessory outputs with values appropriate for camera stabilization
 * @{
 *
 * @file       camerastab.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Stabilize camera against the roll pitch and yaw of aircraft
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Output object: Accessory
 *
 * This module will periodically calculate the output values for stabilizing the camera
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */

#include "openpilot.h"

#include "accessorydesired.h"
#include "attitudestate.h"
#include "camerastabsettings.h"
#include "cameradesired.h"
#include "hwsettings.h"

//
// Configuration
//
#define SAMPLE_PERIOD_MS 10

// Private types

// Private variables
static struct CameraStab_data {
    portTickType lastSysTime;
    float inputs[CAMERASTABSETTINGS_INPUT_NUMELEM];

#ifdef USE_GIMBAL_LPF
    float attitudeFiltered[CAMERASTABSETTINGS_INPUT_NUMELEM];
#endif

#ifdef USE_GIMBAL_FF
    float ffLastAttitude[CAMERASTABSETTINGS_INPUT_NUMELEM];
    float ffLastAttitudeFiltered[CAMERASTABSETTINGS_INPUT_NUMELEM];
    float ffFilterAccumulator[CAMERASTABSETTINGS_INPUT_NUMELEM];
#endif
} *csd;

// Private functions
static void attitudeUpdated(UAVObjEvent *ev);
static float bound(float val, float limit);

#ifdef USE_GIMBAL_FF
static void applyFeedForward(uint8_t index, float dT, float *attitude, CameraStabSettingsData *cameraStab);
#endif


/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t CameraStabInitialize(void)
{
    bool cameraStabEnabled;

#ifdef MODULE_CAMERASTAB_BUILTIN
    cameraStabEnabled = true;
#else
    uint8_t optionalModules[HWSETTINGS_OPTIONALMODULES_NUMELEM];

    HwSettingsInitialize();
    HwSettingsOptionalModulesGet(optionalModules);

    if (optionalModules[HWSETTINGS_OPTIONALMODULES_CAMERASTAB] == HWSETTINGS_OPTIONALMODULES_ENABLED) {
        cameraStabEnabled = true;
    } else {
        cameraStabEnabled = false;
    }
#endif

    if (cameraStabEnabled) {
        // allocate and initialize the static data storage only if module is enabled
        csd = (struct CameraStab_data *)pvPortMalloc(sizeof(struct CameraStab_data));
        if (!csd) {
            return -1;
        }

        // initialize camera state variables
        memset(csd, 0, sizeof(struct CameraStab_data));
        csd->lastSysTime = xTaskGetTickCount();

        AttitudeStateInitialize();
        CameraStabSettingsInitialize();
        CameraDesiredInitialize();

        UAVObjEvent ev = {
            .obj    = AttitudeStateHandle(),
            .instId = 0,
            .event  = 0,
        };
        EventPeriodicCallbackCreate(&ev, attitudeUpdated, SAMPLE_PERIOD_MS / portTICK_RATE_MS);

        return 0;
    }

    return -1;
}

/* stub: module has no module thread */
int32_t CameraStabStart(void)
{
    return 0;
}

MODULE_INITCALL(CameraStabInitialize, CameraStabStart);

static void attitudeUpdated(UAVObjEvent *ev)
{
    if (ev->obj != AttitudeStateHandle()) {
        return;
    }

    AccessoryDesiredData accessory;

    CameraStabSettingsData cameraStab;
    CameraStabSettingsGet(&cameraStab);

    // check how long since last update, time delta between calls in ms
    portTickType thisSysTime = xTaskGetTickCount();
    float dT_millis = (thisSysTime > csd->lastSysTime) ?
                      (float)((thisSysTime - csd->lastSysTime) * portTICK_RATE_MS) :
                      (float)SAMPLE_PERIOD_MS;
    csd->lastSysTime = thisSysTime;

    // storage for elevon roll component before the pitch component has been generated
    // we are guaranteed that the iteration order of i is roll pitch yaw
    // that guarnteees this won't be used uninited, but the compiler doesn't know that
    // so we init it or turn the warning/error off for each compiler
    float elevon_roll = 0.0f;

    // process axes
    for (uint8_t i = 0; i < CAMERASTABSETTINGS_INPUT_NUMELEM; i++) {
        // read and process control input
        if (cameraStab.Input.data[i] != CAMERASTABSETTINGS_INPUT_NONE) {
            if (AccessoryDesiredInstGet(cameraStab.Input.data[i] - CAMERASTABSETTINGS_INPUT_ACCESSORY0, &accessory) == 0) {
                float input_rate;
                switch (cameraStab.StabilizationMode.data[i]) {
                case CAMERASTABSETTINGS_STABILIZATIONMODE_ATTITUDE:
                    csd->inputs[i] = accessory.AccessoryVal * cameraStab.InputRange.data[i];
                    break;
                case CAMERASTABSETTINGS_STABILIZATIONMODE_AXISLOCK:
                    input_rate     = accessory.AccessoryVal * cameraStab.InputRate.data[i];
                    if (fabsf(input_rate) > cameraStab.MaxAxisLockRate) {
                        csd->inputs[i] = bound(csd->inputs[i] + input_rate * 0.001f * dT_millis, cameraStab.InputRange.data[i]);
                    }
                    break;
                default:
                    PIOS_Assert(0);
                }
            }
        }

        // calculate servo output
        float attitude;

        switch (i) {
        case CAMERASTABSETTINGS_INPUT_ROLL:
            AttitudeStateRollGet(&attitude);
            break;
        case CAMERASTABSETTINGS_INPUT_PITCH:
            AttitudeStatePitchGet(&attitude);
            break;
        case CAMERASTABSETTINGS_INPUT_YAW:
            AttitudeStateYawGet(&attitude);
            break;
        default:
            PIOS_Assert(0);
        }

#ifdef USE_GIMBAL_LPF
        if (cameraStab.ResponseTime.data) {
            float rt = (float)cameraStab.ResponseTime.data[i];
            attitude = csd->attitudeFiltered[i] = ((rt * csd->attitudeFiltered[i]) + (dT_millis * attitude)) / (rt + dT_millis);
        }
#endif

#ifdef USE_GIMBAL_FF
        if (cameraStab.FeedForward.data[i]) {
            applyFeedForward(i, dT_millis, &attitude, &cameraStab);
        }
#endif

        // bounding for elevon mixing occurs on the unmixed output
        // to limit the range of the mixed output you must limit the range
        // of both the unmixed pitch and unmixed roll
        float output = bound((attitude + csd->inputs[i]) / cameraStab.OutputRange.data[i], 1.0f);

        // set output channels
        switch (i) {
        case CAMERASTABSETTINGS_INPUT_ROLL:
            // we are guaranteed that the iteration order of i is roll pitch yaw
            // for elevon mixing we simply grab the value for later use
            if (cameraStab.GimbalType == CAMERASTABSETTINGS_GIMBALTYPE_ROLLPITCHMIXED) {
                elevon_roll = output;
            } else {
                CameraDesiredRollOrServo1Set(&output);
            }
            break;
        case CAMERASTABSETTINGS_INPUT_PITCH:
            // we are guaranteed that the iteration order of i is roll pitch yaw
            // for elevon mixing we use the value we previously grabbed and set both s1 and s2
            if (cameraStab.GimbalType == CAMERASTABSETTINGS_GIMBALTYPE_ROLLPITCHMIXED) {
                float elevon_pitch = output;
                // elevon reversing works like this:
                // first use the normal reversing facilities to get servo 1 roll working in the correct direction
                // then use the normal reversing facilities to get servo 2 roll working in the correct direction
                // then use these new reversing switches to reverse servo 1 and/or 2 pitch as needed
                // if servo 1 pitch is reversed
                if (cameraStab.Servo1PitchReverse == CAMERASTABSETTINGS_SERVO1PITCHREVERSE_TRUE) {
                    // use (reversed pitch) + roll
                    output = ((1.0f - elevon_pitch) + elevon_roll) / 2.0f;
                } else {
                    // use pitch + roll
                    output = (elevon_pitch + elevon_roll) / 2.0f;
                }
                CameraDesiredRollOrServo1Set(&output);
                // if servo 2 pitch is reversed
                if (cameraStab.Servo2PitchReverse == CAMERASTABSETTINGS_SERVO2PITCHREVERSE_TRUE) {
                    // use (reversed pitch) - roll
                    output = ((1.0f - elevon_pitch) - elevon_roll) / 2.0f;
                } else {
                    // use pitch - roll
                    output = (elevon_pitch - elevon_roll) / 2.0f;
                }
                CameraDesiredPitchOrServo2Set(&output);
            } else {
                CameraDesiredPitchOrServo2Set(&output);
            }
            break;
        case CAMERASTABSETTINGS_INPUT_YAW:
            CameraDesiredYawSet(&output);
            break;
        default:
            PIOS_Assert(0);
        }
    }
}

float bound(float val, float limit)
{
    return (val > limit) ? limit :
           (val < -limit) ? -limit :
           val;
}

#ifdef USE_GIMBAL_FF
void applyFeedForward(uint8_t index, float dT_millis, float *attitude, CameraStabSettingsData *cameraStab)
{
    // compensate high feed forward values depending on gimbal type
    float gimbalTypeCorrection = 1.0f;

    switch (cameraStab->GimbalType) {
    case CAMERASTABSETTINGS_GIMBALTYPE_GENERIC:
    case CAMERASTABSETTINGS_GIMBALTYPE_ROLLPITCHMIXED:
        // no correction
        break;
    case CAMERASTABSETTINGS_GIMBALTYPE_YAWROLLPITCH:
        if (index == CAMERASTABSETTINGS_INPUT_ROLL) {
            float pitch;
            AttitudeStatePitchGet(&pitch);
            gimbalTypeCorrection = (cameraStab->OutputRange.fields.Pitch - fabsf(pitch))
                                   / cameraStab->OutputRange.fields.Pitch;
        }
        break;
    case CAMERASTABSETTINGS_GIMBALTYPE_YAWPITCHROLL:
        if (index == CAMERASTABSETTINGS_INPUT_PITCH) {
            float roll;
            AttitudeStateRollGet(&roll);
            gimbalTypeCorrection = (cameraStab->OutputRange.fields.Roll - fabsf(roll))
                                   / cameraStab->OutputRange.fields.Roll;
        }
        break;
    default:
        PIOS_Assert(0);
    }

    // apply feed forward
    float accumulator = csd->ffFilterAccumulator[index];
    accumulator += (*attitude - csd->ffLastAttitude[index]) * (float)cameraStab->FeedForward.data[index] * gimbalTypeCorrection;
    csd->ffLastAttitude[index] = *attitude;
    *attitude   += accumulator;

    float filter = (float)((accumulator > 0.0f) ? cameraStab->AccelTime.data[index] : cameraStab->DecelTime.data[index]) / dT_millis;
    if (filter < 1.0f) {
        filter = 1.0f;
    }
    accumulator -= accumulator / filter;
    csd->ffFilterAccumulator[index] = accumulator;
    *attitude   += accumulator;

    // apply acceleration limit
    float delta    = *attitude - csd->ffLastAttitudeFiltered[index];
    float maxDelta = (float)cameraStab->MaxAccel * 0.001f * dT_millis;

    if (fabsf(delta) > maxDelta) {
        // we are accelerating too hard
        *attitude = csd->ffLastAttitudeFiltered[index] + ((delta > 0.0f) ? maxDelta : -maxDelta);
    }
    csd->ffLastAttitudeFiltered[index] = *attitude;
}
#endif // USE_GIMBAL_FF

/**
 * @}
 */

/**
 * @}
 */
