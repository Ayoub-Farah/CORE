/*
 * Copyright (c) 2021-present LAAS-CNRS
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
 * @date   2022
 * @author Clément Foucher <clement.foucher@laas.fr>
 */


/* STM32 LL */
#include <stm32_ll_tim.h>
#include <stm32_ll_bus.h>
#include <stm32_ll_gpio.h>

/* Current file header */
#include "stm32_timer_driver.h"
#include <errno.h>

/* The ZLI handler reaches PB1 about 1.44 us after CH2 on SPIN.
 * Allow 3 us so SCOUT and ITR10 open before the control boundary.
 */
#define TIMER2_SYNC_LEAD_TICKS 30U

static bool timer2_master_mode = false;
static bool timer2_sync_started = false;
static bool timer2_sync_acquiring = false;
static bool timer2_sync_dma_armed = false;
static bool timer2_sync_irq_armed = false;
static uint32_t timer2_sync_open_ticks;
static bool timer2_slave_mode = false;

bool timer2_sync_enabled(void)
{
	return timer2_master_mode || timer2_slave_mode;
}

bool timer2_master_sync_enabled(void)
{
	return timer2_master_mode;
}


static int timer_stm32_init(const struct device* dev)
{
	TIM_TypeDef* tim_dev =
				((struct stm32_timer_driver_data*)dev->data)->timer_struct;

	if (tim_dev == TIM2)
		init_timer_2();
	else if (tim_dev == TIM4)
		init_timer_4();
	else if (tim_dev == TIM3)
		init_timer_3();
	else if (tim_dev == TIM6)
		init_timer_6();
	else if (tim_dev == TIM7)
		init_timer_7();
	else
		return -1;

	return 0;
}

/**
 * @brief Callback function triggered by a timer interrupt.
 *
 * This function is called when the hardware timer generates an interrupt.
 * It clears the interrupt flag and calls the user-defined callback if it
 * has been set.
 *
 * - Casts the generic argument to a device pointer.
 *
 * - Clears the hardware timer interrupt flag.
 *
 * - Invokes the registered callback function, if available.
 *
 * @param arg Pointer to the timer device (cast from a generic void pointer).
 *
 */
static void timer_stm32_callback(const void* arg)
{
	const struct device* timer_dev = (const struct device*)arg;
	struct stm32_timer_driver_data* data =
							(struct stm32_timer_driver_data*)timer_dev->data;

	if (data->timer_struct == TIM2)
	{
		if (timer2_sync_enabled() && LL_TIM_IsEnabledIT_CC2(TIM2) &&
			LL_TIM_IsActiveFlag_CC2(TIM2))
		{
			LL_TIM_ClearFlag_CC2(TIM2);
			LL_TIM_DisableIT_CC2(TIM2);
			/* Only accept the next PWM reset, at the control boundary. */
			LL_TIM_ClearFlag_TRIG(TIM2);
			LL_TIM_SetSlaveMode(TIM2, LL_TIM_SLAVEMODE_COMBINED_RESETTRIGGER);
			if (timer2_master_mode)
				LL_GPIO_SetPinMode(GPIOB, LL_GPIO_PIN_1, LL_GPIO_MODE_ALTERNATE);
		}
		if ( !LL_TIM_IsEnabledIT_CC1(TIM2) || !LL_TIM_IsActiveFlag_CC1(TIM2) )
			return;

		LL_TIM_ClearFlag_CC1(TIM2);
	}
	else
	{
		timer_stm32_clear(timer_dev);
	}

	if (data->timer_irq_callback != NULL)
	{
		data->timer_irq_callback();
	}
}


/** @brief Defines a structure to hold the timer functions   */
static const struct timer_driver_api timer_funcs =
{
	.config    = timer_stm32_config,
	.start     = timer_stm32_start,
	.stop      = timer_stm32_stop,
	.get_count = timer_stm32_get_count
};

void timer_stm32_config(const struct device* dev,
						const struct timer_config_t* config)
{
	struct stm32_timer_driver_data* data =
						(struct stm32_timer_driver_data*)dev->data;

	TIM_TypeDef* tim_dev = data->timer_struct;

	if ( (tim_dev == TIM2) || (tim_dev == TIM6) || (tim_dev == TIM7) )
	{
		if (tim_dev == TIM2)
		{
			if (!timer2_sync_enabled())
				timer_stm32_stop(dev);
			else
			{
				timer2_compare_disarm();
			}
			data->timer_mode = unconfigured;
			/* Leave room for ARR beyond the CH1 compare value. */
			if (config->timer_compare_t_usec == 0 ||
				config->timer_compare_t_usec > (UINT32_MAX - 1U) / 10U)
				return;
			if (timer2_sync_enabled() &&
				config->timer_compare_t_usec * 10U >= timer2_sync_open_ticks)
				return;

			data->timer_compare_dma = config->timer_enable_compare_dma;
			if (data->timer_compare_dma)
			{
				data->timer_mode = synchronized_compare;
				data->timer_compare_usec = config->timer_compare_t_usec;
			}
		}

		if (config->timer_enable_irq == 1)
		{
			data->timer_mode = (tim_dev == TIM2) ?
							   synchronized_compare : periodic_interrupt;
			data->timer_irq_callback = config->timer_irq_callback;
			data->timer_irq_period_usec = config->timer_irq_t_usec;
			if (tim_dev == TIM2)
				data->timer_compare_usec = config->timer_compare_t_usec;
			uint32_t flags = 0;

			if (config->timer_use_zero_latency == 1 ||
				(tim_dev == TIM2 && timer2_sync_enabled()))
			{
				flags = IRQ_ZERO_LATENCY;
			}

			irq_disable(data->interrupt_line);
			irq_connect_dynamic(data->interrupt_line,
								data->interrupt_prio,
								timer_stm32_callback,
								dev,
								flags);

			irq_enable(data->interrupt_line);
		}
		if (tim_dev == TIM2 && data->timer_mode == synchronized_compare)
		{
			if (!timer2_sync_enabled())
				data->timer_mode = unconfigured;
			else
				LL_TIM_OC_SetCompareCH1(TIM2, data->timer_compare_usec * 10U);
		}
	}
	else if (tim_dev == TIM4)
	{
		if (config->timer_enable_encoder == 1)
		{
			data->timer_mode = incremental_coder;

			uint32_t pull = 0;
			switch (config->timer_enc_pin_mode)
			{
				case no_pull:
					pull = LL_GPIO_PULL_NO;
					break;
				case pull_up:
					pull = LL_GPIO_PULL_UP;
					break;
				case pull_down:
					pull = LL_GPIO_PULL_DOWN;
					break;
			}

			/* Configure GPIO */
			LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOB);

			LL_GPIO_SetPinMode(GPIOB,
							   LL_GPIO_PIN_3,
							   LL_GPIO_MODE_ALTERNATE);

			LL_GPIO_SetPinSpeed(GPIOB,
								LL_GPIO_PIN_3,
								LL_GPIO_SPEED_FREQ_LOW);
			LL_GPIO_SetPinOutputType(GPIOB,
									 LL_GPIO_PIN_3,
									 LL_GPIO_OUTPUT_PUSHPULL);

			LL_GPIO_SetPinPull(GPIOB,
							   LL_GPIO_PIN_3,
							   pull);

			LL_GPIO_SetAFPin_0_7(GPIOB,
								 LL_GPIO_PIN_3,
								 LL_GPIO_AF_2);

			LL_GPIO_SetPinMode(GPIOB,
							   LL_GPIO_PIN_6,
							   LL_GPIO_MODE_ALTERNATE);

			LL_GPIO_SetPinSpeed(GPIOB,
								LL_GPIO_PIN_6,
								LL_GPIO_SPEED_FREQ_LOW);

			LL_GPIO_SetPinOutputType(GPIOB,
									 LL_GPIO_PIN_6,
									 LL_GPIO_OUTPUT_PUSHPULL);

			LL_GPIO_SetPinPull(GPIOB,LL_GPIO_PIN_6,pull);
			LL_GPIO_SetAFPin_0_7(GPIOB,LL_GPIO_PIN_6,LL_GPIO_AF_2);
			LL_GPIO_SetPinMode(GPIOB,LL_GPIO_PIN_7,LL_GPIO_MODE_ALTERNATE);
			LL_GPIO_SetPinSpeed(GPIOB,LL_GPIO_PIN_7,LL_GPIO_SPEED_FREQ_LOW);

			LL_GPIO_SetPinOutputType(GPIOB,
									 LL_GPIO_PIN_7,
									 LL_GPIO_OUTPUT_PUSHPULL);

			LL_GPIO_SetPinPull(GPIOB,LL_GPIO_PIN_7,pull);
			LL_GPIO_SetAFPin_0_7(GPIOB,LL_GPIO_PIN_7,LL_GPIO_AF_2);
		}
	} 
	else if (tim_dev == TIM3)
	{
		if (config->timer_enable_encoder == 1)
		{
			data->timer_mode = incremental_coder;

			uint32_t pull = 0;
			switch (config->timer_enc_pin_mode)
			{
				case no_pull:
					pull = LL_GPIO_PULL_NO;
					break;
				case pull_up:
					pull = LL_GPIO_PULL_UP;
					break;
				case pull_down:
					pull = LL_GPIO_PULL_DOWN;
					break;
			}

			/* Configure GPIO */
			LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOD);

			LL_GPIO_SetPinMode(GPIOD,
								LL_GPIO_PIN_2,
								LL_GPIO_MODE_ALTERNATE);

			LL_GPIO_SetPinSpeed(GPIOD,
								LL_GPIO_PIN_2,
								LL_GPIO_SPEED_FREQ_LOW);
			LL_GPIO_SetPinOutputType(GPIOD,
										LL_GPIO_PIN_2,
										LL_GPIO_OUTPUT_PUSHPULL);

			LL_GPIO_SetPinPull(GPIOD,
								LL_GPIO_PIN_2,
								pull);

			LL_GPIO_SetAFPin_0_7(GPIOD,
									LL_GPIO_PIN_2,
									LL_GPIO_AF_2);

			LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOC);

			LL_GPIO_SetPinMode(GPIOC,
								LL_GPIO_PIN_6,
								LL_GPIO_MODE_ALTERNATE);

			LL_GPIO_SetPinSpeed(GPIOC,
								LL_GPIO_PIN_6,
								LL_GPIO_SPEED_FREQ_LOW);

			LL_GPIO_SetPinOutputType(GPIOC,
										LL_GPIO_PIN_6,
										LL_GPIO_OUTPUT_PUSHPULL);

			LL_GPIO_SetPinPull(GPIOC,LL_GPIO_PIN_6,pull);
			LL_GPIO_SetAFPin_0_7(GPIOC,LL_GPIO_PIN_6,LL_GPIO_AF_2);
			LL_GPIO_SetPinMode(GPIOC,LL_GPIO_PIN_7,LL_GPIO_MODE_ALTERNATE);
			LL_GPIO_SetPinSpeed(GPIOC,LL_GPIO_PIN_7,LL_GPIO_SPEED_FREQ_LOW);

			LL_GPIO_SetPinOutputType(GPIOC,
										LL_GPIO_PIN_7,
										LL_GPIO_OUTPUT_PUSHPULL);

			LL_GPIO_SetPinPull(GPIOC,LL_GPIO_PIN_7,pull);
			LL_GPIO_SetAFPin_0_7(GPIOC,LL_GPIO_PIN_7,LL_GPIO_AF_2);
		}
	}
}

void timer_stm32_start(const struct device* dev)
{
	struct stm32_timer_driver_data* data =
							(struct stm32_timer_driver_data*)dev->data;

	TIM_TypeDef* tim_dev = data->timer_struct;

	if (tim_dev == TIM2)
	{
		if (data->timer_mode == synchronized_compare)
		{
			if (timer2_sync_enabled())
			{
				/* Rearm TX without disturbing the control-period clock. */
				LL_TIM_OC_SetCompareCH1(TIM2, data->timer_compare_usec * 10U);
				LL_TIM_ClearFlag_CC1(TIM2);
				if (timer2_sync_acquiring)
				{
					timer2_sync_dma_armed = data->timer_compare_dma;
					timer2_sync_irq_armed = !data->timer_compare_dma;
					return;
				}
				if (data->timer_compare_dma)
					LL_TIM_EnableDMAReq_CC1(TIM2);
				else
					LL_TIM_EnableIT_CC1(TIM2);
				return;
			}
		}
	}
	else if ( (tim_dev == TIM6) || (tim_dev == TIM7) )
	{
		if (data->timer_mode == periodic_interrupt)
		{
			LL_TIM_SetAutoReload(tim_dev,
								 (data->timer_irq_period_usec*10) - 1);
			LL_TIM_EnableIT_UPDATE(tim_dev);
			LL_TIM_EnableCounter(tim_dev);
		}
	}
	else if (tim_dev == TIM4 || tim_dev == TIM3)
	{
		if (data->timer_mode == incremental_coder)
		{
			LL_TIM_EnableCounter(tim_dev);
		}
	}
}

void timer_stm32_stop(const struct device* dev)
{
	struct stm32_timer_driver_data* data =
								(struct stm32_timer_driver_data*)dev->data;

	TIM_TypeDef* tim_dev = data->timer_struct;

	if (tim_dev == TIM2)
	{
		timer2_sync_stop();
		/* Disable the trigger as well, so incoming sync cannot restart it. */
		LL_TIM_SetSlaveMode(TIM2, LL_TIM_SLAVEMODE_DISABLED);
		LL_TIM_DisableCounter(TIM2);
		LL_TIM_DisableIT_CC1(TIM2);
		LL_TIM_DisableDMAReq_CC1(TIM2);
		LL_TIM_ClearFlag_CC1(TIM2);
		LL_TIM_ClearFlag_UPDATE(TIM2);
	}
	else if ( (tim_dev == TIM6) || (tim_dev == TIM7) )
	{
		if (data->timer_mode == periodic_interrupt)
		{
			LL_TIM_DisableCounter(tim_dev);
			LL_TIM_DisableIT_UPDATE(tim_dev);
		}
	}
	else if (tim_dev == TIM4 || tim_dev == TIM3)
	{
		if (data->timer_mode == incremental_coder)
		{
			LL_TIM_DisableCounter(tim_dev);
		}
	}
}

void timer_stm32_clear(const struct device* dev)
{
	TIM_TypeDef* tim_dev =
				((struct stm32_timer_driver_data*)dev->data)->timer_struct;

	if (tim_dev != NULL)
	{
		LL_TIM_ClearFlag_UPDATE(tim_dev);
	}
}

uint32_t timer_stm32_get_count(const struct device* dev)
{
	TIM_TypeDef* tim_dev =
				((struct stm32_timer_driver_data*)dev->data)->timer_struct;

	return LL_TIM_GetCounter(tim_dev);
}

static int timer2_sync_configure(uint32_t control_ticks, uint32_t pwm_ticks,
								 bool master)
{
#if DT_NODE_HAS_STATUS(TIMER2_NODE, okay)
	const struct device* dev = DEVICE_DT_GET(TIMER2_DEVICE);
	/* The opening lead must fit entirely between two PWM events. */
	if (!device_is_ready(dev))
		return -ENODEV;
	if (control_ticks <= TIMER2_SYNC_LEAD_TICKS ||
		pwm_ticks <= TIMER2_SYNC_LEAD_TICKS || control_ticks == UINT32_MAX)
		return -EINVAL;

	bool dma_armed = LL_TIM_IsEnabledDMAReq_CC1(TIM2) || timer2_sync_dma_armed;
	bool irq_armed = LL_TIM_IsEnabledIT_CC1(TIM2) || timer2_sync_irq_armed;
	struct stm32_timer_driver_data* data = dev->data;
	if (data->timer_mode == synchronized_compare &&
		data->timer_compare_usec * 10U >= control_ticks - TIMER2_SYNC_LEAD_TICKS)
		return -EINVAL;
	irq_disable(TIMER2_INTERRUPT_LINE);
	timer2_sync_stop();
	timer2_sync_open_ticks = control_ticks - TIMER2_SYNC_LEAD_TICKS;
	LL_TIM_SetSlaveMode(TIM2, LL_TIM_SLAVEMODE_DISABLED);
	LL_TIM_DisableCounter(TIM2);
	LL_TIM_DisableIT_CC1(TIM2);
	LL_TIM_DisableIT_CC2(TIM2);
	LL_TIM_DisableIT_TRIG(TIM2);
	LL_TIM_DisableDMAReq_CC1(TIM2);
	/* Keep both modes counting until the selected hardware reset. With a
	 * one-pulse master, overflow near the next control boundary can clear
	 * CEN after the trigger starts the counter, losing the next CH1/CH2
	 * compares and forcing phase reacquisition. CH2 and the HRTIM callback
	 * gate the resets; ARR must not introduce another period boundary.
	 */
	LL_TIM_SetOnePulseMode(TIM2, LL_TIM_ONEPULSEMODE_REPETITIVE);
	LL_TIM_SetAutoReload(TIM2, UINT32_MAX);
	LL_TIM_OC_SetMode(TIM2, LL_TIM_CHANNEL_CH2, LL_TIM_OCMODE_FROZEN);
	LL_TIM_OC_SetCompareCH2(TIM2, timer2_sync_open_ticks);
	LL_TIM_GenerateEvent_UPDATE(TIM2);
	LL_TIM_SetCounter(TIM2, 0);
	LL_TIM_ClearFlag_UPDATE(TIM2);
	LL_TIM_ClearFlag_CC1(TIM2);
	LL_TIM_ClearFlag_CC2(TIM2);
	LL_TIM_ClearFlag_TRIG(TIM2);
	NVIC_ClearPendingIRQ((IRQn_Type)TIMER2_INTERRUPT_LINE);
	irq_connect_dynamic(TIMER2_INTERRUPT_LINE, TIMER2_INTERRUPT_PRIO,
						timer_stm32_callback, dev, IRQ_ZERO_LATENCY);
	timer2_master_mode = master;
	timer2_slave_mode = !master;
	timer2_sync_acquiring = true;
	/* Defer prepared TX until hardware has acquired a control boundary. */
	timer2_sync_dma_armed = dma_armed;
	timer2_sync_irq_armed = irq_armed;
	irq_enable(TIMER2_INTERRUPT_LINE);
	return 0;
#else
	return -ENODEV;
#endif
}

int timer2_master_sync_configure(uint32_t control_ticks, uint32_t pwm_ticks)
{
	return timer2_sync_configure(control_ticks, pwm_ticks, true);
}

int timer2_slave_sync_configure(uint32_t control_ticks, uint32_t pwm_ticks)
{
	return timer2_sync_configure(control_ticks, pwm_ticks, false);
}

void timer2_compare_disarm(void)
{
	LL_TIM_DisableIT_CC1(TIM2);
	LL_TIM_DisableDMAReq_CC1(TIM2);
	timer2_sync_dma_armed = false;
	timer2_sync_irq_armed = false;
}

static void timer2_sync_event(void)
{
	if (!timer2_sync_enabled())
		return;

	/* Prevent the zero-latency CH2 handler from reopening the pin while
	 * the HRTIM repetition/SCIN callback is closing the sync window.
	 */
	LL_TIM_DisableIT_CC2(TIM2);
	if (timer2_master_mode)
		LL_GPIO_SetPinMode(GPIOB, LL_GPIO_PIN_1, LL_GPIO_MODE_OUTPUT);
	LL_TIM_ClearFlag_CC2(TIM2);
	if (!timer2_sync_started || !LL_TIM_IsActiveFlag_TRIG(TIM2))
	{
		/* Acquire phase from PWM resets until the next HRTIM callback.
		 * Keep SCOUT closed and TX deferred. This also recovers a missed gate.
		 */
		timer2_sync_dma_armed |= LL_TIM_IsEnabledDMAReq_CC1(TIM2);
		timer2_sync_irq_armed |= LL_TIM_IsEnabledIT_CC1(TIM2);
		LL_TIM_DisableDMAReq_CC1(TIM2);
		LL_TIM_DisableIT_CC1(TIM2);
		LL_TIM_ClearFlag_TRIG(TIM2);
		LL_TIM_SetSlaveMode(TIM2, LL_TIM_SLAVEMODE_COMBINED_RESETTRIGGER);
		timer2_sync_started = true;
		timer2_sync_acquiring = true;
		return;
	}

	/* The accepted hardware reset establishes the boundary phase.
	 * Close the internal gate before the next PWM event, then arm CH2.
	 */
	LL_TIM_SetSlaveMode(TIM2, LL_TIM_SLAVEMODE_DISABLED);
	LL_TIM_ClearFlag_TRIG(TIM2);
	LL_TIM_EnableIT_CC2(TIM2);
	if (timer2_sync_acquiring)
	{
		LL_TIM_ClearFlag_CC1(TIM2);
		if (timer2_sync_dma_armed)
			LL_TIM_EnableDMAReq_CC1(TIM2);
		if (timer2_sync_irq_armed)
			LL_TIM_EnableIT_CC1(TIM2);
		timer2_sync_dma_armed = false;
		timer2_sync_irq_armed = false;
		timer2_sync_acquiring = false;
	}
}

void timer2_master_sync_event(void)
{
	if (timer2_master_mode)
		timer2_sync_event();
}

void timer2_slave_sync_event(void)
{
	if (timer2_slave_mode)
		timer2_sync_event();
}

void timer2_sync_stop(void)
{
	if (!timer2_sync_enabled())
		return;

	LL_TIM_DisableIT_CC2(TIM2);
	LL_TIM_DisableIT_TRIG(TIM2);
	/* Only the master drives TWIST's shared SYNC line. A stopped slave
	 * must leave SCOUT high impedance, including during reconfiguration.
	 */
	LL_GPIO_SetPinMode(GPIOB, LL_GPIO_PIN_1,
					   timer2_master_mode ? LL_GPIO_MODE_OUTPUT : LL_GPIO_MODE_INPUT);
	LL_TIM_SetSlaveMode(TIM2, LL_TIM_SLAVEMODE_DISABLED);
	LL_TIM_ClearFlag_CC2(TIM2);
	LL_TIM_ClearFlag_TRIG(TIM2);
	LL_TIM_DisableDMAReq_CC1(TIM2);
	LL_TIM_DisableCounter(TIM2);
	timer2_compare_disarm();
	timer2_master_mode = false;
	timer2_slave_mode = false;
	timer2_sync_started = false;
	timer2_sync_acquiring = false;
}

void timer2_master_sync_stop(void)
{
	if (timer2_master_mode)
		timer2_sync_stop();
}

/* Per-timer inits */

void init_timer_2()
{
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2);

	LL_TIM_InitTypeDef TIM_InitStruct = {0};
	/* Same 0.1 microsecond time base as TIM6/TIM7 on SPIN. */
	TIM_InitStruct.Prescaler = (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC/10000000U) - 1U;
	TIM_InitStruct.CounterMode = LL_TIM_COUNTERMODE_UP;
	TIM_InitStruct.Autoreload = UINT32_MAX;
	LL_TIM_Init(TIM2, &TIM_InitStruct);
	LL_TIM_DisableARRPreload(TIM2);
	LL_TIM_SetOnePulseMode(TIM2, LL_TIM_ONEPULSEMODE_SINGLE);
	LL_TIM_OC_SetMode(TIM2, LL_TIM_CHANNEL_CH1, LL_TIM_OCMODE_FROZEN);
	LL_TIM_CC_SetDMAReqTrigger(TIM2, LL_TIM_CCDMAREQUEST_CC);
	LL_TIM_SetTriggerInput(TIM2, LL_TIM_TS_ITR10);
	LL_TIM_SetSlaveMode(TIM2, LL_TIM_SLAVEMODE_DISABLED);
	LL_TIM_DisableIT_UPDATE(TIM2);
	LL_TIM_DisableIT_CC1(TIM2);
	LL_TIM_ClearFlag_UPDATE(TIM2);
}

 void init_timer_3()
 {
	 /* Configure Timer in incremental coder mode */
 
	 /* Peripheral clock enable */
	 LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM3);
 
	 LL_TIM_InitTypeDef TIM_InitStruct = {0};
	 TIM_InitStruct.Prescaler = 0;
	 TIM_InitStruct.CounterMode = LL_TIM_COUNTERMODE_UP;
	 TIM_InitStruct.Autoreload = 65535;
	 TIM_InitStruct.ClockDivision = LL_TIM_CLOCKDIVISION_DIV1;
 
	 LL_TIM_Init(TIM3, &TIM_InitStruct);
	 LL_TIM_EnableARRPreload(TIM3);
	 LL_TIM_SetEncoderMode(TIM3, LL_TIM_ENCODERMODE_X4_TI12);
	 LL_TIM_IC_SetActiveInput(TIM3,
							  LL_TIM_CHANNEL_CH1,
							  LL_TIM_ACTIVEINPUT_DIRECTTI);
 
	 LL_TIM_IC_SetPrescaler(TIM3, LL_TIM_CHANNEL_CH1, LL_TIM_ICPSC_DIV1);
	 LL_TIM_IC_SetFilter(TIM3, LL_TIM_CHANNEL_CH1, LL_TIM_IC_FILTER_FDIV16_N5);
	 LL_TIM_IC_SetPolarity(TIM3, LL_TIM_CHANNEL_CH1, LL_TIM_IC_POLARITY_RISING);
	 LL_TIM_IC_SetActiveInput(TIM3,
							  LL_TIM_CHANNEL_CH2,
							  LL_TIM_ACTIVEINPUT_DIRECTTI);
 
	 LL_TIM_IC_SetPrescaler(TIM3, LL_TIM_CHANNEL_CH2, LL_TIM_ICPSC_DIV1);
	 LL_TIM_IC_SetFilter(TIM3, LL_TIM_CHANNEL_CH2, LL_TIM_IC_FILTER_FDIV1);
	 LL_TIM_IC_SetPolarity(TIM3, LL_TIM_CHANNEL_CH2, LL_TIM_IC_POLARITY_RISING);
	 LL_TIM_SetTriggerOutput(TIM3, LL_TIM_TRGO_RESET);
	 LL_TIM_DisableMasterSlaveMode(TIM3);
	 LL_TIM_ConfigETR(TIM3,
					  LL_TIM_ETR_POLARITY_NONINVERTED,
					  LL_TIM_ETR_PRESCALER_DIV1,
					  LL_TIM_ETR_FILTER_FDIV1);
 
	 LL_TIM_ConfigIDX(
		 TIM3,
		 LL_TIM_INDEX_ALL|LL_TIM_INDEX_POSITION_DOWN_DOWN|LL_TIM_INDEX_UP_DOWN
	 );
 
	 LL_TIM_EnableEncoderIndex(TIM3);
 }
 

void init_timer_4()
{
	/* Configure Timer in incremental coder mode */

	/* Peripheral clock enable */
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM4);

	LL_TIM_InitTypeDef TIM_InitStruct = {0};
	TIM_InitStruct.Prescaler = 0;
	TIM_InitStruct.CounterMode = LL_TIM_COUNTERMODE_UP;
	TIM_InitStruct.Autoreload = 65535;
	TIM_InitStruct.ClockDivision = LL_TIM_CLOCKDIVISION_DIV1;

	LL_TIM_Init(TIM4, &TIM_InitStruct);
	LL_TIM_EnableARRPreload(TIM4);
	LL_TIM_SetEncoderMode(TIM4, LL_TIM_ENCODERMODE_X4_TI12);
	LL_TIM_IC_SetActiveInput(TIM4,
							 LL_TIM_CHANNEL_CH1,
							 LL_TIM_ACTIVEINPUT_DIRECTTI);

	LL_TIM_IC_SetPrescaler(TIM4, LL_TIM_CHANNEL_CH1, LL_TIM_ICPSC_DIV1);
	LL_TIM_IC_SetFilter(TIM4, LL_TIM_CHANNEL_CH1, LL_TIM_IC_FILTER_FDIV16_N5);
	LL_TIM_IC_SetPolarity(TIM4, LL_TIM_CHANNEL_CH1, LL_TIM_IC_POLARITY_RISING);
	LL_TIM_IC_SetActiveInput(TIM4,
							 LL_TIM_CHANNEL_CH2,
							 LL_TIM_ACTIVEINPUT_DIRECTTI);

	LL_TIM_IC_SetPrescaler(TIM4, LL_TIM_CHANNEL_CH2, LL_TIM_ICPSC_DIV1);
	LL_TIM_IC_SetFilter(TIM4, LL_TIM_CHANNEL_CH2, LL_TIM_IC_FILTER_FDIV1);
	LL_TIM_IC_SetPolarity(TIM4, LL_TIM_CHANNEL_CH2, LL_TIM_IC_POLARITY_RISING);
	LL_TIM_SetTriggerOutput(TIM4, LL_TIM_TRGO_RESET);
	LL_TIM_DisableMasterSlaveMode(TIM4);
	LL_TIM_ConfigETR(TIM4,
					 LL_TIM_ETR_POLARITY_NONINVERTED,
					 LL_TIM_ETR_PRESCALER_DIV1,
					 LL_TIM_ETR_FILTER_FDIV1);

	LL_TIM_ConfigIDX(
		TIM4,
		LL_TIM_INDEX_ALL|LL_TIM_INDEX_POSITION_DOWN_DOWN|LL_TIM_INDEX_UP_DOWN
	);

	LL_TIM_EnableEncoderIndex(TIM4);
}

void init_timer_6()
{
	LL_TIM_InitTypeDef TIM_InitStruct = {0};

	/* Peripheral clock enable */
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);

	/* TIM6 interrupt Init */

	/* Set prescaler to tick to 0.1µs */
	TIM_InitStruct.Prescaler = (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC/1e7) - 1;
	TIM_InitStruct.CounterMode = LL_TIM_COUNTERMODE_UP;
	LL_TIM_Init(TIM6, &TIM_InitStruct);
	LL_TIM_DisableARRPreload(TIM6);
	LL_TIM_SetTriggerOutput(TIM6, LL_TIM_TRGO_RESET);
	LL_TIM_DisableMasterSlaveMode(TIM6);
}

void init_timer_7()
{
	LL_TIM_InitTypeDef TIM_InitStruct = {0};

	/* Peripheral clock enable */
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM7);

	/* TIM7 interrupt Init */

	/* Set prescaler to tick to 0.1µs */
	TIM_InitStruct.Prescaler = (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC/1e7) - 1;
	TIM_InitStruct.CounterMode = LL_TIM_COUNTERMODE_UP;
	LL_TIM_Init(TIM7, &TIM_InitStruct);
	LL_TIM_DisableARRPreload(TIM7);
	LL_TIM_SetTriggerOutput(TIM7, LL_TIM_TRGO_RESET);
	LL_TIM_DisableMasterSlaveMode(TIM7);
}

/* Device definitions */

/* Timer 2 */
#if DT_NODE_HAS_STATUS(TIMER2_NODE, okay)

struct stm32_timer_driver_data timer2_data =
{
	.timer_struct       = TIM2,
	.interrupt_line     = TIMER2_INTERRUPT_LINE,
	.interrupt_prio     = TIMER2_INTERRUPT_PRIO,
	.timer_mode         = unconfigured,
	.timer_irq_callback = NULL
};

DEVICE_DT_DEFINE(TIMER2_NODE,
                 timer_stm32_init,
                 NULL,
                 &timer2_data,
                 NULL,
                 PRE_KERNEL_1,
                 CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
                 &timer_funcs
                );

#endif /* Timer 2 */

/* Timer4 */
#if DT_NODE_HAS_STATUS(TIMER3_NODE, okay)

struct stm32_timer_driver_data timer3_data =
{
	.timer_struct       = TIM3,
	.interrupt_line     = TIMER3_INTERRUPT_LINE,
	.interrupt_prio     = TIMER3_INTERRUPT_PRIO,
	.timer_irq_callback = NULL
};

DEVICE_DT_DEFINE(TIMER3_NODE,
                 timer_stm32_init,
                 NULL,
                 &timer3_data,
                 NULL,
                 PRE_KERNEL_1,
                 CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
                 &timer_funcs
                );

#endif /* Timer 3 */


/* Timer4 */
#if DT_NODE_HAS_STATUS(TIMER4_NODE, okay)

/** @brief Structure that holds the timer 4 data  */
struct stm32_timer_driver_data timer4_data =
{
	.timer_struct       = TIM4,
	.interrupt_line     = TIMER4_INTERRUPT_LINE,
	.interrupt_prio     = TIMER4_INTERRUPT_PRIO,
	.timer_irq_callback = NULL
};

/**
 * @brief Define and initialize the STM32 timer device for TIM4.
 *
 * This macro registers the TIM4 timer as a Zephyr device using the device
 * tree node specified by `TIMER4_NODE`.
 *
 * - Registers the device with the STM32 timer driver init function.
 *
 * - Associates static device data with the driver.
 *
 * - Uses `PRE_KERNEL_1` initialization level with a configurable priority.
 *
 * - Provides the function pointer table `timer_funcs` for driver API access.
 *
 */
DEVICE_DT_DEFINE(TIMER4_NODE,
                 timer_stm32_init,
                 NULL,
                 &timer4_data,
                 NULL,
                 PRE_KERNEL_1,
                 CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
                 &timer_funcs
                );

#endif /* Timer 4 */

/* Timer 6 */
#if DT_NODE_HAS_STATUS(TIMER6_NODE, okay)

/** @brief Structure that holds the timer 7 data  */
struct stm32_timer_driver_data timer6_data =
{
	.timer_struct       = TIM6,
	.interrupt_line     = TIMER6_INTERRUPT_LINE,
	.interrupt_prio     = TIMER6_INTERRUPT_PRIO,
	.timer_irq_callback = NULL
};


/**
 * @brief Define and initialize the STM32 timer device for TIM6.
 *
 * This macro registers the TIM6 timer as a Zephyr device using the device
 * tree node specified by `TIMER6_NODE`.
 *
 * - Registers the device with the STM32 timer driver init function.
 *
 * - Associates static device data with the driver.
 *
 * - Uses `PRE_KERNEL_1` initialization level with a configurable priority.
 *
 * - Provides the function pointer table `timer_funcs` for driver API access.
 *
 */
DEVICE_DT_DEFINE(TIMER6_NODE,
                 timer_stm32_init,
                 NULL,
                 &timer6_data,
                 NULL,
                 PRE_KERNEL_1,
                 CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
                 &timer_funcs
                );

#endif /* Timer 6 */

/* Timer 7 */
#if DT_NODE_HAS_STATUS(TIMER7_NODE, okay)

/** @brief Structure that holds the timer 7 data  */
struct stm32_timer_driver_data timer7_data =
{
	.timer_struct       = TIM7,
	.interrupt_line     = TIMER7_INTERRUPT_LINE,
	.interrupt_prio     = TIMER7_INTERRUPT_PRIO,
	.timer_irq_callback = NULL
};

/**
 * @brief Define and initialize the STM32 timer device for TIM7.
 *
 * This macro registers the TIM7 timer as a Zephyr device using the device
 * tree node specified by `TIMER7_NODE`.
 *
 * - Registers the device with the STM32 timer driver init function.
 *
 * - Associates static device data with the driver.
 *
 * - Uses `PRE_KERNEL_1` initialization level with a configurable priority.
 *
 * - Provides the function pointer table `timer_funcs` for driver API access.
 *
 */
DEVICE_DT_DEFINE(TIMER7_NODE,
                 timer_stm32_init,
                 NULL,
                 &timer7_data,
                 NULL,
                 PRE_KERNEL_1,
                 CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
                 &timer_funcs
                );

#endif /* Timer 7 */
