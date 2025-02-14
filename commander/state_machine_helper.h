/****************************************************************************
 *
 *   Copyright (c) 2013, 2014 PX4 Development Team. All rights reserved.
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
 * @file state_machine_helper.h
 * State machine helper functions definitions
 *
 * @author Thomas Gubler <thomasgubler@student.ethz.ch>
 * @author Julian Oes <julian@oes.ch>
 */

#ifndef STATE_MACHINE_HELPER_H_
#define STATE_MACHINE_HELPER_H_

#include <drivers/drv_hrt.h>

#include "Arming/PreFlightCheck/PreFlightCheck.hpp"

#include <uORB/uORB.h>
#include <uORB/topics/vehicle_status.h>
#include <uORB/topics/battery_status.h>
#include <uORB/topics/actuator_armed.h>
#include <uORB/topics/safety.h>
#include <uORB/topics/commander_state.h>
#include <uORB/topics/vehicle_status_flags.h>
#include <px4_platform_common/events.h>

//枚举型，枚举变量transition_result_t只能取花括号内的值。
typedef enum {
	TRANSITION_DENIED = -1,
	TRANSITION_NOT_CHANGED = 0,
	TRANSITION_CHANGED
} transition_result_t;

enum class link_loss_actions_t {
	DISABLED = 0,
	AUTO_LOITER = 1,	// Hold mode
	AUTO_RTL = 2,		// Return mode
	AUTO_LAND = 3,		// Land mode
	TERMINATE = 5,		// Terminate flight (set actuator outputs to failsafe values, and stop controllers)
	LOCKDOWN = 6,		// Lock actuators (set actuator outputs to disarmed values)
};

enum class quadchute_actions_t {
	NO_ACTION = -1,
	AUTO_RTL = 0,		// Return mode
	AUTO_LAND = 1,		// Land mode
	AUTO_LOITER = 2,	// Hold mode
};

enum class offboard_loss_actions_t {
	DISABLED = -1,
	AUTO_LAND = 0,		// Land mode
	AUTO_LOITER = 1,	// Hold mode
	AUTO_RTL = 2,		// Return mode
	TERMINATE = 3,		// Terminate flight (set actuator outputs to failsafe values, and stop controllers)
	LOCKDOWN = 4,		// Lock actuators (set actuator outputs to disarmed values)
};

enum class offboard_loss_rc_actions_t {
	DISABLED = -1, 		// Disabled
	MANUAL_POSITION = 0, 	// Position mode
	MANUAL_ALTITUDE = 1, 	// Altitude mode
	MANUAL_ATTITUDE = 2, 	// Manual
	AUTO_RTL = 3, 		// Return mode
	AUTO_LAND = 4, 		// Land mode
	AUTO_LOITER = 5, 	// Hold mode
	TERMINATE = 6, 		// Terminate flight (set actuator outputs to failsafe values, and stop controllers)
	LOCKDOWN = 7, 		// Lock actuators (set actuator outputs to disarmed values)
};

enum class position_nav_loss_actions_t {
	ALTITUDE_MANUAL = 0,	// Altitude/Manual. Assume use of remote control after fallback. Switch to Altitude mode if a height estimate is available, else switch to MANUAL.
	LAND_TERMINATE = 1,	// Land/Terminate.  Assume no use of remote control after fallback. Switch to Land mode if a height estimate is available, else switch to TERMINATION.
};

extern const char *const arming_state_names[];
extern const char *const nav_state_names[];

using arm_disarm_reason_t = events::px4::enums::arm_disarm_reason_t;

enum RCLossExceptionBits {
	RCL_EXCEPT_MISSION = (1 << 0),
	RCL_EXCEPT_HOLD = (1 << 1),
	RCL_EXCEPT_OFFBOARD = (1 << 2)
};

transition_result_t
arming_state_transition(vehicle_status_s &status, const vehicle_control_mode_s &control_mode, const safety_s &safety,
			const arming_state_t new_arming_state,
			actuator_armed_s &armed, const bool fRunPreArmChecks, orb_advert_t *mavlink_log_pub,
			vehicle_status_flags_s &status_flags, const PreFlightCheck::arm_requirements_t &arm_requirements,
			const hrt_abstime &time_since_boot, arm_disarm_reason_t calling_reason);

transition_result_t
main_state_transition(const vehicle_status_s &status, const main_state_t new_main_state,
		      const vehicle_status_flags_s &status_flags, commander_state_s &internal_state);

bool set_nav_state(vehicle_status_s &status, actuator_armed_s &armed, commander_state_s &internal_state,
		   orb_advert_t *mavlink_log_pub, const link_loss_actions_t data_link_loss_act, const bool mission_finished,
		   const bool stay_in_failsafe, const vehicle_status_flags_s &status_flags, bool landed,
		   const link_loss_actions_t rc_loss_act, const offboard_loss_actions_t offb_loss_act,
		   const quadchute_actions_t quadchute_act,
		   const offboard_loss_rc_actions_t offb_loss_rc_act,
		   const position_nav_loss_actions_t posctl_nav_loss_act,
		   const float param_com_rcl_act_t, const int param_com_rcl_except);

/*
 * Checks the validty of position data against the requirements of the current navigation
 * mode and switches mode if position data required is not available.
 */
bool check_invalid_pos_nav_state(vehicle_status_s &status, bool old_failsafe, orb_advert_t *mavlink_log_pub,
				 const vehicle_status_flags_s &status_flags, const bool use_rc, const bool using_global_pos);


// COM_LOW_BAT_ACT parameter values
typedef enum LOW_BAT_ACTION {
	WARNING = 0,		// Warning
	RETURN = 1,			// Return mode
	LAND = 2,			// Land mode
	RETURN_OR_LAND = 3	// Return mode at critically low level, Land mode at current position if reaching dangerously low levels
} low_battery_action_t;

void warn_user_about_battery(orb_advert_t *mavlink_log_pub, const uint8_t battery_warning,
			     const uint8_t failsafe_action, const float param_com_bat_act_t,
			     const char *failsafe_action_string, const events::px4::enums::navigation_mode_t failsafe_action_navigation_mode);
uint8_t get_battery_failsafe_action(const commander_state_s &internal_state, const uint8_t battery_warning,
				    const low_battery_action_t param_com_low_bat_act);

// COM_IMB_PROP_ACT parameter values
enum class imbalanced_propeller_action_t {
	DISABLED = -1,
	WARNING = 0,
	RETURN = 1,
	LAND = 2
};

void imbalanced_prop_failsafe(orb_advert_t *mavlink_log_pub, const vehicle_status_s &status,
			      const vehicle_status_flags_s &status_flags, commander_state_s *internal_state,
			      const imbalanced_propeller_action_t failsafe_action);

#endif /* STATE_MACHINE_HELPER_H_ */
