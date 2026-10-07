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

#include "Rs485.h"
#include "Rs485Communication.h"
#include "communication_dispatch.h"
#include <errno.h>

void Rs485Communication::configure(uint8_t *transmission_buffer,
                                   uint8_t *reception_buffer,
                                   uint16_t data_size, void (*user_function)(),
                                   rs485_speed_t data_speed)
{
    if (serial_tx_busy())
    {
        printk("Unable to configure RS485 while a TX is active or armed\n");
        return;
    }
    serial_stop();
    init_usrBuffer(transmission_buffer, reception_buffer);
    init_usrFunc(user_function);
    init_usrDataSize(data_size);

    switch(data_speed)
    {
        case SPEED_2M:
            init_usrBaudrate(2656250);
            break;
        case SPEED_5M:
            init_usrBaudrate(5312500);
            break;
        case SPEED_10M:
            init_usrBaudrate(10625000);
            break;
        default:
            init_usrBaudrate(10625000);
            break;
    }
    init_usrBaudrate(10625000);

    dma_channel_init_tx();
    dma_channel_init_rx();
    serial_init();
    init_DEmode();

    if(data_speed == SPEED_20M) oversamp_set(OVER8);
    else oversamp_set(OVER16);
}

void Rs485Communication::configureCustom(uint8_t* transmission_buffer,
                                         uint8_t* reception_buffer,
                                         uint16_t data_size,
                                         void (*user_function)(void),
                                         uint32_t baudrate,
                                         bool oversampling_8)
{
    if (serial_tx_busy())
    {
        printk("Unable to configure RS485 while a TX is active or armed\n");
        return;
    }
    serial_stop();
    init_usrBuffer(transmission_buffer, reception_buffer);
    init_usrFunc(user_function);
    init_usrDataSize(data_size);
    init_usrBaudrate(baudrate);
    dma_channel_init_tx();
    dma_channel_init_rx();
    serial_init();
    init_DEmode();
    if(oversampling_8 == true) oversamp_set(OVER8);
    else oversamp_set(OVER16);
}

void Rs485Communication::startTransmission()
{
    serial_tx_on();
}

int Rs485Communication::configureSynchronizedTransmission(uint32_t delay_us)
{
    return serial_sync_tx_config(delay_us);
}

int Rs485Communication::prepareSynchronizedTransmission()
{
    return serial_sync_tx_prepare();
}

void Rs485Communication::stopSynchronizedTransmission()
{
    serial_sync_tx_stop();
}

void Rs485Communication::turnOnCommunication()
{
    serial_start();
}

void Rs485Communication::turnOffCommunication()
{
    serial_stop();
}

int Rs485Communication::configureSynchronous(uint8_t *transmission_buffer,
                                            uint16_t transmission_size,
                                            uint8_t *rx_ring,
                                            uint8_t *rx_snapshot,
                                            uint16_t reception_size,
                                            uint32_t delay_us,
                                            uint32_t baudrate,
                                            bool oversampling_8)
{
    if (transmission_buffer == nullptr || rx_ring == nullptr ||
        rx_snapshot == nullptr || transmission_size == 0 ||
        reception_size < 2 || baudrate == 0 || delay_us == 0)
        return -EINVAL;
    const uintptr_t tx = reinterpret_cast<uintptr_t>(transmission_buffer);
    const uintptr_t ring = reinterpret_cast<uintptr_t>(rx_ring);
    const uintptr_t snapshot = reinterpret_cast<uintptr_t>(rx_snapshot);
    if ((tx < ring + reception_size && ring < tx + transmission_size) ||
        (tx < snapshot + reception_size && snapshot < tx + transmission_size) ||
        (ring < snapshot + reception_size && snapshot < ring + reception_size))
        return -EINVAL;
    if (serial_tx_busy())
        return -EBUSY;

    serial_stop();
    init_usrBuffer(transmission_buffer, rx_ring);
    init_usrFunc(nullptr);
    init_usrDataSize(transmission_size);
    init_usrBaudrate(baudrate);
    dma_channel_init_tx();
    serial_synchronous_mode(reception_size);
    dma_channel_init_rx();
    serial_init();
    init_DEmode();
    oversamp_set(oversampling_8 ? OVER8 : OVER16);
    int result = serial_sync_tx_config(delay_us);
    if (result != 0)
        return result;
    communication_dispatch_configure(rx_ring, rx_snapshot, reception_size);
    return 0;
}

uint16_t Rs485Communication::receivedSize()
{
    return communication_dispatch_received_size();
}

bool Rs485Communication::transmissionBusy()
{
    return serial_tx_busy();
}

uint32_t Rs485Communication::dispatchErrors()
{
    return communication_dispatch_errors();
}
