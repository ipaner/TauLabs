/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup CCAttitude Copter Control Attitude Estimation
 * @brief Handles communication with AHRS and updating position
 * Specifically updates the the @ref AttitudeActual "AttitudeActual" and @ref AttitudeRaw "AttitudeRaw" settings objects
 * @{
 *
 * @file       ccattitude.c
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
 * Input objects: None, takes sensor data via pios
 * Output objects: @ref AttitudeRaw @ref AttitudeActual
 *
 * This module computes an attitude estimate from the sensor data
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

#include "pios.h"
#include "ccattitude.h"
#include "attituderaw.h"
#include "attitudeactual.h"
#include "attitudedesired.h"
#include "manualcontrolcommand.h"
#include "CoordinateConversions.h"

// Private constants
#define STACK_SIZE_BYTES 740
#define TASK_PRIORITY (tskIDLE_PRIORITY+0)

#define UPDATE_RATE  2 /* ms */
#define GYRO_NEUTRAL 1665
#define GYRO_SCALE   (0.010f * 180 / M_PI)

#define PI_MOD(x) (fmod(x + M_PI, M_PI * 2) - M_PI)
// Private types

// Private variables
static xTaskHandle taskHandle;

// Private functions
static void CCAttitudeTask(void *parameters);

void updateSensors();
void updateAttitude();

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t CCAttitudeInitialize(void)
{
	// Start main task
	xTaskCreate(CCAttitudeTask, (signed char *)"CCAttitude", STACK_SIZE_BYTES/4, NULL, TASK_PRIORITY, &taskHandle);
	TaskMonitorAdd(TASKINFO_RUNNING_AHRSCOMMS, taskHandle);
	return 0;
}

/**
 * Module thread, should not return.
 */
static void CCAttitudeTask(void *parameters)
{

//	AlarmsClear(SYSTEMALARMS_ALARM_AHRSCOMMS, SYSTEMALARMS_ALARM_CRITICAL);

	// Keep flash CS pin high while talking accel
	PIOS_FLASH_DISABLE;
	
	PIOS_ADXL345_Init();

	// Main task loop
	while (1) {
		//portTickType lastSysTime;
		//PIOS_WDG_UpdateFlag(PIOS_WDG_AHRS);
		
		// TODO: register the adc callback, push the data onto a queue (safe for thread)
		// with the queue ISR version
		updateSensors();		
		updateAttitude();
		
		/* Wait for the next update interval */
		//vTaskDelayUntil(&lastSysTime, UPDATE_RATE / portTICK_RATE_MS);
		vTaskDelay(UPDATE_RATE / portTICK_RATE_MS);

	}
}

void updateSensors() 
{
	AttitudeRawData attitudeRaw;
	AttitudeRawGet(&attitudeRaw);		
	struct pios_adxl345_data accel_data;
	
	static float gyro_bias[3] = {0,0,0};
	static const float tau = 0.9999f;

	attitudeRaw.gyros[ATTITUDERAW_GYROS_X] = PIOS_ADC_PinGet(1);
	attitudeRaw.gyros[ATTITUDERAW_GYROS_Y] = PIOS_ADC_PinGet(2);
	attitudeRaw.gyros[ATTITUDERAW_GYROS_Z] = PIOS_ADC_PinGet(3);
	
	attitudeRaw.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_X] = -(attitudeRaw.gyros[ATTITUDERAW_GYROS_X] - GYRO_NEUTRAL) * GYRO_SCALE;
	attitudeRaw.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_Y] = (attitudeRaw.gyros[ATTITUDERAW_GYROS_Y] - GYRO_NEUTRAL) * GYRO_SCALE;
	attitudeRaw.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_Z] = (attitudeRaw.gyros[ATTITUDERAW_GYROS_Z] - GYRO_NEUTRAL) * GYRO_SCALE;
	
	gyro_bias[0] = tau * gyro_bias[0] + (1-tau) * attitudeRaw.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_X];
	gyro_bias[1] = tau * gyro_bias[1] + (1-tau) * attitudeRaw.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_Y];
	gyro_bias[2] = tau * gyro_bias[2] + (1-tau) * attitudeRaw.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_Z];
	
	attitudeRaw.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_X] -= gyro_bias[0];
	attitudeRaw.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_Y] -= gyro_bias[1];
	attitudeRaw.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_Z] -= gyro_bias[2];
	
	// Get the accel data
	uint8_t i = 0;
	attitudeRaw.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_X] = 0;
	attitudeRaw.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_Y] = 0;
	attitudeRaw.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_Z] = 0; 
	
	do {
		i++;
		attitudeRaw.gyrotemp[0] = PIOS_ADXL345_Read(&accel_data);
		
		attitudeRaw.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_X] += (float) accel_data.x * 0.004f * 9.81;
		attitudeRaw.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_Y] += -(float) accel_data.y * 0.004f * 9.81;
		attitudeRaw.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_Z] += -(float) accel_data.z * 0.004f * 9.81;
	} while ( (i < 32) && (attitudeRaw.gyrotemp[0] > 0) );
	
	attitudeRaw.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_X] /= i;
	attitudeRaw.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_Y] /= i;
	attitudeRaw.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_Z] /= i; 
	
	attitudeRaw.accels[ATTITUDERAW_ACCELS_X] = accel_data.x;
	attitudeRaw.accels[ATTITUDERAW_ACCELS_Y] = accel_data.y;
	attitudeRaw.accels[ATTITUDERAW_ACCELS_Z] = accel_data.z;
	
	AttitudeRawSet(&attitudeRaw); 	
}

#define UPDATE_FRAC 0.99f
void updateAttitude()
{
	AttitudeActualData attitudeActual;
	AttitudeActualGet(&attitudeActual);
	
	AttitudeRawData attitudeRaw;
	AttitudeRawGet(&attitudeRaw);		

	static portTickType lastSysTime = 0;
	static portTickType thisSysTime;
	
	float accel_pitch, accel_roll;
	static float dT = 0;
	
	thisSysTime = xTaskGetTickCount();
	if(thisSysTime > lastSysTime) // reuse dt in case of wraparound
		dT = (thisSysTime - lastSysTime) / portTICK_RATE_MS / 1000.0f;
	lastSysTime = thisSysTime;
	
	// Convert into radians
	attitudeActual.Roll = attitudeActual.Roll * M_PI / 180;
	attitudeActual.Pitch = attitudeActual.Pitch * M_PI / 180;
	attitudeActual.Yaw = attitudeActual.Yaw * M_PI / 180;
	
	// Integrate gyros
	attitudeActual.Roll  = PI_MOD(attitudeActual.Roll + attitudeRaw.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_X] * dT * M_PI / 180);
	attitudeActual.Pitch = PI_MOD(attitudeActual.Pitch + attitudeRaw.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_Y] * dT * M_PI / 180);
	attitudeActual.Yaw += attitudeRaw.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_Z] * dT * M_PI / 180;
	       
	// Compute gravity sense of ground
	accel_roll = atan2(-attitudeRaw.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_Y],
			   -attitudeRaw.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_Z]);
	accel_pitch = atan2(attitudeRaw.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_X],
			    -attitudeRaw.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_Z]);
	
	// Compute quaternion
	RPY2Quaternion(&attitudeActual.Roll, &attitudeActual.q1);
	
	// Weighted average and back into degrees
	attitudeActual.Roll = (UPDATE_FRAC * attitudeActual.Roll + (1-UPDATE_FRAC) * accel_roll) * 180 / M_PI;
	attitudeActual.Pitch = (UPDATE_FRAC * attitudeActual.Pitch + (1-UPDATE_FRAC) * accel_pitch) * 180 / M_PI;
	attitudeActual.Yaw = fmod(attitudeActual.Yaw * 180 / M_PI, 360);	
	AttitudeActualSet(&attitudeActual);
}

/**
  * @}
  * @}
  */
