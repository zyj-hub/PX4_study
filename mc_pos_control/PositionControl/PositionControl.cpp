/****************************************************************************
 *
 *   Copyright (c) 2018 - 2019 PX4 Development Team. All rights reserved.
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
 * @file PositionControl.cpp
 */

#include "PositionControl.hpp"
#include "ControlMath.hpp"
#include <float.h>
#include <mathlib/mathlib.h>
#include <px4_platform_common/defines.h>
#include <geo/geo.h>

using namespace matrix;

void PositionControl::setVelocityGains(const Vector3f &P, const Vector3f &I, const Vector3f &D)
{
	_gain_vel_p = P;
	_gain_vel_i = I;
	_gain_vel_d = D;
}

void PositionControl::setVelocityLimits(const float vel_horizontal, const float vel_up, const float vel_down)
{
	_lim_vel_horizontal = vel_horizontal;
	_lim_vel_up = vel_up;
	_lim_vel_down = vel_down;
}

void PositionControl::setThrustLimits(const float min, const float max)
{
	// make sure there's always enough thrust vector length to infer the attitude
	_lim_thr_min = math::max(min, 10e-4f);
	_lim_thr_max = max;
}

void PositionControl::setHorizontalThrustMargin(const float margin)
{
	_lim_thr_xy_margin = margin;
}

void PositionControl::updateHoverThrust(const float hover_thrust_new)
{
	// Given that the equation for thrust is T = a_sp * Th / g - Th
	// with a_sp = desired acceleration, Th = hover thrust and g = gravity constant,
	// we want to find the acceleration that needs to be added to the integrator in order obtain
	// the same thrust after replacing the current hover thrust by the new one.
	// T' = T => a_sp' * Th' / g - Th' = a_sp * Th / g - Th
	// so a_sp' = (a_sp - g) * Th / Th' + g
	// we can then add a_sp' - a_sp to the current integrator to absorb the effect of changing Th by Th'
	if (hover_thrust_new > FLT_EPSILON) {
		_vel_int(2) += (_acc_sp(2) - CONSTANTS_ONE_G) * _hover_thrust / hover_thrust_new + CONSTANTS_ONE_G - _acc_sp(2);
		setHoverThrust(hover_thrust_new);
	}
}

void PositionControl::setState(const PositionControlStates &states)
{
	_pos = states.position;
	_vel = states.velocity;
	_yaw = states.yaw;
	_vel_dot = states.acceleration;
}

void PositionControl::setInputSetpoint(const vehicle_local_position_setpoint_s &setpoint)
{
	_pos_sp = Vector3f(setpoint.x, setpoint.y, setpoint.z);
	_vel_sp = Vector3f(setpoint.vx, setpoint.vy, setpoint.vz);
	_acc_sp = Vector3f(setpoint.acceleration);
	_yaw_sp = setpoint.yaw;
	_yawspeed_sp = setpoint.yawspeed;
}

bool PositionControl::update(const float dt)
{
	bool valid = _inputValid();

	if (valid) {
		_positionControl();
		_velocityControl(dt);

		_yawspeed_sp = PX4_ISFINITE(_yawspeed_sp) ? _yawspeed_sp : 0.f;
		_yaw_sp = PX4_ISFINITE(_yaw_sp) ? _yaw_sp : _yaw; // TODO: better way to disable yaw control
	}

	// There has to be a valid output accleration and thrust setpoint otherwise something went wrong
	valid = valid && PX4_ISFINITE(_acc_sp(0)) && PX4_ISFINITE(_acc_sp(1)) && PX4_ISFINITE(_acc_sp(2));
	valid = valid && PX4_ISFINITE(_thr_sp(0)) && PX4_ISFINITE(_thr_sp(1)) && PX4_ISFINITE(_thr_sp(2));

	return valid;
}

//位置P控制
void PositionControl::_positionControl()
{
	// P-position controller
	//位置外环P控制
	Vector3f vel_sp_position = (_pos_sp - _pos).emult(_gain_pos_p);
	// Position and feed-forward velocity setpoints or position states being NAN results in them not having an influence
	ControlMath::addIfNotNanVector3f(_vel_sp, vel_sp_position);
	// make sure there are no NAN elements for further reference while constraining
	//如果位置误差产生的期望速度产生的期望速度为NAN，则将其置为零
	ControlMath::setZeroIfNanVector3f(vel_sp_position);

	// Constrain horizontal velocity by prioritizing the velocity component along the
	// the desired position setpoint over the feed-forward term.
	//根据设置的水平最大速度和上升/下降最大速度对期望速度进行限幅
	_vel_sp.xy() = ControlMath::constrainXY(vel_sp_position.xy(), (_vel_sp - vel_sp_position).xy(), _lim_vel_horizontal);
	// Constrain velocity in z-direction.
	_vel_sp(2) = math::constrain(_vel_sp(2), -_lim_vel_up, _lim_vel_down);
}

void PositionControl::_velocityControl(const float dt)
{
	// PID velocity control
	//速度误差以及期望加速度计算(NED系)
	Vector3f vel_error = _vel_sp - _vel;
	Vector3f acc_sp_velocity = vel_error.emult(_gain_vel_p) + _vel_int - _vel_dot.emult(_gain_vel_d);

	// No control input from setpoints or corresponding states which are NAN
	//速度误差产生的期望加速度和期望加速度前馈叠加，作为总的期望加速度，原理同位置控制
	ControlMath::addIfNotNanVector3f(_acc_sp, acc_sp_velocity);

	//加速度控制
	_accelerationControl();

	// Integrator anti-windup in vertical direction
	/* 
	垂直方向的误差速度误差限制，由于采用的是NED坐标系，即指向地面为正，而升力是向上的，所以是负的，
	如果升力大于 -_lim_thr_min，也就是升力的绝对值小于_lim_thr_min，并且垂直速度误差是正的（指向地面），
	则将垂直速度误差置为零，即不再使升力降低．如果升力小于 -_lim_thr_max，也就是升力的绝对值大于_lim_thr_max，
	并且垂直速度误差是负的（指向空中），则将垂直速度误差置为零，即不再使升力升高．
 	*/
	if ((_thr_sp(2) >= -_lim_thr_min && vel_error(2) >= 0.0f) ||
	    (_thr_sp(2) <= -_lim_thr_max && vel_error(2) <= 0.0f)) {
		vel_error(2) = 0.f;
	}

	// Prioritize vertical control while keeping a horizontal margin
	// 优先考虑垂直控制同时保持水平
	const Vector2f thrust_sp_xy(_thr_sp); //x,y方向的推力
	const float thrust_sp_xy_norm = thrust_sp_xy.norm();
	const float thrust_max_squared = math::sq(_lim_thr_max);

	// Determine how much vertical thrust is left keeping horizontal margin
	//根据勾股定理，垂直方向的最大推力的平方等于总的最大推力的平方减去水平方向的推力的平方
	const float allocated_horizontal_thrust = math::min(thrust_sp_xy_norm, _lim_thr_xy_margin); //求解最小的水平方向的推力
	const float thrust_z_max_squared = thrust_max_squared - math::sq(allocated_horizontal_thrust); //最大垂直拉力等于总的最大推力减去最小的水平推力

	// Saturate maximal vertical thrust
	//最大升力限幅
	_thr_sp(2) = math::max(_thr_sp(2), -sqrtf(thrust_z_max_squared));

	// Determine how much horizontal thrust is left after prioritizing vertical control
	//计算用于水平控制的推力
	const float thrust_max_xy_squared = thrust_max_squared - math::sq(_thr_sp(2));
	float thrust_max_xy = 0;

	if (thrust_max_xy_squared > 0) {
		thrust_max_xy = sqrtf(thrust_max_xy_squared);
	}

	// Saturate thrust in horizontal direction
	// 水平方向推力限幅
	if (thrust_sp_xy_norm > thrust_max_xy) {
		_thr_sp.xy() = thrust_sp_xy / thrust_sp_xy_norm * thrust_max_xy;
	}

	// Use tracking Anti-Windup for horizontal direction: during saturation, the integrator is used to unsaturate the output
	// see Anti-Reset Windup for PID controllers, L.Rundqwist, 1990
	/* 积分抗饱和处理，当控制律输出没到极限时，_thr_sp和thrust_desired_NE是相等的，所以正常积分，当输出达到极限后，
		在积分的同时就会向反方向减去一个超限的值，超得越多减得越多，这可以起到抗饱和的作用，避免超限后积分还在持续增大。*/
	const Vector2f acc_sp_xy_limited = Vector2f(_thr_sp) * (CONSTANTS_ONE_G / _hover_thrust);
	const float arw_gain = 2.f / _gain_vel_p(0);
	vel_error.xy() = Vector2f(vel_error) - (arw_gain * (Vector2f(_acc_sp) - acc_sp_xy_limited));

	// Make sure integral doesn't get NAN
	ControlMath::setZeroIfNanVector3f(vel_error);
	// Update integral part of velocity control
	_vel_int += vel_error.emult(_gain_vel_i) * dt;

	// limit thrust integral
	_vel_int(2) = math::min(fabsf(_vel_int(2)), CONSTANTS_ONE_G) * sign(_vel_int(2));
}

void PositionControl::_accelerationControl()
{
	// Assume standard acceleration due to gravity in vertical direction for attitude generation
	/*假设z轴方向只受重力加速度g,以此来计算在该水平期望加速度下，负的总推力矢量（推力矢量在机体坐标系下总是和Z轴平行的），
	单位化之后，在NED坐标系下的坐标，记改矢量为bodyz */
	Vector3f body_z = Vector3f(-_acc_sp(0), -_acc_sp(1), CONSTANTS_ONE_G).normalized(); //北东地坐标系下的推力向量单位化
	ControlMath::limitTilt(body_z, Vector3f(0, 0, 1), _lim_tilt); //对倾角进行限幅，px4中并没有分别对俯仰角和横滚角进行限制，而是限制了倾斜角tilt，即限制的是机体系Z轴和NED坐标系Z轴之间的夹角
	// Scale thrust assuming hover thrust produces standard gravity
	float collective_thrust = _acc_sp(2) * (_hover_thrust / CONSTANTS_ONE_G) - _hover_thrust;
	
	// Project thrust to planned body attitude
	/*合力不仅用于悬停，还用于水平移动，因此需要将倾斜角度考虑进去,根据合力的垂直分量除以倾斜角水平分量来求得新的collective_thrust
	保证垂直上面的分量一直为，_acc_sp(2) * (_hover_thrust / CONSTANTS_ONE_G) - _hover_thrust;
	这里的body_z已经是经过上面limitTilt限幅过的了因此这里的倾角也进行了修正 */
	collective_thrust /= (Vector3f(0, 0, 1).dot(body_z));
	//最小升力限制
	collective_thrust = math::min(collective_thrust, -_lim_thr_min);
	//求合力(NED)
	_thr_sp = body_z * collective_thrust;
}

bool PositionControl::_inputValid()
{
	bool valid = true;

	// Every axis x, y, z needs to have some setpoint
	for (int i = 0; i <= 2; i++) {
		valid = valid && (PX4_ISFINITE(_pos_sp(i)) || PX4_ISFINITE(_vel_sp(i)) || PX4_ISFINITE(_acc_sp(i)));
	}

	// x and y input setpoints always have to come in pairs
	valid = valid && (PX4_ISFINITE(_pos_sp(0)) == PX4_ISFINITE(_pos_sp(1)));
	valid = valid && (PX4_ISFINITE(_vel_sp(0)) == PX4_ISFINITE(_vel_sp(1)));
	valid = valid && (PX4_ISFINITE(_acc_sp(0)) == PX4_ISFINITE(_acc_sp(1)));

	// For each controlled state the estimate has to be valid
	for (int i = 0; i <= 2; i++) {
		if (PX4_ISFINITE(_pos_sp(i))) {
			valid = valid && PX4_ISFINITE(_pos(i));
		}

		if (PX4_ISFINITE(_vel_sp(i))) {
			valid = valid && PX4_ISFINITE(_vel(i)) && PX4_ISFINITE(_vel_dot(i));
		}
	}

	return valid;
}

void PositionControl::getLocalPositionSetpoint(vehicle_local_position_setpoint_s &local_position_setpoint) const
{
	local_position_setpoint.x = _pos_sp(0);
	local_position_setpoint.y = _pos_sp(1);
	local_position_setpoint.z = _pos_sp(2);
	local_position_setpoint.yaw = _yaw_sp;
	local_position_setpoint.yawspeed = _yawspeed_sp;
	local_position_setpoint.vx = _vel_sp(0);
	local_position_setpoint.vy = _vel_sp(1);
	local_position_setpoint.vz = _vel_sp(2);
	_acc_sp.copyTo(local_position_setpoint.acceleration);
	_thr_sp.copyTo(local_position_setpoint.thrust);
}

void PositionControl::getAttitudeSetpoint(vehicle_attitude_setpoint_s &attitude_setpoint) const
{
	ControlMath::thrustToAttitude(_thr_sp, _yaw_sp, attitude_setpoint);
	attitude_setpoint.yaw_sp_move_rate = _yawspeed_sp;
}
