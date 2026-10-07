/*
 * Copyright (c) 2024-present LAAS-CNRS
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published by
 *   the Free Software Foundation, either version 2.1 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1
 */

/*
 * @date   2024
 *
 * @author Luiz Villa <luiz.villa@laas.fr>
 * @author Ayoub Farah Hassan <ayoub.farah-hassan@laas.fr>
 */

#ifndef SYNCCOMMUNICATION_H_
#define SYNCCOMMUNICATION_H_

#ifdef CONFIG_OWNTECH_COMMUNICATION_ENABLE_SYNC

#include <stdint.h>

/* OWNTECH API */
#include "SpinAPI.h"


/**
 *  Static class definition
 */

class SyncCommunication
{

public:

	/**
	 * @brief Initialization synchronization as `MASTER`,
	 * 		  the master send the synchronization pulse.
	 * Initialize PWM Timer A first: it must reset on the master period event.
	 * Then create/start an HRTIM critical task. TIM2 CH2 opens SCOUT and ITR10
	 * 3 us before each boundary; the repetition ISR closes them. The first ISR
	 * arms hardware phase acquisition with SCOUT closed and TX deferred; the
	 * next ISR ends acquisition. CH2 uses a zero-latency ISR and can preempt
	 * the control task. It must execute within its 3 us lead; other zero-latency
	 * ISRs can still delay it. A missed trigger reacquires phase.
	 */
	static void initMaster();

	/**
	 * @brief Initialization synchronization as `SLAVE`,
	 * 		  the slave receive the synchronization pulse.
	 * Initialize PWM Timer A first at zero phase, in continuous mode, with
	 * MASTER_PER as its only reset trigger. The HRTIM master waits for SCIN
	 * and resets on each pulse. Timer A relays each local master rollover
	 * through SYNCOUT to ITR10, with PB1 held low. Then create/start an HRTIM
	 * critical task: TIM2 keeps counting, opens ITR10 from a ZLI 3 us before
	 * each control boundary, and closes it from the accepted trigger ZLI.
	 * The first local rollover acquires phase. Other ZLIs may delay the gate;
	 * a missed rollover is acquired on the next available PWM event.
	 */
	static void initSlave();
};

#endif /* CONFIG_OWNTECH_COMMUNICATION_ENABLE_SYNC */

#endif /* SYNCCOMMUNICATION_H_ */
