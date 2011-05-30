/**
 ******************************************************************************
 * @addtogroup PIOS PIOS Core hardware abstraction layer
 * @{
 * @addtogroup PIOS_BMA180 BMA180 Functions
 * @brief Deals with the hardware interface to the BMA180 3-axis accelerometer
 * @{
 *
 * @file       pios_bma180.h
 * @author     David "Buzz" Carlson (buzz@chebuzz.com)
 * 				The OpenPilot Team, http://www.openpilot.org Copyright (C) 2011.
 * @brief      PiOS BMA180 digital accelerometer driver.
 *                 - Driver for the BMA180 digital accelerometer on the SPI bus.
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
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

#include "pios.h"
#include "fifo_buffer.h"

static uint32_t PIOS_SPI_ACCEL;

static int32_t PIOS_BMA180_GetReg(uint8_t reg);
static int32_t PIOS_BMA180_SetReg(uint8_t reg, uint8_t data);
static int32_t PIOS_BMA180_SelectBW(enum bma180_bandwidth bw);
static int32_t PIOS_BMA180_SetRange(enum bma180_range range);
static int32_t PIOS_BMA180_Config();
static int32_t PIOS_BMA180_EnableIrq();

volatile bool pios_bma180_data_ready = false;

#define PIOS_BMA180_MAX_DOWNSAMPLE 10
static int16_t pios_bma180_buffer[PIOS_BMA180_MAX_DOWNSAMPLE * 3];
static t_fifo_buffer pios_bma180_fifo;

/**
 * @brief Initialize with good default settings
 */
void PIOS_BMA180_Init()
{
	GPIO_InitTypeDef GPIO_InitStructure;
	EXTI_InitTypeDef EXTI_InitStructure;
	NVIC_InitTypeDef NVIC_InitStructure;
	
	/* Enable DRDY GPIO clock */
	RCC_APB2PeriphClockCmd(PIOS_BMA180_DRDY_CLK | RCC_APB2Periph_AFIO, ENABLE);
	
	/* Configure EOC pin as input floating */
	GPIO_InitStructure.GPIO_Pin = PIOS_BMA180_DRDY_GPIO_PIN;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
	GPIO_Init(PIOS_BMA180_DRDY_GPIO_PORT, &GPIO_InitStructure);
	
	/* Configure the End Of Conversion (EOC) interrupt */
	GPIO_EXTILineConfig(PIOS_BMA180_DRDY_PORT_SOURCE, PIOS_BMA180_DRDY_PIN_SOURCE);
	EXTI_InitStructure.EXTI_Line = PIOS_BMA180_DRDY_EXTI_LINE;
	EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
	EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
	EXTI_InitStructure.EXTI_LineCmd = ENABLE;
	EXTI_Init(&EXTI_InitStructure);
	
	/* Enable and set EOC EXTI Interrupt to the lowest priority */
	NVIC_InitStructure.NVIC_IRQChannel = PIOS_BMA180_DRDY_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = PIOS_BMA180_DRDY_PRIO;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	NVIC_Init(&NVIC_InitStructure);
	
	pios_bma180_data_ready = false;
	
	fifoBuf_init(&pios_bma180_fifo, (uint8_t *) pios_bma180_buffer, sizeof(pios_bma180_buffer));
	
	PIOS_BMA180_Config();	
	PIOS_BMA180_SelectBW(BMA_BW_600HZ);
	PIOS_BMA180_SetRange(BMA_RANGE_8G);
	PIOS_DELAY_WaituS(50);
	PIOS_BMA180_EnableIrq();
}

/**
 * @brief Claim the SPI bus for the accel communications and select this chip
 * @return 0 if successful, -1 if unable to claim bus
 */
int32_t PIOS_BMA180_ClaimBus()
{
	if(PIOS_SPI_ClaimBus(PIOS_SPI_ACCEL) != 0)
		return -1;
	PIOS_BMA180_ENABLE;
	return 0;
}

/**
 * @brief Release the SPI bus for the accel communications and end the transaction
 * @return 0 if successful
 */
int32_t PIOS_BMA180_ReleaseBus()
{
	PIOS_BMA180_DISABLE;
	return PIOS_SPI_ReleaseBus(PIOS_SPI_ACCEL);
}

/**
 * @brief Read a register from BMA180
 * @returns The register value or -1 if failure to get bus
 * @param reg[in] Register address to be read
 */
int32_t PIOS_BMA180_GetReg(uint8_t reg)
{
	uint8_t data;
	
	if(PIOS_BMA180_ClaimBus() != 0)
		return -1;	

	PIOS_SPI_TransferByte(PIOS_SPI_ACCEL,(0x80 | reg) ); // request byte
	data = PIOS_SPI_TransferByte(PIOS_SPI_ACCEL,0 );     // receive response
	
	PIOS_BMA180_ReleaseBus();
	return data;
}

/**
 * @brief Write a BMA180 register.  EEPROM must be unlocked before calling this function.
 * @return none
 * @param reg[in] address of register to be written
 * @param data[in] data that is to be written to register
 */
int32_t PIOS_BMA180_SetReg(uint8_t reg, uint8_t data)
{
	if(PIOS_BMA180_ClaimBus() != 0)
		return -1;
	
	PIOS_SPI_TransferByte(PIOS_SPI_ACCEL, 0x7f & reg);
	PIOS_SPI_TransferByte(PIOS_SPI_ACCEL, data);

	PIOS_BMA180_ReleaseBus();
	
	return 0;
}


static int32_t PIOS_BMA180_EnableEeprom() {
	// Enable EEPROM writing
	int32_t byte = PIOS_BMA180_GetReg(BMA_CTRREG0);
	if(byte < 0)
		return -1;
	byte |= 0x10;                                      // Set bit 4
	if(PIOS_BMA180_SetReg(BMA_CTRREG0,(uint8_t) byte) < 0)    // Have to set ee_w to		
		return -1;
	return 0;
}

static int32_t PIOS_BMA180_DisableEeprom() {
	// Enable EEPROM writing
	int32_t byte = PIOS_BMA180_GetReg(BMA_CTRREG0);
	if(byte < 0)
		return -1;
	byte |= 0x10;                                      // Set bit 4
	if(PIOS_BMA180_SetReg(BMA_CTRREG0,(uint8_t) byte) < 0)    // Have to set ee_w to		
		return -1;
	return 0;
}

/**
 * @brief Set the default register settings
 * @return 0 if successful, -1 if not
 */
static int32_t PIOS_BMA180_Config() 
{
	/*
	0x35 = 0x81  //smp-skip = 1 for less interrupts
	0x33 = 0x81  //shadow-dis = 1, update MSB and LSB synchronously
	0x27 = 0x01  //dis-i2c
	0x21 = 0x02  //new_data_int = 1
	 */
		
	if(PIOS_BMA180_SetReg(BMA_OFFSET_LSB1, 0x81) < 0)
		return -1;
	if(PIOS_BMA180_SetReg(BMA_GAIN_Y, 0x81) < 0)
		return -1;
	if(PIOS_BMA180_SetReg(BMA_CTRREG3, 0xFF) < 0)
		return -1;
	
	return 0;
}

/**
 * @brief Select the bandwidth the digital filter pass allows.
 * @return 0 if successful, -1 if not
 * @param rate[in] Bandwidth setting to be used
 * 
 * EEPROM must be write-enabled before calling this function.
 */
static int32_t PIOS_BMA180_SelectBW(enum bma180_bandwidth bw)
{
	uint8_t reg;
	reg = PIOS_BMA180_GetReg(BMA_BW_ADDR);
	reg = (reg & ~BMA_BW_MASK) | ((bw << BMA_BW_SHIFT) & BMA_BW_MASK);
	return PIOS_BMA180_SetReg(BMA_BW_ADDR, reg);
}

/**
 * @brief Select the full scale acceleration range.
 * @return 0 if successful, -1 if not
 * @param rate[in] Range setting to be used
 *
 */
static int32_t PIOS_BMA180_SetRange(enum bma180_range range) 
{
	uint8_t reg;
	reg = PIOS_BMA180_GetReg(BMA_RANGE_ADDR);
	reg = (reg & ~BMA_RANGE_MASK) | ((range << BMA_RANGE_SHIFT) & BMA_RANGE_MASK);
	return PIOS_BMA180_SetReg(BMA_RANGE_ADDR, reg);
}

static int32_t PIOS_BMA180_EnableIrq() 
{

	if(PIOS_BMA180_EnableEeprom() < 0)
		return -1;

	if(PIOS_BMA180_SetReg(BMA_CTRREG3, BMA_NEW_DAT_INT) < 0)
		return -1;

	if(PIOS_BMA180_DisableEeprom() < 0)
		return -1;

	return 0;
}

/**
 * @brief Connect to the correct SPI bus
 */
void PIOS_BMA180_Attach(uint32_t spi_id)
{
	PIOS_SPI_ACCEL = spi_id;
}

/**
 * @brief Read a single set of values from the x y z channels
 * @param[out] data Int16 array of (x,y,z) sensor values
 * @returns 0 if successful
 * @retval -1 unable to claim bus
 * @retval -2 unable to transfer data
 */
int32_t PIOS_BMA180_ReadAccels(int16_t * data)
{
	// To save memory use same buffer for in and out but offset by
	// a byte
	uint8_t buf[7] = {BMA_X_LSB_ADDR | 0x80,0,0,0,0,0};
	uint8_t rec[7] = {0,0,0,0,0,0};
	
	if(PIOS_BMA180_ClaimBus() != 0)
		return -1;
	if(PIOS_SPI_TransferBlock(PIOS_SPI_ACCEL,&buf[0],&rec[0],7,NULL) != 0)
		return -2;
	PIOS_BMA180_ReleaseBus();	
	
	//        |    MSB        |   LSB       | 0 | new_data |
	data[0] = (rec[2] << 8) | rec[1];
	data[1] = (rec[4] << 8) | rec[3];
	data[2] = (rec[6] << 8) | rec[5];
	data[0] /= 4;
	data[1] /= 4;
	data[2] /= 4;
	
	return 0; // return number of remaining entries
}

/**
 * @brief Returns the scale the BMA180 chip is using
 * @return Scale (m / s^2) / LSB
 */
float PIOS_BMA180_GetScale()
{
	return 9.81 / 1024;
}

t_fifo_buffer * PIOS_BMA180_GetFifo()
{
	return &pios_bma180_fifo;
}


/**
 * @brief Test SPI and chip functionality by reading chip ID register
 * @return 0 if success, -1 if failure.
 *
 */
int32_t PIOS_BMA180_Test()
{
	// Read chip ID then version ID
	uint8_t buf[3] = {0x80 | BMA_CHIPID_ADDR, 0, 0};
	uint8_t rec[3] = {0,0, 0};
	int32_t retval;

	if(PIOS_BMA180_ClaimBus() != 0)
		return -1;
	retval = PIOS_SPI_TransferBlock(PIOS_SPI_ACCEL,&buf[0],&rec[0],sizeof(buf),NULL);
	PIOS_BMA180_ReleaseBus();
	
	if(retval != 0)
		return -2;
	
	int16_t data[3];
	if(PIOS_BMA180_ReadAccels(data) != 0)
		return -3;
	
	if(rec[1] != 0x3)
		return -4;
	
	if(rec[2] < 0x12)
		return -5;

	return 0;
}

uint32_t pios_bma180_count = 0;
/**
 * @brief IRQ Handler
 */
void PIOS_BMA180_IRQHandler(void)
{
	int16_t accels[3];
	pios_bma180_count++;
	
	if(PIOS_BMA180_ReadAccels(accels) < 0)
		return;
	if(fifoBuf_getFree(&pios_bma180_fifo) < sizeof(accels))
		return;
	
	fifoBuf_putData(&pios_bma180_fifo, accels, sizeof(accels));
}

/**
 * @}
 * @}
 */
