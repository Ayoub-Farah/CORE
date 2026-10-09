/*
 * Copyright (c) 2023-present LAAS-CNRS
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
 * @date   2023
 *
 * @author Ayoub Farah Hassan <ayoub.farah-hassan@laas.fr>
 */

/* LL drivers */
#include <stm32_ll_dma.h>
#include <stm32_ll_gpio.h>
#include <stm32_ll_bus.h>
#include <stm32_ll_tim.h>
#include <stm32_ll_hrtim.h>
#include <errno.h>
#include "timer.h"
#include "communication_dispatch.h"

/* Zephyr drivers */
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/dma.h>

/* Header */
#include "Rs485.h"

#define DMA_USART DMA1 /* DMA used */

/**
 *  HAL override is used because of undesired effect in zephyr's dma handler
 *  dma_stm32_irq_handler. We noticed that we called the callback only once,
 *  setting stream->busy to false then never called the callback again.
 *  This problem was only noticed in zephyr 3.3 and not in zephyr 2.7
 *  that platformIO is using. For the sake of safety, we'll use HAL override.
 */
#define STM32_DMA_HAL_OVERRIDE 0x7F

/**
 * Warning : if you change the channels number,
 * you'll have to change some code line manually
 */

/* Transmission DMA channel for zephyr driver */
#define ZEPHYR_DMA_CHANNEL_TX 6
/* Reception DMA channel for zephyr driver */
#define ZEPHYR_DMA_CHANNEL_RX 7

/**
 * Warning : if you change the channels,
 * you'll have to change some code line manually
 */

/* Transmission DMA channel for LL driver */
#define LL_DMA_CHANNEL_TX LL_DMA_CHANNEL_6
/* Reception DMA channel for LL driver */
#define LL_DMA_CHANNEL_RX LL_DMA_CHANNEL_7

/* DT definitions */
static const struct device *dma1 = DEVICE_DT_GET(DT_NODELABEL(dma1));
static const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(usart3));

/* USART initialization parameters */

/* Initial baudrate to  10.625Mhz */
static uint32_t baud = 21250000 / (2);
struct uart_config uart_cfg;
struct uart_event evt;

/* DMA buffer for transmission and reception */
static uint8_t* tx_usart_val;
static uint8_t* rx_usart_val;

static uint16_t dma_buffer_size;
static uint16_t rx_buffer_size;
static bool synchronous_mode = false;

/* TIM2 CH1 performs one word write to enable the USART-paced TX channel.
 * DMA1 channels 1..5 are used by ADC acquisition, 6..7 by USART3.
 * DMA1 channel 8 is reserved for this trigger and has no enabled IRQs.
 */
static uint32_t sync_tx_enable;
static bool tx_configured = false;
static bool sync_tx_configured = false;
static const struct device *timer2 = DEVICE_DT_GET(TIMER2_DEVICE);

/* User function to call in RX callback */
static dma_callbackRXfunc_t user_fnc = NULL;

/* Private functions */

/**
 *  DMA callback TX clear transmission flag, and disabled DMA channel TX.
 */
static void _dma_callback_tx(const struct device *dev,
                             void *user_data,
                             uint32_t channel,
                             int status)
{
    /* Disable DMA channel after sending datas */
    LL_DMA_DisableChannel(DMA_USART, LL_DMA_CHANNEL_TX);

    LL_USART_ClearFlag_TXFE(USART3);
    /* Preserve USART TC: a delayed DMA callback may run after the last
     * stop bit. Writing the next byte to TDR clears TC automatically.
     */
    /* Clear transmission complete dma channel TX */
    LL_DMA_ClearFlag_TC6(DMA_USART);
}

/**
 *  DMA callback RX clear reception flag, then call user functions
 */
static void _dma_callback_rx()
{
    /* Clear transmission complete flag */
    LL_DMA_ClearFlag_TC7(DMA_USART);

    if(user_fnc != NULL){
        user_fnc();
    }
}

/* Public functions */


/**
 *  Initialize RX and TX buffer by user
 */
void init_usrBuffer(uint8_t* tx_buffer, uint8_t* rx_buffer)
{
    tx_usart_val = tx_buffer;
    rx_usart_val = rx_buffer;
}

/**
 *  Initialize usr function to use in the RX callback
 */
void init_usrFunc(dma_callbackRXfunc_t fnc_callback)
{
    user_fnc = fnc_callback;
}

/**
 *  Initialize baudrate by user
 */
void init_usrBaudrate(uint32_t usr_baud)
{
    baud = usr_baud;
}

/**
 *  Set the size of data we send ie. the number of bytes.
 *  eg. : size = 5 means that we send 5 byte (40 bits) of data to USART 3.
 */
void init_usrDataSize(uint16_t size)
{
    dma_buffer_size = size;
    rx_buffer_size = size;
}

/**
 *  This function initialise USART3 peripheral
 */
void serial_init(void)
{
    uart_config_get(uart_dev, &uart_cfg);
    uart_cfg.baudrate = baud;
    uart_cfg.flow_ctrl = UART_CFG_FLOW_CTRL_NONE;
    uart_cfg.data_bits = UART_CFG_DATA_BITS_8;
    uart_cfg.parity = UART_CFG_PARITY_NONE;
    uart_cfg.stop_bits = UART_CFG_STOP_BITS_1;

    uart_configure(uart_dev, &uart_cfg);

    LL_USART_ConfigAsyncMode(USART3);

    /* Enable DMA request*/
    LL_USART_EnableDMAReq_TX(USART3);
    LL_USART_EnableDMAReq_RX(USART3);

    /* Disable Interrupts for TX (not used for DMA) */

    /* Disable Transmission Complete Interrupt */
    LL_USART_DisableIT_TC(USART3);

    /* Disable Transmission Data Register Empty Interrupt
     * for DMA to provide data.
     * Disable interrupts for RX (not used with DMA) */
    LL_USART_DisableIT_TXE_TXFNF(USART3);
    /* Disable Receiver Data Register
     * Not Empty Interrupt for DMA to fetch data */
    LL_USART_DisableIT_RXNE_RXFNE(USART3);

    if (synchronous_mode)
    {
        LL_USART_DisableIT_IDLE(USART3);
        LL_USART_DisableIT_PE(USART3);
        LL_USART_DisableIT_ERROR(USART3);
    }
    LL_USART_Enable(USART3);
}

/**
 *  Initialize driver enable mode for RS485 flowcontrol.
 *  See RM0440 37.5.20 for more details.
 */
void init_DEmode(void)
{
    LL_USART_Disable(USART3);

    /* GPIO initialization and GPIO clock set-up */
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};
    LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOB);
    /* Set GPIO_InitStruct */
    GPIO_InitStruct.Pin = LL_GPIO_PIN_14;
    GPIO_InitStruct.Mode = LL_GPIO_MODE_ALTERNATE;
    GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
    GPIO_InitStruct.Alternate = LL_GPIO_AF_7;
    LL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* Enable DE mode */
    LL_USART_EnableDEMode(USART3);
    /* Polarity is high ie. PB14 will be at high level when sending datas */
    LL_USART_SetDESignalPolarity(USART3, LL_USART_DE_POLARITY_HIGH);

    /* Assertion time is set to maximum */
    LL_USART_SetDEAssertionTime(USART3, 31);
    LL_USART_SetDEDeassertionTime(USART3, 31);

    LL_USART_Enable(USART3);
}


/**
 *  Set the oversampling.
 *  See RM0440 37.5.7 for more details.
 */
void oversamp_set(usart_oversampling_t oversampling)
{
    LL_USART_Disable(USART3);
    LL_USART_SetOverSampling(USART3, oversampling);
    LL_USART_Enable(USART3);
}

/**
 *  This is the TX dma channel initialization.
 *  The channel is not enabled here to avoid sending data unexpectedly,
 *  this channel is enabled only with serial_tx_on when data must be sent.
 */
void dma_channel_init_tx()
{
    serial_sync_tx_stop();
    sync_tx_configured = false;
    synchronous_mode = false;
    communication_dispatch_reset();
    /* Configure DMA */
    struct dma_config dma_config_s = {0};
    LL_DMA_InitTypeDef DMA_InitStruct = {0};

    /* Callback function set-up */
    dma_config_s.dma_callback = _dma_callback_tx;
    /* HAL override */
    dma_config_s.linked_channel = STM32_DMA_HAL_OVERRIDE;

    /* DMA configuration with LL drivers */
    DMA_InitStruct.Direction = LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
    DMA_InitStruct.PeriphOrM2MSrcAddress = (uint32_t)(&(USART3->TDR));
    DMA_InitStruct.MemoryOrM2MDstAddress = (uint32_t)(tx_usart_val);
    DMA_InitStruct.Mode = LL_DMA_MODE_NORMAL;
    DMA_InitStruct.MemoryOrM2MDstDataSize = LL_DMA_MDATAALIGN_BYTE;
    DMA_InitStruct.PeriphOrM2MSrcDataSize = LL_DMA_PDATAALIGN_BYTE;
    DMA_InitStruct.PeriphOrM2MSrcIncMode = LL_DMA_PERIPH_NOINCREMENT;
    DMA_InitStruct.MemoryOrM2MDstIncMode = LL_DMA_MEMORY_INCREMENT;
    DMA_InitStruct.PeriphRequest = LL_DMAMUX_REQ_USART3_TX;
    DMA_InitStruct.NbData = dma_buffer_size;

    /* Indicates callback function to zephyr driver */
    dma_config(dma1, ZEPHYR_DMA_CHANNEL_TX, &dma_config_s);
    /* Disabling channel for initial set-up */
    LL_DMA_DisableChannel(DMA_USART, LL_DMA_CHANNEL_TX);

    /* Initialize DMA */

    /* DMA data size */
    LL_DMA_SetDataLength(DMA_USART, LL_DMA_CHANNEL_TX, dma_buffer_size);
    /* LL_DMA_Init writes the priority along with the channel settings. */
    DMA_InitStruct.Priority = LL_DMA_PRIORITY_VERYHIGH;

    LL_DMA_Init(DMA_USART, LL_DMA_CHANNEL_TX, &DMA_InitStruct);

    /* Clearing flags */
    LL_DMA_ClearFlag_TC6(DMA_USART);
    LL_DMA_ClearFlag_HT6(DMA_USART);

    /* Enable transfer complete interruption */
    LL_DMA_EnableIT_TC(DMA_USART, LL_DMA_CHANNEL_TX);
    LL_DMA_DisableIT_TE(DMA_USART, LL_DMA_CHANNEL_TX);
    /* Disable half-transfer interruption */
    LL_DMA_DisableIT_HT(DMA_USART, LL_DMA_CHANNEL_TX);
    tx_configured = true;
}


/**
 *  DMA channel RX initialization, this channel is set on circular mode.
 */
void dma_channel_init_rx()
{
    /* Configure DMA */
    LL_DMA_InitTypeDef DMA_InitStruct = {0};

    /* Initialization of DMA */
    DMA_InitStruct.Direction = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
    DMA_InitStruct.PeriphOrM2MSrcAddress = (uint32_t)(&(USART3->RDR));
    DMA_InitStruct.MemoryOrM2MDstAddress = (uint32_t)(rx_usart_val);
    DMA_InitStruct.Mode = LL_DMA_MODE_CIRCULAR;
    DMA_InitStruct.MemoryOrM2MDstDataSize = LL_DMA_MDATAALIGN_BYTE;
    DMA_InitStruct.PeriphOrM2MSrcDataSize = LL_DMA_PDATAALIGN_BYTE;
    DMA_InitStruct.PeriphOrM2MSrcIncMode = LL_DMA_PERIPH_NOINCREMENT;
    DMA_InitStruct.MemoryOrM2MDstIncMode = LL_DMA_MEMORY_INCREMENT;
    DMA_InitStruct.PeriphRequest = LL_DMAMUX_REQ_USART3_RX;
    DMA_InitStruct.NbData = rx_buffer_size;

    IRQ_DIRECT_CONNECT(17, 0, _dma_callback_rx, IRQ_ZERO_LATENCY);
    if (synchronous_mode)
        irq_disable(17);
    else
        irq_enable(17);

    /* Disabling channel for initial set-up */
    LL_DMA_DisableChannel(DMA_USART, LL_DMA_CHANNEL_RX);

    /* Initialize DMA */

    /* DMA data size */
    LL_DMA_SetDataLength(DMA_USART, LL_DMA_CHANNEL_RX, rx_buffer_size);
    /* LL_DMA_Init writes the priority along with the channel settings. */
    DMA_InitStruct.Priority = LL_DMA_PRIORITY_VERYHIGH;

    LL_DMA_Init(DMA_USART, LL_DMA_CHANNEL_RX, &DMA_InitStruct);

    /* Clearing flag */
    LL_DMA_ClearFlag_TC7(DMA_USART);
    LL_DMA_ClearFlag_HT7(DMA_USART);

    /* Enabling channel */
    LL_DMA_EnableChannel(DMA_USART, LL_DMA_CHANNEL_RX);
    /* Enable transfer complete interruption */
    if (synchronous_mode)
        LL_DMA_DisableIT_TC(DMA_USART, LL_DMA_CHANNEL_RX);
    else
        LL_DMA_EnableIT_TC(DMA_USART, LL_DMA_CHANNEL_RX);
    LL_DMA_DisableIT_TE(DMA_USART, LL_DMA_CHANNEL_RX);
    /* Disable half-transfer interruption */
    LL_DMA_DisableIT_HT(DMA_USART, LL_DMA_CHANNEL_RX);
}


/**
 *  Reload DMA TX buffer.
 *  This functions enable TX channel to start sending the datas
 */
void serial_tx_on()
{
    /* Synchronous mode is armed only through prepareSynchronizedTransmission. */
    if (synchronous_mode || serial_tx_busy())
        return;
    serial_sync_tx_stop();
    /* Making sure the flag is cleared before transmission */
    LL_DMA_ClearFlag_TC6(DMA_USART);
    /* Disable channel to reload TX buffer */
    LL_DMA_DisableChannel(DMA_USART, LL_DMA_CHANNEL_TX);
    /* Reloading TX buffer */
    LL_DMA_SetMemoryAddress(DMA_USART,
                            LL_DMA_CHANNEL_TX,
                            (uint32_t)(tx_usart_val));

    LL_DMA_SetDataLength(DMA_USART, LL_DMA_CHANNEL_TX, dma_buffer_size);
    /* Re-enable the channel */
    LL_DMA_EnableChannel(DMA_USART, LL_DMA_CHANNEL_TX);
}

int serial_sync_tx_config(uint32_t delay_us)
{
    if (!tx_configured || tx_usart_val == NULL || dma_buffer_size == 0 ||
        delay_us == 0 || delay_us > (UINT32_MAX - 1U) / 10U)
        return -EINVAL;

    if (!device_is_ready(timer2))
        return -ENODEV;

    bool master = LL_HRTIM_GetSyncInSrc(HRTIM1) == LL_HRTIM_SYNCIN_SRC_NONE;
    uint32_t source = LL_HRTIM_SYNCOUT_SRC_TIMA_START;
    if ((!master && LL_HRTIM_GetSyncInSrc(HRTIM1) != LL_HRTIM_SYNCIN_SRC_EXTERNAL_EVENT) ||
        LL_HRTIM_GetSyncOutSrc(HRTIM1) != source ||
        LL_HRTIM_GetSyncOutConfig(HRTIM1) != LL_HRTIM_SYNCOUT_POSITIVE_PULSE)
        return -EINVAL;
    if (!timer2_sync_enabled())
        return -EAGAIN;
    if (delay_us * 10U >= LL_TIM_OC_GetCompareCH2(TIM2))
        return -EINVAL;

    if (LL_DMA_IsEnabledChannel(DMA_USART, LL_DMA_CHANNEL_TX) ||
        !LL_USART_IsActiveFlag_TC(USART3))
        return -EBUSY;

    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
    if (!sync_tx_configured && LL_DMA_IsEnabledChannel(DMA1, LL_DMA_CHANNEL_8))
        return -EBUSY;

    serial_sync_tx_stop();
    LL_DMA_InitTypeDef DMA_InitStruct = {0};
    DMA_InitStruct.Direction = LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
    DMA_InitStruct.PeriphOrM2MSrcAddress = (uint32_t)(&(DMA1_Channel6->CCR));
    DMA_InitStruct.MemoryOrM2MDstAddress = (uint32_t)(&sync_tx_enable);
    DMA_InitStruct.Mode = LL_DMA_MODE_NORMAL;
    DMA_InitStruct.MemoryOrM2MDstDataSize = LL_DMA_MDATAALIGN_WORD;
    DMA_InitStruct.PeriphOrM2MSrcDataSize = LL_DMA_PDATAALIGN_WORD;
    DMA_InitStruct.PeriphOrM2MSrcIncMode = LL_DMA_PERIPH_NOINCREMENT;
    DMA_InitStruct.MemoryOrM2MDstIncMode = LL_DMA_MEMORY_NOINCREMENT;
    DMA_InitStruct.PeriphRequest = LL_DMAMUX_REQ_TIM2_CH1;
    DMA_InitStruct.NbData = 1;
    DMA_InitStruct.Priority = LL_DMA_PRIORITY_VERYHIGH;
    LL_DMA_Init(DMA1, LL_DMA_CHANNEL_8, &DMA_InitStruct);
    LL_DMA_DisableIT_TC(DMA1, LL_DMA_CHANNEL_8);
    LL_DMA_DisableIT_HT(DMA1, LL_DMA_CHANNEL_8);
    LL_DMA_DisableIT_TE(DMA1, LL_DMA_CHANNEL_8);
    LL_DMA_ClearFlag_GI8(DMA1);

    struct timer_config_t timer_cfg = {0};
    timer_cfg.timer_compare_t_usec = delay_us;
    timer_cfg.timer_enable_compare_dma = 1;
    timer_config(timer2, &timer_cfg);
    sync_tx_configured = true;
    return 0;
}

int serial_sync_tx_prepare()
{
    serial_tx_poll();
    if (!sync_tx_configured)
        return -EINVAL;

    /* Completion is checked rather than CNDTR: the count can reach zero
     * before the peripheral write enabling TX has finished.
     */
    if ((LL_DMA_IsEnabledChannel(DMA1, LL_DMA_CHANNEL_8) &&
         !LL_DMA_IsActiveFlag_TC8(DMA1)) ||
        LL_DMA_IsEnabledChannel(DMA_USART, LL_DMA_CHANNEL_TX) ||
        !LL_USART_IsActiveFlag_TC(USART3))
        return -EBUSY;

    serial_sync_tx_stop();
    LL_DMA_ClearFlag_TC6(DMA_USART);
    LL_DMA_ClearFlag_HT6(DMA_USART);
    LL_DMA_SetMemoryAddress(DMA_USART, LL_DMA_CHANNEL_TX, (uint32_t)tx_usart_val);
    LL_DMA_SetDataLength(DMA_USART, LL_DMA_CHANNEL_TX, dma_buffer_size);
    /* Snapshot all channel settings, adding only the enable bit. */
    sync_tx_enable = DMA1_Channel6->CCR | DMA_CCR_EN;
    LL_DMA_ClearFlag_GI8(DMA1);
    LL_DMA_SetDataLength(DMA1, LL_DMA_CHANNEL_8, 1);
    __DMB();
    LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_8);
    timer_start(timer2);
    return 0;
}

void serial_sync_tx_stop()
{
    if (!sync_tx_configured)
        return;

    /* Cancel TX without discarding the phase acquired from HRTIM sync. */
    timer2_compare_disarm();
    LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_8);
    LL_DMA_ClearFlag_GI8(DMA1);
}

/* Poll completion in synchronous mode; USART TC remains set until next TX. */
void serial_tx_poll()
{
    if (synchronous_mode && LL_DMA_IsActiveFlag_TC6(DMA_USART))
    {
        LL_DMA_DisableChannel(DMA_USART, LL_DMA_CHANNEL_TX);
        LL_DMA_ClearFlag_TC6(DMA_USART);
        LL_DMA_ClearFlag_HT6(DMA_USART);
    }
}

uint32_t serial_poll_errors()
{
    if (!synchronous_mode)
        return RS485_ERROR_NONE;

    /* Snapshot before clearing: faults must remain visible to the caller. */
    const uint32_t uart_faults = USART3->ISR &
        (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE);
    uint32_t errors = RS485_ERROR_NONE;
    if (uart_faults & USART_ISR_ORE)
        errors |= RS485_ERROR_RX_OVERRUN;
    if (uart_faults & USART_ISR_FE)
        errors |= RS485_ERROR_RX_FRAMING;
    if (uart_faults & USART_ISR_NE)
        errors |= RS485_ERROR_RX_NOISE;
    if (uart_faults & USART_ISR_PE)
        errors |= RS485_ERROR_RX_PARITY;
    if (LL_DMA_IsActiveFlag_TE7(DMA_USART))
        errors |= RS485_ERROR_RX_DMA;
    if (LL_DMA_IsActiveFlag_TE6(DMA_USART))
        errors |= RS485_ERROR_TX_DMA;
    if (sync_tx_configured && LL_DMA_IsActiveFlag_TE8(DMA1))
        errors |= RS485_ERROR_TRIGGER_DMA;

    if (errors & (RS485_ERROR_TX_DMA | RS485_ERROR_TRIGGER_DMA))
    {
        /* Stop the trigger before disabling TX; never retry a partial frame
         * automatically. Bytes already in USART may finish shifting out.
         */
        serial_sync_tx_stop();
        LL_DMA_DisableChannel(DMA_USART, LL_DMA_CHANNEL_TX);
        LL_DMA_ClearFlag_GI6(DMA_USART);
    }
    if (errors & RS485_RX_ERRORS)
    {
        /* Data after a receive fault cannot be presented as a valid snapshot.
         * Flush pending UART data and restart DMA from the ring base.
         */
        LL_USART_DisableDMAReq_RX(USART3);
        LL_DMA_DisableChannel(DMA_USART, LL_DMA_CHANNEL_RX);
        LL_USART_RequestRxDataFlush(USART3);
        WRITE_REG(USART3->ICR, uart_faults);
        LL_DMA_ClearFlag_GI7(DMA_USART);
        LL_DMA_SetMemoryAddress(DMA_USART, LL_DMA_CHANNEL_RX, (uint32_t)rx_usart_val);
        LL_DMA_SetDataLength(DMA_USART, LL_DMA_CHANNEL_RX, rx_buffer_size);
        __DMB();
        LL_DMA_EnableChannel(DMA_USART, LL_DMA_CHANNEL_RX);
        LL_USART_EnableDMAReq_RX(USART3);
    }
    return errors;
}

bool serial_tx_busy()
{
    serial_tx_poll();
    return (sync_tx_configured &&
            LL_DMA_IsEnabledChannel(DMA1, LL_DMA_CHANNEL_8) &&
            !LL_DMA_IsActiveFlag_TC8(DMA1)) ||
        (tx_configured && (LL_DMA_IsEnabledChannel(DMA_USART, LL_DMA_CHANNEL_TX) ||
         !LL_USART_IsActiveFlag_TC(USART3)));
}

uint16_t serial_rx_write_position()
{
    return (rx_buffer_size - LL_DMA_GetDataLength(DMA_USART, LL_DMA_CHANNEL_RX))
        % rx_buffer_size;
}

void serial_synchronous_mode(uint16_t reception_size)
{
    synchronous_mode = true;
    user_fnc = NULL;
    rx_buffer_size = reception_size;
    /* Configure mode before initializing the RX DMA ring. */
    LL_DMA_DisableIT_TC(DMA_USART, LL_DMA_CHANNEL_TX);
    LL_DMA_DisableIT_HT(DMA_USART, LL_DMA_CHANNEL_TX);
    LL_DMA_DisableIT_TE(DMA_USART, LL_DMA_CHANNEL_TX);
    irq_disable(17);
}

/**
 *  To stop communication
 */
void serial_stop()
{
    serial_sync_tx_stop();
    LL_USART_Disable(USART3);
}

/**
 *  To start or restart communication
 */
void serial_start()
{
    LL_USART_Enable(USART3);
}
