/**
 * \file robotInertial.cpp
 *
 *  Created on: 26/03/2010
 *     \author: jsola@laas.fr
 *
 *
 *  ## Add a description here ##
 *
 * \ingroup rtslam
 */

#include "jmath/jblas.hpp"

//#include "rtslam/robotAbstract.hpp"
#include "rtslam/robotInertial.hpp"

#include "rtslam/quatTools.hpp"

namespace jafar {
	namespace rtslam {
		using namespace std;
		using namespace jblas;
		using namespace ublas;
		using namespace quaternion;


		/**
		 * Remote constructor from remote map.
		 * \param _map the remote map
		 */
		RobotInertial::RobotInertial(MapAbstract & _map) :
			RobotAbstract(_map, RobotInertial::size(), RobotInertial::size_control()) {
			// Build constant perturbation Jacobian
			constantPerturbation = false;
			type("Inertial");
		}


		/*
		 * This motion model is driven by IMU measurements and random perturbations, and defined by:
		 * The state vector, x = [p q v ab wb g] , of size 19.
		 * The transition equation x+ = move(x,u), with u = [am, wm, ar, wr] the control impulse, decomposed as:
		 * - p+  = p + v*dt
		 * - v+  = v + R(q)*(am - ab) + g     <-- am and wm: IMU measurements
		 * - q+  = q**((wm - wb)*dt)          <-- ** : quaternion product
		 * - ab+ = ab + ar                    <-- ar : random walk in acc bias with ar perturbation
		 * - wb+ = wb + wr                    <-- wr : random walk of gyro bias with wr perturbation
		 * - g+  = g                          <-- g  : gravity vector, constant but unknown
		 * -----------------------------------------------------------------------------
		 *
		 * The Jacobian XNEW_x is built with
		 *   var    |  p       q       v       ab      wb      g
		 *      pos |  0       3       7       10      13      16
		 *   -------+---------------------------------------------
		 *   p   0  |  I       0      I*dt     0       0       0
		 *   q   3  |  0     QNEW_q    0       0     QNEW_wb   0
		 *   v   7  |  0     VNEW_q    I     -R*dt     0      I*dt
		 *   ab  10 |  0       0       0       I       0       0
		 *   wb  13 |  0       0       0       0       I       0
		 *   g   16 |  0       0       0       0       0       I
		 * -----------------------------------------------------------------------------
		 *
		 * The Jacobian XNEW_control is built with
		 *   var    |  an    wn    ar    wr
		 *      pos |  0     3     6     9
		 *   -------+----------------------
		 *   r   0  |  0     0     0     0
		 *   q   3  |  I     0     0     0
		 *   v   7  |  0   VNEW_wn 0     0
		 *   ab  10 |  0     0     I     0
		 *   wb  13 |  0     0     0     I
		 *   g   16 |  0     0     0     0
		 * -----------------------------------------------------------------------------
		 */
		void RobotInertial::move_func(const vec & x, const vec & u, const double dt, vec & xnew, mat & _XNEW_x,
		    mat & _XNEW_control) {


			// Separate things out to make it clearer
			vec3 p, v, ab, wb, g;
			vec4 q;
			splitState(x, p, q, v, ab, wb, g); // split state vector

			// Split control vector into sensed acceleration and sensed angular rate
			vec3 am, wm, ar, wr; // measurements and random walks
			splitControl(u, am, wm, ar, wr);


			// It is useful to start obtaining a nice rotation matrix and the product R*dt
			Rold = q2R(q);
			Rdt = Rold * dt;


			// Invert sensor functions. Get true acc. and ang. rates
			// a = R(q)(asens - ab) + g     true acceleration
			// w = wsens - wb               true angular rate
			vec3 atrue, wtrue;
			atrue = prod(Rold, (am - ab)) + g;
			wtrue = wm - wb;


			// Get new state vector
			vec3 pnew, vnew, abnew, wbnew, gnew;
			vec4 qnew;

			pnew = p + v * dt; //     position
			// qnew = q x q(w * dt)
			// Keep qwt ( = q(w * dt)) for later use
			vec4 qwdt = v2q(wtrue * dt);
			qnew = qProd(q, qwdt); //    orientation
			vnew = v + atrue * dt; //    velocity
			abnew = ab + ar; //          acc bias
			wbnew = wb + wr; //          gyro bias
			gnew = g; //                 gravity does not change


			// Put it all together - this is the output state
			unsplitState(pnew, qnew, vnew, abnew, wbnew, gnew, xnew);


			// Now on to the Jacobian...
			// Identity is a good place to start since overall structure is like this
			// var    |  p       q       v       ab      wb      g
			//    pos |  0       3       7       10      13      16
			// -------+---------------------------------------------
			// p   0  |  I       0      I*dt     0       0       0
			// q   3  |  0     QNEW_q    0       0     QNEW_wb   0
			// v   7  |  0     VNEW_q    I     -R*dt     0      I*dt
			// ab  10 |  0       0       0       I       0       0
			// wb  13 |  0       0       0       0       I       0
			// g   16 |  0       0       0       0       0       I

			_XNEW_x.assign(identity_mat(state.size()));


			// Fill in XNEW_v: VNEW_g and PNEW_v = I * dt
			identity_mat I(3);
			Idt = I * dt;
			subrange(_XNEW_x, 0, 3, 7, 10) = Idt;
			subrange(_XNEW_x, 7, 10, 16, 19) = Idt;


			// Fill in QNEW_q
			// qnew = qold ** qwdt  ( qnew = q1 ** q2 = qProd(q1, q2) in rtslam/quatTools.hpp )
			qProd_by_dq1(qwdt, QNEW_q);
			subrange(_XNEW_x, 3, 7, 3, 7) = QNEW_q;


			// Fill in QNEW_wb
			// QNEW_wb = QNEW_qwdt * QWDT_wdt * WDT_w * W_wb
			//         = QNEW_qwdt * QWDT_w * W_wb
			//         = QNEW_qwdt * QWDT_w * (-1)
			qProd_by_dq2(q, QNEW_qwdt);
			// Here we get the derivative of qwdt wrt wtrue, so we consider dt = 1 and call for the derivative of v2q() with v = w*dt
			v2q_by_dv(wtrue, QWDT_w);
			QNEW_w = prod<mat> (QNEW_qwdt, QWDT_w);
			subrange(_XNEW_x, 3, 7, 13, 16) = -QNEW_w;


			// Fill VNEW_q
			// VNEW_q = d(R(q)*v) / dq
			rotate_by_dq(q, v, VNEW_q);
			subrange(_XNEW_x, 7, 10, 3, 7) = VNEW_q;


			// Fill in VNEW_ab
			subrange(_XNEW_x, 7, 10, 10, 13) = -Rdt;


			// Now on to the control Jacobian XNEW_control

			// Form of Jacobian XNEW_control
			// It is like this:
			// var    |  an    wn    ar    wr
			//    pos |  0     3     6     9
			// -------+----------------------
			// r   0  |  0     0     0     0
			// q   3  |  0   VNEW_wn 0     0
			// v   7  |  I     0     0     0
			// ab  10 |  0     0     I     0
			// wb  13 |  0     0     0     I
			// g   16 |  0     0     0     0

			// Fill in the easy bits first
			_XNEW_control.clear();
			ublas::subrange(_XNEW_control, 7, 10, 0, 3) = I;
			ublas::subrange(_XNEW_control, 10, 13, 6, 9) = I;
			ublas::subrange(_XNEW_control, 13, 16, 9, 12) = I;


			// Tricky bit is QNEW_w = d(qnew)/d(wi)
			// Here, wi is the integral of the perturbation, wi = integral_tau=0^dt (wn(t) * dtau),
			// with: wn: the angular rate measurement noise
			//       dt: the integration period
			//       wi: the resulting angular impulse
			// We have: QNEW_wi = QNEW_qwdt * QWDT_wi
			//                  = QNEW_qwdt * QWDT_wdt // Hey! wdt is the integral when w is deterministic. The jacobians *_wdt and *_wi are the same!!!
			//                  = QNEW_w * W_wdt
			//                  = QNEW_w / dt,
			//    with: QNEW_w computed before.
			// The time dependence needs to be included in control.P(), proportional to control.dt:
			//   U = control.P() = U_continuous_time * dt
			//	with: U_continuous_time expressed in ( rad / s / sqrt(s) )^2 = rad^2 / s^3 <-- yeah, it is confusing, but true.
			//   (Use control.convert_P_from_continuous() helper if necessary.)
			//
			subrange(_XNEW_control, 3, 7, 3, 6) = QNEW_w * (1 / dt);

		}

	}
}
