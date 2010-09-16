/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{ 
 * @addtogroup AHRSCommsModule AHRSComms Module
 * @brief Handles communication with AHRS and updating position
 * Specifically updates the the @ref AttitudeActual "AttitudeActual" and @ref AttitudeRaw "AttitudeRaw" settings objects
 * @{ 
 *
 * @file       ahrs_comms.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Module to handle all comms to the AHRS on a periodic basis.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 ******************************************************************************/
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
 * Input object: AttitudeSettings
 * Output object: AttitudeActual
 *
 * This module will periodically update the value of latest attitude solution
 * that is available from the AHRS.
 * The module settings can configure how often AHRS is polled for a new solution.
 *
 * The module executes in its own thread.
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

#include "ahrs_comms.h"
#include "attitudeactual.h"
#include "ahrssettings.h"
#include "attituderaw.h"
#include "attitudesettings.h"
#include "ahrsstatus.h"
#include "alarms.h"
#include "baroaltitude.h"
#include "stdbool.h"
#include "gpsposition.h"
#include "positionactual.h"
#include "homelocation.h"
#include "ahrscalibration.h"
#include "CoordinateConversions.h"

#include "pios_opahrs.h" // library for OpenPilot AHRS access functions
#include "pios_opahrs_proto.h"

// Private constants
#define STACK_SIZE 400
#define TASK_PRIORITY (tskIDLE_PRIORITY+4)

// Private types

// Private variables
static xTaskHandle taskHandle;

// Private functions
static void ahrscommsTask(void* parameters);
static void load_baro_altitude(struct opahrs_msg_v1_req_update * update);
static void load_gps_position(struct opahrs_msg_v1_req_update * update);
static void load_magnetic_north(struct opahrs_msg_v1_req_north * north);
static void load_calibration(struct opahrs_msg_v1_req_calibration * calibration);
static void update_attitude_raw(struct opahrs_msg_v1_rsp_attituderaw * attituderaw);
static void update_ahrs_status(struct opahrs_msg_v1_rsp_serial * serial);
static void update_calibration(struct opahrs_msg_v1_rsp_calibration * calibration);
static void process_update(struct opahrs_msg_v1_rsp_update * update); // main information parser

static bool AHRSSettingsIsUpdatedFlag = false;
static void AHRSSettingsUpdatedCb(UAVObjEvent * ev)
{
  AHRSSettingsIsUpdatedFlag = true;
}

static bool BaroAltitudeIsUpdatedFlag = false;
static void BaroAltitudeUpdatedCb(UAVObjEvent * ev)
{
  BaroAltitudeIsUpdatedFlag = true;
}

static bool GPSPositionIsUpdatedFlag = false;
static void GPSPositionUpdatedCb(UAVObjEvent * ev)
{
  GPSPositionIsUpdatedFlag = true;
}

static bool HomeLocationIsUpdatedFlag = false;
static void HomeLocationUpdatedCb(UAVObjEvent * ev)
{
  HomeLocationIsUpdatedFlag = true;
}

static bool AHRSCalibrationIsUpdatedFlag = false;
static bool AHRSCalibrationIsLocallyUpdateFlag = false;
static void AHRSCalibrationUpdatedCb(UAVObjEvent * ev)
{
  if(AHRSCalibrationIsLocallyUpdateFlag == true)
    AHRSCalibrationIsLocallyUpdateFlag = false;
  else
    AHRSCalibrationIsUpdatedFlag = true;
}

static uint32_t GPSGoodUpdates;

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AHRSCommsInitialize(void)
{
  AHRSSettingsConnectCallback(AHRSSettingsUpdatedCb);
  BaroAltitudeConnectCallback(BaroAltitudeUpdatedCb);
  GPSPositionConnectCallback(GPSPositionUpdatedCb);
  HomeLocationConnectCallback(HomeLocationUpdatedCb);
  AHRSCalibrationConnectCallback(AHRSCalibrationUpdatedCb);

  PIOS_OPAHRS_Init();

  // Start main task
  xTaskCreate(ahrscommsTask, (signed char*)"AHRSComms", STACK_SIZE, NULL, TASK_PRIORITY, &taskHandle);

  return 0;
}

static uint16_t update_errors      = 0;
static uint16_t attituderaw_errors = 0;
static uint16_t home_errors        = 0;
static uint16_t calibration_errors = 0;
static uint16_t algorithm_errors   = 0;

/**
 * Module thread, should not return.
 */
static void ahrscommsTask(void* parameters)
{
  enum opahrs_result result;
  portTickType lastSysTime;
  
  GPSGoodUpdates = 0;
    
  // Main task loop
  while (1) {
    struct opahrs_msg_v1 rsp;
    AhrsStatusData data;

    AlarmsSet(SYSTEMALARMS_ALARM_AHRSCOMMS, SYSTEMALARMS_ALARM_CRITICAL);

    /* Whenever resyncing, assume AHRS doesn't reset and doesn't know home */
    AhrsStatusGet(&data);
    data.HomeSet = AHRSSTATUS_HOMESET_FALSE;
    data.CalibrationSet = AHRSSTATUS_CALIBRATIONSET_FALSE;
    data.AlgorithmSet = AHRSSTATUS_CALIBRATIONSET_FALSE;
    AhrsStatusSet(&data);
    
    /* Spin here until we're in sync */
    while (PIOS_OPAHRS_resync() != OPAHRS_RESULT_OK) {
      vTaskDelay(100 / portTICK_RATE_MS);
    }
      
    /* Give the other side a chance to keep up */
    //vTaskDelay(100 / portTICK_RATE_MS);

    if (PIOS_OPAHRS_GetSerial(&rsp) == OPAHRS_RESULT_OK) {
      update_ahrs_status(&(rsp.payload.user.v.rsp.serial));
    } else {
      /* Comms error */
      continue;
    }

    AlarmsClear(SYSTEMALARMS_ALARM_AHRSCOMMS);

    /* We're in sync with the AHRS, spin here until an error occurs */
    lastSysTime = xTaskGetTickCount();
    while (1) {
      AHRSSettingsData settings;

      /* Update settings with latest value */
      AHRSSettingsGet(&settings);

      // Update home coordinate if it hasn't been updated
      AhrsStatusGet(&data);
      if (HomeLocationIsUpdatedFlag || (data.HomeSet == AHRSSTATUS_HOMESET_FALSE)) {
        struct opahrs_msg_v1 req;
        
        load_magnetic_north(&(req.payload.user.v.req.north));
        if ((result = PIOS_OPAHRS_SetMagNorth(&req)) == OPAHRS_RESULT_OK) {
          HomeLocationIsUpdatedFlag = false;
          data.HomeSet = AHRSSTATUS_HOMESET_TRUE;
          AhrsStatusSet(&data);
        } else {
          /* Comms error */
          home_errors++;
          data.HomeSet = AHRSSTATUS_HOMESET_FALSE;
          AhrsStatusSet(&data);
          break;
        }
      }    
      
      // Update calibration if necessary
      AhrsStatusGet(&data);
      if (AHRSCalibrationIsUpdatedFlag || (data.CalibrationSet == AHRSSTATUS_CALIBRATIONSET_FALSE)) 
      {
        struct opahrs_msg_v1 req;
        struct opahrs_msg_v1 rsp;
        load_calibration(&(req.payload.user.v.req.calibration));
        if(( result = PIOS_OPAHRS_SetGetCalibration(&req,&rsp) ) == OPAHRS_RESULT_OK ) {
          update_calibration(&(rsp.payload.user.v.rsp.calibration));
          AHRSCalibrationIsUpdatedFlag = false;
          if(rsp.payload.user.v.rsp.calibration.measure_var != AHRS_ECHO)
            data.CalibrationSet = AHRSSTATUS_CALIBRATIONSET_TRUE;
          AhrsStatusSet(&data);
          
        } else {
          /* Comms error */
          data.CalibrationSet = AHRSSTATUS_CALIBRATIONSET_FALSE;
          AhrsStatusSet(&data);
          calibration_errors++;
          break;
        }
      }
            
      // Update algorithm
      if (AHRSSettingsIsUpdatedFlag || (data.AlgorithmSet == AHRSSTATUS_ALGORITHMSET_FALSE)) 
      {
        struct opahrs_msg_v1 req;
        
        if(settings.Algorithm == AHRSSETTINGS_ALGORITHM_INSGPS)
          req.payload.user.v.req.algorithm.algorithm = INSGPS_Algo;
        if(settings.Algorithm == AHRSSETTINGS_ALGORITHM_SIMPLE)
          req.payload.user.v.req.algorithm.algorithm = SIMPLE_Algo;

        if(( result = PIOS_OPAHRS_SetAlgorithm(&req) ) == OPAHRS_RESULT_OK ) {
          data.AlgorithmSet = AHRSSTATUS_ALGORITHMSET_TRUE;
          AhrsStatusSet(&data);          
        } else {
          /* Comms error */
          data.AlgorithmSet = AHRSSTATUS_ALGORITHMSET_FALSE;
          AhrsStatusSet(&data);
          algorithm_errors++;
          break;
        }
      }
      
      

      // If settings indicate, grab the raw and filtered data instead of estimate
      if (settings.UpdateRaw)
      {
        if( (result = PIOS_OPAHRS_GetAttitudeRaw(&rsp)) == OPAHRS_RESULT_OK) {
          update_attitude_raw(&(rsp.payload.user.v.rsp.attituderaw));
        } else {
          /* Comms error */
          attituderaw_errors++;
          break;
        }
      }

      if (settings.UpdateFiltered)
      {
        // Otherwise do standard technique
        struct opahrs_msg_v1 req;
        struct opahrs_msg_v1 rsp;
        
        // Load barometer if updated
        if (BaroAltitudeIsUpdatedFlag)       
          load_baro_altitude(&(req.payload.user.v.req.update));
        else 
          req.payload.user.v.req.update.barometer.updated = 0;

        // Load GPS if updated
        if (GPSPositionIsUpdatedFlag) 
          load_gps_position(&(req.payload.user.v.req.update));
        else 
          req.payload.user.v.req.update.gps.updated = 0;

        // Transfer packet and process returned attitude
        if ((result = PIOS_OPAHRS_SetGetUpdate(&req,&rsp)) == OPAHRS_RESULT_OK) {
          if (req.payload.user.v.req.update.barometer.updated) 
            BaroAltitudeIsUpdatedFlag = false;
          if (req.payload.user.v.req.update.gps.updated)
            GPSPositionIsUpdatedFlag = false;        
          process_update(&(rsp.payload.user.v.rsp.update));
        } else {
          /* Comms error */
          update_errors++;
          break;
        }
      }

      /* Wait for the next update interval */
      vTaskDelayUntil(&lastSysTime, settings.UpdatePeriod / portTICK_RATE_MS );
    }
  }
}

static void load_calibration(struct opahrs_msg_v1_req_calibration * calibration) 
{
  AHRSCalibrationData data;
  AHRSCalibrationGet(&data);
  if(data.measure_var == AHRSCALIBRATION_MEASURE_VAR_SET)
    calibration->measure_var  = AHRS_SET;
  else if(data.measure_var == AHRSCALIBRATION_MEASURE_VAR_MEASURE)
    calibration->measure_var = AHRS_MEASURE;
  else 
    calibration->measure_var = AHRS_ECHO;

  calibration->accel_bias[0]  = data.accel_bias[0];
  calibration->accel_bias[1]  = data.accel_bias[1];
  calibration->accel_bias[2]  = data.accel_bias[2];
  calibration->accel_scale[0] = data.accel_scale[0];
  calibration->accel_scale[1] = data.accel_scale[1];
  calibration->accel_scale[2] = data.accel_scale[2];
  calibration->accel_var[0]   = data.accel_var[0];
  calibration->accel_var[1]   = data.accel_var[1];
  calibration->accel_var[2]   = data.accel_var[2];
  calibration->gyro_bias[0]   = data.gyro_bias[0];
  calibration->gyro_bias[1]   = data.gyro_bias[1];
  calibration->gyro_bias[2]   = data.gyro_bias[2];
  calibration->gyro_scale[0]  = data.gyro_scale[0];
  calibration->gyro_scale[1]  = data.gyro_scale[1];
  calibration->gyro_scale[2]  = data.gyro_scale[2];
  calibration->gyro_var[0]    = data.gyro_var[0];
  calibration->gyro_var[1]    = data.gyro_var[1];
  calibration->gyro_var[2]    = data.gyro_var[2];
  calibration->mag_bias[0]    = data.mag_bias[0];
  calibration->mag_bias[1]    = data.mag_bias[1];
  calibration->mag_bias[2]    = data.mag_bias[2];
  calibration->mag_var[0]     = data.mag_var[0];
  calibration->mag_var[1]     = data.mag_var[1];
  calibration->mag_var[2]     = data.mag_var[2];
  
}

static void update_calibration(struct opahrs_msg_v1_rsp_calibration * calibration) 
{
  AHRSCalibrationData data;
  AHRSCalibrationGet(&data);
  
  AHRSCalibrationIsLocallyUpdateFlag = true;
  data.accel_var[0] = calibration->accel_var[0];
  data.accel_var[1] = calibration->accel_var[1];
  data.accel_var[2] = calibration->accel_var[2];
  data.gyro_var[0]  = calibration->gyro_var[0];
  data.gyro_var[1]  = calibration->gyro_var[1];
  data.gyro_var[2]  = calibration->gyro_var[2];
  data.mag_var[0]   = calibration->mag_var[0];
  data.mag_var[1]   = calibration->mag_var[1];
  data.mag_var[2]   = calibration->mag_var[2];  
  AHRSCalibrationSet(&data);

}

static void load_magnetic_north(struct opahrs_msg_v1_req_north * mag_north)
{
  HomeLocationData   data;
  
  HomeLocationGet(&data);
  mag_north->Be[0] = data.Be[0];
  mag_north->Be[1] = data.Be[1];
  mag_north->Be[2] = data.Be[2];

  if(data.Be[0] == 0 && data.Be[1] == 0 && data.Be[2] == 0) {
    // in case nothing has been set go to default to prevent NaN in attitude solution
    mag_north->Be[0] = 1;  
    mag_north->Be[1] = 0;
    mag_north->Be[2] = 0;
  } else {
    // normalize for unit length here
    float len = sqrt(data.Be[0] * data.Be[0] + data.Be[1] * data.Be[1] + data.Be[2] * data.Be[2]);
    mag_north->Be[0] = data.Be[0] / len;
    mag_north->Be[1] = data.Be[1] / len;
    mag_north->Be[2] = data.Be[2] / len;
  }
}

static void load_baro_altitude(struct opahrs_msg_v1_req_update * update)
{
  BaroAltitudeData   data;

  BaroAltitudeGet(&data);

  update->barometer.altitude = data.Altitude;
  update->barometer.updated = TRUE;
}

static void load_gps_position(struct opahrs_msg_v1_req_update * update)
{
    GPSPositionData data;
    GPSPositionGet(&data);
    HomeLocationData home;
    HomeLocationGet(&home);
    
    update->gps.updated = TRUE;
    
    if(home.Set == HOMELOCATION_SET_FALSE || home.Indoor == HOMELOCATION_INDOOR_TRUE) {
        PositionActualData pos;
        PositionActualGet(&pos);
        
        update->gps.NED[0] = 0;
        update->gps.NED[1] = 0;
        update->gps.NED[2] = 0;
        update->gps.groundspeed = 0;
        update->gps.heading = 0;
        update->gps.quality = -1; // indicates indoor mode, high variance zeros update
    } else {
        // TODO: Parameterize these heuristics into the settings
        if(data.Satellites >= 7 && data.PDOP < 3.5) {
            if(GPSGoodUpdates < 30) {
                GPSGoodUpdates++;
                update->gps.quality = 0;
            } else {                            
                update->gps.groundspeed = data.Groundspeed;
                update->gps.heading = data.Heading;
                double LLA[3] = {(double) data.Latitude / 1e7, (double) data.Longitude / 1e7, (double) (data.GeoidSeparation + data.Altitude)};
                // convert from cm back to meters
                double ECEF[3] = {(double) (home.ECEF[0] / 100), (double) (home.ECEF[1] / 100), (double) (home.ECEF[2] / 100)};
                LLA2Base(LLA, ECEF, (float (*)[3]) home.RNE, update->gps.NED);
                update->gps.quality = 1;
            }
        } else {
            GPSGoodUpdates = 0;
            update->gps.quality = 0;
        }
    }    
}

static void process_update(struct opahrs_msg_v1_rsp_update * update)
{
    AttitudeActualData   data;
    AttitudeSettingsData attitudeSettings;
    AttitudeSettingsGet(&attitudeSettings);
    
    data.q1 = update->quaternion.q1;
    data.q2 = update->quaternion.q2;
    data.q3 = update->quaternion.q3;
    data.q4 = update->quaternion.q4;
    
    float q[4] = {data.q1, data.q2, data.q3, data.q4};
    float rpy[3];
    Quaternion2RPY(q,rpy);
    data.Roll    = rpy[0] - attitudeSettings.RollBias;
    data.Pitch   = rpy[1] - attitudeSettings.PitchBias;
    data.Yaw     = rpy[2];
    if(data.Yaw < 0) data.Yaw += 360;
    
    AttitudeActualSet(&data);
    
    PositionActualData pos;
    PositionActualGet(&pos);    
    pos.NED[0] = update->NED[0];
    pos.NED[1] = update->NED[1];
    pos.NED[2] = update->NED[2];
    pos.Vel[0] = update->Vel[0];
    pos.Vel[1] = update->Vel[1];
    pos.Vel[2] = update->Vel[2];
    PositionActualSet(&pos);
    
    AhrsStatusData status;
    AhrsStatusGet(&status);
    status.CPULoad = update->load;
	status.IdleTimePerCyle = update->idle_time;
	status.RunningTimePerCyle = update->run_time;
	status.DroppedUpdates = update->dropped_updates;
    AhrsStatusSet(&status);
}

static void update_attitude_raw(struct opahrs_msg_v1_rsp_attituderaw * attituderaw)
{
  AttitudeRawData    data;

  data.magnetometers[ATTITUDERAW_MAGNETOMETERS_X] = attituderaw->mags.x;
  data.magnetometers[ATTITUDERAW_MAGNETOMETERS_Y] = attituderaw->mags.y;
  data.magnetometers[ATTITUDERAW_MAGNETOMETERS_Z] = attituderaw->mags.z;

  data.gyros[ATTITUDERAW_GYROS_X] = attituderaw->gyros.x;
  data.gyros[ATTITUDERAW_GYROS_Y] = attituderaw->gyros.y;
  data.gyros[ATTITUDERAW_GYROS_Z] = attituderaw->gyros.z;

  data.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_X] = attituderaw->gyros_filtered.x;
  data.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_Y] = attituderaw->gyros_filtered.y;
  data.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_Z] = attituderaw->gyros_filtered.z;
  
  data.gyrotemp[ATTITUDERAW_GYROTEMP_XY] = attituderaw->gyros.xy_temp;
  data.gyrotemp[ATTITUDERAW_GYROTEMP_Z]  = attituderaw->gyros.z_temp;

  data.accels[ATTITUDERAW_ACCELS_X] = attituderaw->accels.x;
  data.accels[ATTITUDERAW_ACCELS_Y] = attituderaw->accels.y;
  data.accels[ATTITUDERAW_ACCELS_Z] = attituderaw->accels.z;

  data.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_X] = attituderaw->accels_filtered.x;
  data.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_Y] = attituderaw->accels_filtered.y;
  data.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_Z] = attituderaw->accels_filtered.z;
  
  AttitudeRawSet(&data);
}

static void update_ahrs_status(struct opahrs_msg_v1_rsp_serial * serial)
{
  AhrsStatusData       data;

  // Get the current object data
  AhrsStatusGet(&data);

  for (uint8_t i = 0; i < sizeof(serial->serial_bcd); i++) {
    data.SerialNumber[i] = serial->serial_bcd[i];
  }

  data.CommErrors[AHRSSTATUS_COMMERRORS_UPDATE] = update_errors;
  data.CommErrors[AHRSSTATUS_COMMERRORS_ATTITUDERAW] = attituderaw_errors;
  data.CommErrors[AHRSSTATUS_COMMERRORS_HOMELOCATION] = home_errors;
  data.CommErrors[AHRSSTATUS_COMMERRORS_CALIBRATION] = calibration_errors;
  data.CommErrors[AHRSSTATUS_COMMERRORS_ALGORITHM] = algorithm_errors;

  AhrsStatusSet(&data);
}

/**
  * @}
  * @}
  */
