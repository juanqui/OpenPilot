/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup TxPIDModule TxPID Module
 * @brief      Optional module to tune PID settings using R/C transmitter.
 * Updates PID settings in RAM in real-time using configured Accessory channels as controllers.
 * @{
 *
 * @file       txpid.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2011.
 * @brief      Optional module to tune PID settings using R/C transmitter.
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
 * Output object: StabilizationSettings
 *
 * This module will periodically update values of stabilization PID settings
 * depending on configured input control channels. New values of stabilization
 * settings are not saved to flash, but updated in RAM. It is expected that the
 * module will be enabled only for tuning. When desired values are found, they
 * can be read via GCS and saved permanently. Then this module should be
 * disabled again.
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://wiki.openpilot.org/display/Doc/OpenPilot+Architecture
 *
 */

#include "openpilot.h"

#include "txpidsettings.h"
#include "accessorydesired.h"
#include "manualcontrolcommand.h"
#include "stabilizationsettings.h"
#include "flightstatus.h"
#include "hwsettings.h"

//
// Configuration
//
#define SAMPLE_PERIOD_MS           200
#define TELEMETRY_UPDATE_PERIOD_MS 0 // 0 = update on change (default)

// Sanity checks
#if (TXPIDSETTINGS_PIDS_NUMELEM != TXPIDSETTINGS_INPUTS_NUMELEM) || \
    (TXPIDSETTINGS_PIDS_NUMELEM != TXPIDSETTINGS_MINPID_NUMELEM) || \
    (TXPIDSETTINGS_PIDS_NUMELEM != TXPIDSETTINGS_MAXPID_NUMELEM)
#error Invalid TxPID UAVObject definition (inconsistent number of field elements)
#endif

// Private types

// Private variables

// Private functions
static void updatePIDs(UAVObjEvent *ev);
static uint8_t update(float *var, float val);
static float scale(float val, float inMin, float inMax, float outMin, float outMax);

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t TxPIDInitialize(void)
{
    bool txPIDEnabled;
    uint8_t optionalModules[HWSETTINGS_OPTIONALMODULES_NUMELEM];

    HwSettingsInitialize();
    HwSettingsOptionalModulesGet(optionalModules);

    if (optionalModules[HWSETTINGS_OPTIONALMODULES_TXPID] == HWSETTINGS_OPTIONALMODULES_ENABLED) {
        txPIDEnabled = true;
    } else {
        txPIDEnabled = false;
    }

    if (txPIDEnabled) {
        TxPIDSettingsInitialize();
        AccessoryDesiredInitialize();

        UAVObjEvent ev = {
            .obj    = AccessoryDesiredHandle(),
            .instId = 0,
            .event  = 0,
        };
        EventPeriodicCallbackCreate(&ev, updatePIDs, SAMPLE_PERIOD_MS / portTICK_RATE_MS);

#if (TELEMETRY_UPDATE_PERIOD_MS != 0)
        // Change StabilizationSettings update rate from OnChange to periodic
        // to prevent telemetry link flooding with frequent updates in case of
        // control channel jitter.
        // Warning: saving to flash with this code active will change the
        // StabilizationSettings update rate permanently. Use Metadata via
        // browser to reset to defaults (telemetryAcked=true, OnChange).
        UAVObjMetadata metadata;
        StabilizationSettingsInitialize();
        StabilizationSettingsGetMetadata(&metadata);
        metadata.telemetryAcked = 0;
        metadata.telemetryUpdateMode   = UPDATEMODE_PERIODIC;
        metadata.telemetryUpdatePeriod = TELEMETRY_UPDATE_PERIOD_MS;
        StabilizationSettingsSetMetadata(&metadata);
#endif

        return 0;
    }

    return -1;
}

/* stub: module has no module thread */
int32_t TxPIDStart(void)
{
    return 0;
}

MODULE_INITCALL(TxPIDInitialize, TxPIDStart)

/**
 * Update PIDs callback function
 */
static void updatePIDs(UAVObjEvent *ev)
{
    if (ev->obj != AccessoryDesiredHandle()) {
        return;
    }

    TxPIDSettingsData inst;
    TxPIDSettingsGet(&inst);

    if (inst.UpdateMode == TXPIDSETTINGS_UPDATEMODE_NEVER) {
        return;
    }

    uint8_t armed;
    FlightStatusArmedGet(&armed);
    if ((inst.UpdateMode == TXPIDSETTINGS_UPDATEMODE_WHENARMED) &&
        (armed == FLIGHTSTATUS_ARMED_DISARMED)) {
        return;
    }

    StabilizationSettingsData stab;
    StabilizationSettingsGet(&stab);
    AccessoryDesiredData accessory;

    uint8_t needsUpdate = 0;

    // Loop through every enabled instance
    for (uint8_t i = 0; i < TXPIDSETTINGS_PIDS_NUMELEM; i++) {
        if (inst.PIDs[i] != TXPIDSETTINGS_PIDS_DISABLED) {
            float value;
            if (inst.Inputs[i] == TXPIDSETTINGS_INPUTS_THROTTLE) {
                ManualControlCommandThrottleGet(&value);
                value = scale(value,
                              inst.ThrottleRange[TXPIDSETTINGS_THROTTLERANGE_MIN],
                              inst.ThrottleRange[TXPIDSETTINGS_THROTTLERANGE_MAX],
                              inst.MinPID[i], inst.MaxPID[i]);
            } else if (AccessoryDesiredInstGet(inst.Inputs[i] - TXPIDSETTINGS_INPUTS_ACCESSORY0, &accessory) == 0) {
                value = scale(accessory.AccessoryVal, -1.0f, 1.0f, inst.MinPID[i], inst.MaxPID[i]);
            } else {
                continue;
            }

            switch (inst.PIDs[i]) {
            case TXPIDSETTINGS_PIDS_ROLLRATEKP:
                needsUpdate |= update(&stab.RollRatePID[STABILIZATIONSETTINGS_ROLLRATEPID_KP], value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLRATEKI:
                needsUpdate |= update(&stab.RollRatePID[STABILIZATIONSETTINGS_ROLLRATEPID_KI], value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLRATEKD:
                needsUpdate |= update(&stab.RollRatePID[STABILIZATIONSETTINGS_ROLLRATEPID_KD], value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLRATEILIMIT:
                needsUpdate |= update(&stab.RollRatePID[STABILIZATIONSETTINGS_ROLLRATEPID_ILIMIT], value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLATTITUDEKP:
                needsUpdate |= update(&stab.RollPI[STABILIZATIONSETTINGS_ROLLPI_KP], value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLATTITUDEKI:
                needsUpdate |= update(&stab.RollPI[STABILIZATIONSETTINGS_ROLLPI_KI], value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLATTITUDEILIMIT:
                needsUpdate |= update(&stab.RollPI[STABILIZATIONSETTINGS_ROLLPI_ILIMIT], value);
                break;
            case TXPIDSETTINGS_PIDS_PITCHRATEKP:
                needsUpdate |= update(&stab.PitchRatePID[STABILIZATIONSETTINGS_PITCHRATEPID_KP], value);
                break;
            case TXPIDSETTINGS_PIDS_PITCHRATEKI:
                needsUpdate |= update(&stab.PitchRatePID[STABILIZATIONSETTINGS_PITCHRATEPID_KI], value);
                break;
            case TXPIDSETTINGS_PIDS_PITCHRATEKD:
                needsUpdate |= update(&stab.PitchRatePID[STABILIZATIONSETTINGS_PITCHRATEPID_KD], value);
                break;
            case TXPIDSETTINGS_PIDS_PITCHRATEILIMIT:
                needsUpdate |= update(&stab.PitchRatePID[STABILIZATIONSETTINGS_PITCHRATEPID_ILIMIT], value);
                break;
            case TXPIDSETTINGS_PIDS_PITCHATTITUDEKP:
                needsUpdate |= update(&stab.PitchPI[STABILIZATIONSETTINGS_PITCHPI_KP], value);
                break;
            case TXPIDSETTINGS_PIDS_PITCHATTITUDEKI:
                needsUpdate |= update(&stab.PitchPI[STABILIZATIONSETTINGS_PITCHPI_KI], value);
                break;
            case TXPIDSETTINGS_PIDS_PITCHATTITUDEILIMIT:
                needsUpdate |= update(&stab.PitchPI[STABILIZATIONSETTINGS_PITCHPI_ILIMIT], value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLPITCHRATEKP:
                needsUpdate |= update(&stab.RollRatePID[STABILIZATIONSETTINGS_ROLLRATEPID_KP], value);
                needsUpdate |= update(&stab.PitchRatePID[STABILIZATIONSETTINGS_PITCHRATEPID_KP], value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLPITCHRATEKI:
                needsUpdate |= update(&stab.RollRatePID[STABILIZATIONSETTINGS_ROLLRATEPID_KI], value);
                needsUpdate |= update(&stab.PitchRatePID[STABILIZATIONSETTINGS_PITCHRATEPID_KI], value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLPITCHRATEKD:
                needsUpdate |= update(&stab.RollRatePID[STABILIZATIONSETTINGS_ROLLRATEPID_KD], value);
                needsUpdate |= update(&stab.PitchRatePID[STABILIZATIONSETTINGS_PITCHRATEPID_KD], value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLPITCHRATEILIMIT:
                needsUpdate |= update(&stab.RollRatePID[STABILIZATIONSETTINGS_ROLLRATEPID_ILIMIT], value);
                needsUpdate |= update(&stab.PitchRatePID[STABILIZATIONSETTINGS_PITCHRATEPID_ILIMIT], value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLPITCHATTITUDEKP:
                needsUpdate |= update(&stab.RollPI[STABILIZATIONSETTINGS_ROLLPI_KP], value);
                needsUpdate |= update(&stab.PitchPI[STABILIZATIONSETTINGS_PITCHPI_KP], value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLPITCHATTITUDEKI:
                needsUpdate |= update(&stab.RollPI[STABILIZATIONSETTINGS_ROLLPI_KI], value);
                needsUpdate |= update(&stab.PitchPI[STABILIZATIONSETTINGS_PITCHPI_KI], value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLPITCHATTITUDEILIMIT:
                needsUpdate |= update(&stab.RollPI[STABILIZATIONSETTINGS_ROLLPI_ILIMIT], value);
                needsUpdate |= update(&stab.PitchPI[STABILIZATIONSETTINGS_PITCHPI_ILIMIT], value);
                break;
            case TXPIDSETTINGS_PIDS_YAWRATEKP:
                needsUpdate |= update(&stab.YawRatePID[STABILIZATIONSETTINGS_YAWRATEPID_KP], value);
                break;
            case TXPIDSETTINGS_PIDS_YAWRATEKI:
                needsUpdate |= update(&stab.YawRatePID[STABILIZATIONSETTINGS_YAWRATEPID_KI], value);
                break;
            case TXPIDSETTINGS_PIDS_YAWRATEKD:
                needsUpdate |= update(&stab.YawRatePID[STABILIZATIONSETTINGS_YAWRATEPID_KD], value);
                break;
            case TXPIDSETTINGS_PIDS_YAWRATEILIMIT:
                needsUpdate |= update(&stab.YawRatePID[STABILIZATIONSETTINGS_YAWRATEPID_ILIMIT], value);
                break;
            case TXPIDSETTINGS_PIDS_YAWATTITUDEKP:
                needsUpdate |= update(&stab.YawPI[STABILIZATIONSETTINGS_YAWPI_KP], value);
                break;
            case TXPIDSETTINGS_PIDS_YAWATTITUDEKI:
                needsUpdate |= update(&stab.YawPI[STABILIZATIONSETTINGS_YAWPI_KI], value);
                break;
            case TXPIDSETTINGS_PIDS_YAWATTITUDEILIMIT:
                needsUpdate |= update(&stab.YawPI[STABILIZATIONSETTINGS_YAWPI_ILIMIT], value);
                break;
            case TXPIDSETTINGS_PIDS_GYROTAU:
                needsUpdate |= update(&stab.GyroTau, value);
                break;
            default:
                PIOS_Assert(0);
            }
        }
    }
    if (needsUpdate) {
        StabilizationSettingsSet(&stab);
    }
}

/**
 * Scales input val from [inMin..inMax] range to [outMin..outMax].
 * If val is out of input range (inMin <= inMax), it will be bound.
 * (outMin > outMax) is ok, in that case output will be decreasing.
 *
 * \returns scaled value
 */
static float scale(float val, float inMin, float inMax, float outMin, float outMax)
{
    // bound input value
    if (val > inMax) {
        val = inMax;
    }
    if (val < inMin) {
        val = inMin;
    }

    // normalize input value to [0..1]
    if (inMax <= inMin) {
        val = 0.0f;
    } else {
        val = (val - inMin) / (inMax - inMin);
    }

    // update output bounds
    if (outMin > outMax) {
        float t = outMin;
        outMin = outMax;
        outMax = t;
        val    = 1.0f - val;
    }

    return (outMax - outMin) * val + outMin;
}

/**
 * Updates var using val if needed.
 * \returns 1 if updated, 0 otherwise
 */
static uint8_t update(float *var, float val)
{
    /* FIXME: this is not an entirely correct way
     * to check if the two floating point
     * numbers are 'not equal'.
     * Epsilon of 1e-9 is probably okay for the range
     * of numbers we see here*/
    if (fabsf(*var - val) > 1e-9f) {
        *var = val;
        return 1;
    }
    return 0;
}

/**
 * @}
 */

/**
 * @}
 */
