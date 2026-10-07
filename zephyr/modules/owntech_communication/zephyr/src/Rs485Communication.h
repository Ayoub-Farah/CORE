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
 * @author Ayoub Farah Hassan <ayoub.farah-hassan@laas.fr>
 */

#ifndef RS485COMMUNICATION_H_
#define RS485COMMUNICATION_H_

#include <stdint.h>

#ifdef CONFIG_OWNTECH_COMMUNICATION_ENABLE_RS485


/**
 * @brief Defines the possible speeds for the RS485: 
 *        
 * - `SPEED_2M`: 2Mbits/s speed communication
 *        
 * - `SPEED_5M`: 5Mbits/s speed communication
 * 
 * - `SPEED_10M`: 10Mbits/s speed communication
 * 
 * - `SPEED_20M`: 20Mbits/s speed communication
 *
 */
typedef enum {
    SPEED_2M,  
    SPEED_5M,  
    SPEED_10M, 
    SPEED_20M, 
}rs485_speed_t;

/** Errors detected by the latest synchronous dispatch. RX errors discard
 * that dispatch's bytes and reset the RX ring; discard any partial frame.
 * TX errors cancel the transmission; a retry must be explicitly prepared.
 */
enum rs485_error_t : uint32_t {
    RS485_ERROR_NONE = 0,
    RS485_ERROR_RX_OVERRUN = 1U << 0,
    RS485_ERROR_RX_FRAMING = 1U << 1,
    RS485_ERROR_RX_NOISE = 1U << 2,
    RS485_ERROR_RX_PARITY = 1U << 3,
    RS485_ERROR_RX_DMA = 1U << 4,
    RS485_ERROR_TX_DMA = 1U << 5,
    RS485_ERROR_TRIGGER_DMA = 1U << 6,
};

/**
 * Static class definition
 */

class Rs485Communication
{
    public :
        /**
         * @brief Configuration for RS485 communication using a 10Mbit/s speed
         *
         * @param transmission_buffer Pointer to the transmitted buffer
         * @param reception_buffer Pointer to the received buffer
         * @param data_size Size of the sent and received data (in byte)
         * @param user_function Callback function called when we received data
         * @param data_speed Transmission speed (by default to 10Mbits/s)
         *                  `SPEED_2M`,`SPEED_5M`,`SPEED_10M`,`SPEED_20M`
         *
         * @note Software-triggered mode; active or armed TX prevents reconfiguration.
         * @warning The size of transmission_buffer and reception_buffer
         *          must be the same
         */
        void configure(uint8_t *transmission_buffer,
                       uint8_t *reception_buffer,
                       uint16_t data_size, void (*user_function)(),
                       rs485_speed_t data_speed = SPEED_10M);

        /**
         * @brief Custom configuration for RS485 communication
         *        to choose the communication speed.
         *
         * @param transmission_buffer Pointer to the transmitted buffer
         * @param reception_buffer Pointer to the received buffer
         * @param data_size Size of the sent and received data (in byte)
         * @param user_function Callback function called when we received data
         * @param baudrate Communication speed in bit/s
         * @param oversampling_8 True for oversampling
         *                       (and multiply communication speed by 2),
         *                       False if you want to keep
         *                       the normal speed communication
         *
         * @note Software-triggered mode; active or armed TX prevents reconfiguration.
         * @warning The size of transmission_buffer
         *          and reception_buffer must be the same
         */
        void configureCustom(uint8_t *transmission_buffer,
                             uint8_t *reception_buffer,
                             uint16_t data_size, void (*user_function)(void),
                             uint32_t baudrate, bool oversampling_8);

        /** @brief Configure interrupt-free synchronous RS485.
         * Configure before starting the critical task; stop it before
         * reconfiguring buffers or switching modes.
         * Initialize HRTIM synchronization first; in master mode create the
         * critical task first. TX is armed separately for each message.
         * rx_ring and rx_snapshot must each hold reception_size bytes and
         * must not overlap each other or TX. Keep all buffers alive.
         * Dispatch copies new bytes before each critical task; it does not
         * parse frames. Fewer than reception_size bytes must arrive between
         * dispatches, including copy time, to avoid undetectable ring wraps.
         * Returns 0 or a negative errno. A failed setup is not ready for TX.
         */
        int configureSynchronous(uint8_t *transmission_buffer,
                                 uint16_t transmission_size,
                                 uint8_t *rx_ring, uint8_t *rx_snapshot,
                                 uint16_t reception_size, uint32_t delay_us,
                                 uint32_t baudrate = 10625000,
                                 bool oversampling_8 = false);

        /** @brief Bytes copied by the latest critical-task dispatch.
         * Read from the configured rx_snapshot inside the critical task.
         * Includes partial frames; zero means no new bytes. Snapshot remains
         * valid until the next dispatch. RX reception uses no interrupts.
         * RX errors discard the snapshot; inspect dispatchErrors().
         */
        uint16_t receivedSize();

        /** @brief Error bitmask from the latest synchronous dispatch.
         * Reading does not clear it; the next dispatch replaces it.
         */
        uint32_t dispatchErrors();

        /** @brief True until USART has finished sending the active message. */
        bool transmissionBusy();

        /**
         * @brief Start a transmission i.e. you send
         *        what is contained in the transmission buffer
         *        in software-triggered mode. Ignored in synchronous mode
         *        or while another TX is active/armed.
         */
        void startTransmission();

        /** @brief Configure TX delay after a sync/control boundary.
         * Call configure() and initSlave() or initMaster() first.
         * For master mode, create the HRTIM critical task before this call.
         * Its delay must be less than the control period minus 3 us.
         * Uses TIM2 CH1 and DMA2 channel 1. Returns 0 or a negative errno.
         */
        int configureSynchronizedTransmission(uint32_t delay_us);

        /** @brief Arm one message for the next TIM2 CH1 compare.
         * Returns -EBUSY while a trigger/TX is pending. The configured buffer
         * must remain unchanged until TX completes. Rearm for each message.
         * The TIM2 event starts DMA without a CPU interrupt. Synchronous
         * configuration polls TX completion; software-triggered configuration
         * retains the DMA completion interrupt.
         */
        int prepareSynchronizedTransmission();

        /** @brief Cancel a pending TX trigger without aborting an active TX.
         * In master mode, SCOUT timing continues independently of TX arming.
         */
        void stopSynchronizedTransmission();

        /**
         * @brief Turn on the RS485 communication
         *
         * @warning The RS485 is automatically turned on
         *          when initializing with configureDefault or configure,
         *          no need to call this function
         */
        void turnOnCommunication();

        /**
         * @brief Turn off the RS485
         */
        void turnOffCommunication();
};

#endif /* CONFIG_OWNTECH_COMMUNICATION_ENABLE_RS485 */

#endif /* RS485COMMUNICATION_H_ */
