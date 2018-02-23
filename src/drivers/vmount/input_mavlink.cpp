/****************************************************************************
*
*   Copyright (c) 2016-2017 PX4 Development Team. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
*
* 1. Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright
*    notice, this list of conditions and the following disclaimer in
*    the documentation and/or other materials provided with the
*    distribution.
* 3. Neither the name PX4 nor the names of its contributors may be
*    used to endorse or promote products derived from this software
*    without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
* OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
* AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
* LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
* ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
* POSSIBILITY OF SUCH DAMAGE.
*
****************************************************************************/

/**
 * @file input_mavlink.cpp
 * @author Leon Müller (thedevleon)
 * @author Beat Küng <beat-kueng@gmx.net>
 *
 */

#include "input_mavlink.h"
#include <uORB/uORB.h>
#include <uORB/topics/vehicle_roi.h>
#include <uORB/topics/vehicle_command_ack.h>
#include <uORB/topics/position_setpoint_triplet.h>
#include <drivers/drv_hrt.h>

#include <px4_defines.h>
#include <px4_posix.h>
#include <errno.h>

namespace vmount
{

InputMavlinkROI::~InputMavlinkROI()
{
	if (_vehicle_roi_sub >= 0) {
		orb_unsubscribe(_vehicle_roi_sub);
	}

	if (_position_setpoint_triplet_sub >= 0) {
		orb_unsubscribe(_position_setpoint_triplet_sub);
	}
}

int InputMavlinkROI::initialize()
{
	_vehicle_roi_sub = orb_subscribe(ORB_ID(vehicle_roi));

	if (_vehicle_roi_sub < 0) {
		return -errno;
	}

	_position_setpoint_triplet_sub = orb_subscribe(ORB_ID(position_setpoint_triplet));

	if (_position_setpoint_triplet_sub < 0) {
		return -errno;
	}

	return 0;
}

int InputMavlinkROI::update_impl(unsigned int timeout_ms, ControlData **control_data, bool already_active)
{
	// already_active is unused, we don't care what happened previously.

	// Default to no change, set if we receive anything.
	*control_data = nullptr;

	const int num_poll = 2;
	px4_pollfd_struct_t polls[num_poll];
	polls[0].fd = 		_vehicle_roi_sub;
	polls[0].events = 	POLLIN;
	polls[1].fd = 		_position_setpoint_triplet_sub;
	polls[1].events = 	POLLIN;

	int ret = px4_poll(polls, num_poll, timeout_ms);

	if (ret < 0) {
		return -errno;
	}

	if (ret == 0) {
		// Timeout, _control_data is already null

	} else {
		if (polls[0].revents & POLLIN) {
			vehicle_roi_s vehicle_roi;
			orb_copy(ORB_ID(vehicle_roi), _vehicle_roi_sub, &vehicle_roi);

			_control_data.gimbal_shutter_retract = false;

			if (vehicle_roi.mode == vehicle_roi_s::ROI_NONE) {

				_control_data.type = ControlData::Type::Neutral;
				*control_data = &_control_data;

			} else if (vehicle_roi.mode == vehicle_roi_s::ROI_WPNEXT) {
				_read_control_data_from_position_setpoint_sub();
				_control_data.type_data.lonlat.roll_angle = 0.f;
				_control_data.type_data.lonlat.pitch_fixed_angle = -10.f;

				*control_data = &_control_data;

			} else if (vehicle_roi.mode == vehicle_roi_s::ROI_WPINDEX) {
				//TODO how to do this?

			} else if (vehicle_roi.mode == vehicle_roi_s::ROI_LOCATION) {
				control_data_set_lon_lat(vehicle_roi.lon, vehicle_roi.lat, vehicle_roi.alt);

				*control_data = &_control_data;

			} else if (vehicle_roi.mode == vehicle_roi_s::ROI_TARGET) {
				//TODO is this even suported?
			}

			_cur_roi_mode = vehicle_roi.mode;

			//set all other control data fields to defaults
			for (int i = 0; i < 3; ++i) {
				_control_data.stabilize_axis[i] = false;
			}
		}

		// check whether the position setpoint got updated
		if (polls[1].revents & POLLIN) {
			if (_cur_roi_mode == vehicle_roi_s::ROI_WPNEXT) {
				_read_control_data_from_position_setpoint_sub();
				*control_data = &_control_data;

			} else { // must do an orb_copy() in *every* case
				position_setpoint_triplet_s position_setpoint_triplet;
				orb_copy(ORB_ID(position_setpoint_triplet), _position_setpoint_triplet_sub, &position_setpoint_triplet);
			}
		}
	}

	return 0;
}

void InputMavlinkROI::_read_control_data_from_position_setpoint_sub()
{
	position_setpoint_triplet_s position_setpoint_triplet;
	orb_copy(ORB_ID(position_setpoint_triplet), _position_setpoint_triplet_sub, &position_setpoint_triplet);
	_control_data.type_data.lonlat.lon = position_setpoint_triplet.next.lon;
	_control_data.type_data.lonlat.lat = position_setpoint_triplet.next.lat;
	_control_data.type_data.lonlat.altitude = position_setpoint_triplet.next.alt;
}

void InputMavlinkROI::print_status()
{
	PX4_INFO("Input: Mavlink (ROI)");
}

InputMavlinkCmdMount::InputMavlinkCmdMount(bool stabilize)
	: _stabilize {stabilize, stabilize, stabilize}
{
	param_t handle = param_find("MAV_SYS_ID");

	if (handle != PARAM_INVALID) {
		param_get(handle, &_mav_sys_id);
	}

	handle = param_find("MAV_COMP_ID");

	if (handle != PARAM_INVALID) {
		param_get(handle, &_mav_comp_id);
	}
}

InputMavlinkCmdMount::~InputMavlinkCmdMount()
{
	if (_vehicle_command_sub >= 0) {
		orb_unsubscribe(_vehicle_command_sub);
	}
}

int InputMavlinkCmdMount::initialize()
{
	if ((_vehicle_command_sub = orb_subscribe(ORB_ID(vehicle_command))) < 0) {
		return -errno;
	}

	return 0;
}


int InputMavlinkCmdMount::update_impl(unsigned int timeout_ms, ControlData **control_data, bool already_active)
{
	// Default to notify that there was no change.
	*control_data = nullptr;

	const int num_poll = 1;
	px4_pollfd_struct_t polls[num_poll];
	polls[0].fd = 		_vehicle_command_sub;
	polls[0].events = 	POLLIN;

	int poll_timeout = (int)timeout_ms;

	bool exit_loop = false;

	while (!exit_loop && poll_timeout >= 0) {
		hrt_abstime poll_start = hrt_absolute_time();

		int ret = px4_poll(polls, num_poll, poll_timeout);

		if (ret < 0) {
			return -errno;
		}

		poll_timeout -= (hrt_absolute_time() - poll_start) / 1000;

		// if we get a command that we need to handle, we exit the loop, otherwise we poll until we reach the timeout
		exit_loop = true;

		if (ret == 0) {
			// Timeout control_data already null.

		} else {
			if (polls[0].revents & POLLIN) {
				vehicle_command_s vehicle_command;
				orb_copy(ORB_ID(vehicle_command), _vehicle_command_sub, &vehicle_command);

				//PX4_WARN("cmd: %d", vehicle_command.command);
				//PX4_WARN("param1: %d", (int)vehicle_command.param1);
				//PX4_WARN("param7: %d", (int)vehicle_command.param7);

				// Process only if the command is for us or for anyone (component id 0).
				const bool sysid_correct = (vehicle_command.target_system == _mav_sys_id);
				const bool compid_correct = ((vehicle_command.target_component == _mav_comp_id) ||
							     (vehicle_command.target_component == 0));

				if (!sysid_correct || !compid_correct) {
					PX4_WARN("sys id or compid incorrect");
					exit_loop = false;
					continue;
				}

				if (vehicle_command.command == vehicle_command_s::VEHICLE_CMD_DO_MOUNT_CONTROL) {

					switch ((int)vehicle_command.param7) {
					case vehicle_command_s::VEHICLE_MOUNT_MODE_RETRACT:
						PX4_WARN("MOUNT_CONTROL: retract");
						_control_data.gimbal_shutter_retract = true;

						*control_data = &_control_data;
						break;

					case vehicle_command_s::VEHICLE_MOUNT_MODE_NEUTRAL:
						PX4_WARN("MOUNT_CONTROL: deploy (neutral)");
						_control_data.type = ControlData::Type::Neutral;
						_control_data.gimbal_shutter_retract = false;

					case vehicle_command_s::VEHICLE_MOUNT_MODE_MAVLINK_TARGETING: {
							_control_data.type = ControlData::Type::Angle;

							// mavlink spec MAV_CMD_DO_MOUNT_CONTROL
							// param1: pitch (tilt)
							// param2: roll
							// param3: yaw (pan)

							// We expect angle of [-pi..+pi]. If the input range is [0..2pi] we can fix that.
							const float roll = vehicle_command.param2 * M_DEG_TO_RAD_F;
							const float pitch = vehicle_command.param1 * M_DEG_TO_RAD_F;
							const float yaw = vehicle_command.param3 * M_DEG_TO_RAD_F;

							if (PX4_ISFINITE(pitch) && PX4_ISFINITE(yaw)) {
								_control_data.type_data.angle.angles[0] = roll;
								_control_data.type_data.angle.angles[1] = pitch;
								_control_data.type_data.angle.angles[2] = yaw;
							}

							*control_data = &_control_data;
						}
						break;

					} else if (vehicle_command.command == vehicle_command_s::VEHICLE_CMD_DO_MOUNT_CONFIGURE) {


						switch ((int)vehicle_command.param1) {
						case vehicle_command_s::VEHICLE_MOUNT_MODE_RETRACT:
							PX4_WARN("MOUNT_CONFIGURE: retract");
							_control_data.gimbal_shutter_retract = true;

							*control_data = &_control_data;
							break;

						case vehicle_command_s::VEHICLE_MOUNT_MODE_NEUTRAL:
							PX4_WARN("MOUNT_CONFIGURE: deploy");
							_control_data.type = ControlData::Type::Neutral;
							_control_data.gimbal_shutter_retract = false;

							*control_data = &_control_data;
							break;
						}

						_stabilize[0] = (uint8_t) vehicle_command.param2 == 1;
						_stabilize[1] = (uint8_t) vehicle_command.param3 == 1;
						_stabilize[2] = (uint8_t) vehicle_command.param4 == 1;

						_control_data.type_data.angle.is_speed[0] = (uint8_t) vehicle_command.param5 == 1;
						_control_data.type_data.angle.is_speed[1] = (uint8_t) vehicle_command.param6 == 1;
						_control_data.type_data.angle.is_speed[2] = (uint8_t) vehicle_command.param7 == 1;

						*control_data = &_control_data;
						_ack_vehicle_command(&vehicle_command);

					} else if (vehicle_command.command == vehicle_command_s::VEHICLE_CMD_DO_DIGICAM_CONTROL) {
						// find a better home for this

						float zoom = (int)vehicle_command.param2;

						_control_data.zoom = zoom;

						_ack_vehicle_command(&vehicle_command);

					} else {
						exit_loop = false;
					}
				}

			}

			return 0;
		}

		void InputMavlinkCmdMount::_ack_vehicle_command(vehicle_command_s * cmd) {
			vehicle_command_ack_s vehicle_command_ack = {
				.timestamp = hrt_absolute_time(),
				.result_param2 = 0,
				.command = cmd->command,
				.result = vehicle_command_s::VEHICLE_CMD_RESULT_ACCEPTED,
				.from_external = false,
				.result_param1 = 0,
				.target_system = cmd->source_system,
				.target_component = cmd->source_component
			};

			if (_vehicle_command_ack_pub == nullptr) {
				_vehicle_command_ack_pub = orb_advertise_queue(ORB_ID(vehicle_command_ack), &vehicle_command_ack,
							   vehicle_command_ack_s::ORB_QUEUE_LENGTH);

			} else {
				orb_publish(ORB_ID(vehicle_command_ack), _vehicle_command_ack_pub, &vehicle_command_ack);
			}

		}

		void InputMavlinkCmdMount::print_status() {
			PX4_INFO("Input: Mavlink (CMD_MOUNT)");
		}


	} /* namespace vmount */
