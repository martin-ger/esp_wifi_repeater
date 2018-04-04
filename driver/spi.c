/*
* The MIT License (MIT)
* 
* Copyright (c) 2015 David Ogilvy (MetalPhreak)
* 
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
* 
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
* 
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/


#include "driver/spi.h"


////////////////////////////////////////////////////////////////////////////////
//
// Function Name: spi_init
//   Description: Wrapper to setup HSPI/SPI GPIO pins and default SPI clock
//    Parameters: spi_no - SPI (0) or HSPI (1)
//				 
////////////////////////////////////////////////////////////////////////////////

void spi_init(uint8 spi_no){
	
	if(spi_no > 1) return; //Only SPI and HSPI are valid spi modules. 

	spi_init_gpio(spi_no, SPI_CLK_USE_DIV);
	spi_clock(spi_no, SPI_CLK_PREDIV, SPI_CLK_CNTDIV);
	spi_tx_byte_order(spi_no, SPI_BYTE_ORDER_HIGH_TO_LOW);
	spi_rx_byte_order(spi_no, SPI_BYTE_ORDER_HIGH_TO_LOW); 

	SET_PERI_REG_MASK(SPI_USER(spi_no), SPI_CS_SETUP|SPI_CS_HOLD);
	CLEAR_PERI_REG_MASK(SPI_USER(spi_no), SPI_FLASH_MODE);

}

////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
//
// Function Name: spi_mode
//   Description: Configures SPI mode parameters for clock edge and clock polarity.
//    Parameters: spi_no - SPI (0) or HSPI (1)
//				  spi_cpha - (0) Data is valid on clock leading edge
//				             (1) Data is valid on clock trailing edge
//				  spi_cpol - (0) Clock is low when inactive
//				             (1) Clock is high when inactive
//
////////////////////////////////////////////////////////////////////////////////

void spi_mode(uint8 spi_no, uint8 spi_cpha,uint8 spi_cpol){
	if(!spi_cpha == !spi_cpol) {
		CLEAR_PERI_REG_MASK(SPI_USER(spi_no), SPI_CK_OUT_EDGE);
	} else {
		SET_PERI_REG_MASK(SPI_USER(spi_no), SPI_CK_OUT_EDGE);
	}

	if (spi_cpol) {
		SET_PERI_REG_MASK(SPI_PIN(spi_no), SPI_IDLE_EDGE);
	} else {
		CLEAR_PERI_REG_MASK(SPI_PIN(spi_no), SPI_IDLE_EDGE);
	}
}


////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
//
// Function Name: spi_init_gpio
//   Description: Initialises the GPIO pins for use as SPI pins.
//    Parameters: spi_no - SPI (0) or HSPI (1)
//				  sysclk_as_spiclk - SPI_CLK_80MHZ_NODIV (1) if using 80MHz
//									 sysclock for SPI clock. 
//									 SPI_CLK_USE_DIV (0) if using divider to
//									 get lower SPI clock speed.
//				 
////////////////////////////////////////////////////////////////////////////////

void spi_init_gpio(uint8 spi_no, uint8 sysclk_as_spiclk){

//	if(spi_no > 1) return; //Not required. Valid spi_no is checked with if/elif below.

	uint32 clock_div_flag = 0;
	if(sysclk_as_spiclk){
		clock_div_flag = 0x0001;	
	} 

	if(spi_no==SPI){
		WRITE_PERI_REG(PERIPHS_IO_MUX, 0x005|(clock_div_flag<<8)); //Set bit 8 if 80MHz sysclock required
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_SD_CLK_U, 1);
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_SD_CMD_U, 1);
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_SD_DATA0_U, 1);	
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_SD_DATA1_U, 1);	
	}else if(spi_no==HSPI){
		WRITE_PERI_REG(PERIPHS_IO_MUX, 0x105|(clock_div_flag<<9)); //Set bit 9 if 80MHz sysclock required
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDI_U, 2); //GPIO12 is HSPI MISO pin (Master Data In)
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, 2); //GPIO13 is HSPI MOSI pin (Master Data Out)
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTMS_U, 2); //GPIO14 is HSPI CLK pin (Clock)
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, 2); //GPIO15 is HSPI CS pin (Chip Select / Slave Select)
	}

}

////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
//
// Function Name: spi_clock
//   Description: sets up the control registers for the SPI clock
//    Parameters: spi_no - SPI (0) or HSPI (1)
//				  prediv - predivider value (actual division value)
//				  cntdiv - postdivider value (actual division value)
//				  Set either divider to 0 to disable all division (80MHz sysclock)
//				 
////////////////////////////////////////////////////////////////////////////////

void spi_clock(uint8 spi_no, uint16 prediv, uint8 cntdiv){
	
	if(spi_no > 1) return;

	if((prediv==0)|(cntdiv==0)){

		WRITE_PERI_REG(SPI_CLOCK(spi_no), SPI_CLK_EQU_SYSCLK);

	} else {
	
		WRITE_PERI_REG(SPI_CLOCK(spi_no), 
					(((prediv-1)&SPI_CLKDIV_PRE)<<SPI_CLKDIV_PRE_S)|
					(((cntdiv-1)&SPI_CLKCNT_N)<<SPI_CLKCNT_N_S)|
					(((cntdiv>>1)&SPI_CLKCNT_H)<<SPI_CLKCNT_H_S)|
					((0&SPI_CLKCNT_L)<<SPI_CLKCNT_L_S));
	}

}

////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
//
// Function Name: spi_tx_byte_order
//   Description: Setup the byte order for shifting data out of buffer
//    Parameters: spi_no - SPI (0) or HSPI (1)
//				  byte_order - SPI_BYTE_ORDER_HIGH_TO_LOW (1) 
//							   Data is sent out starting with Bit31 and down to Bit0
//
//							   SPI_BYTE_ORDER_LOW_TO_HIGH (0)
//							   Data is sent out starting with the lowest BYTE, from 
//							   MSB to LSB, followed by the second lowest BYTE, from
//							   MSB to LSB, followed by the second highest BYTE, from
//							   MSB to LSB, followed by the highest BYTE, from MSB to LSB
//							   0xABCDEFGH would be sent as 0xGHEFCDAB
//
//				 
////////////////////////////////////////////////////////////////////////////////

void spi_tx_byte_order(uint8 spi_no, uint8 byte_order){

	if(spi_no > 1) return;

	if(byte_order){
		SET_PERI_REG_MASK(SPI_USER(spi_no), SPI_WR_BYTE_ORDER);
	} else {
		CLEAR_PERI_REG_MASK(SPI_USER(spi_no), SPI_WR_BYTE_ORDER);
	}
}
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
//
// Function Name: spi_rx_byte_order
//   Description: Setup the byte order for shifting data into buffer
//    Parameters: spi_no - SPI (0) or HSPI (1)
//				  byte_order - SPI_BYTE_ORDER_HIGH_TO_LOW (1) 
//							   Data is read in starting with Bit31 and down to Bit0
//
//							   SPI_BYTE_ORDER_LOW_TO_HIGH (0)
//							   Data is read in starting with the lowest BYTE, from 
//							   MSB to LSB, followed by the second lowest BYTE, from
//							   MSB to LSB, followed by the second highest BYTE, from
//							   MSB to LSB, followed by the highest BYTE, from MSB to LSB
//							   0xABCDEFGH would be read as 0xGHEFCDAB
//
//				 
////////////////////////////////////////////////////////////////////////////////

void spi_rx_byte_order(uint8 spi_no, uint8 byte_order){

	if(spi_no > 1) return;

	if(byte_order){
		SET_PERI_REG_MASK(SPI_USER(spi_no), SPI_RD_BYTE_ORDER);
	} else {
		CLEAR_PERI_REG_MASK(SPI_USER(spi_no), SPI_RD_BYTE_ORDER);
	}
}
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
//
// Function Name: spi_transaction
//   Description: SPI transaction function
//    Parameters: spi_no - SPI (0) or HSPI (1)
//				  cmd_bits - actual number of bits to transmit
//				  cmd_data - command data
//				  addr_bits - actual number of bits to transmit
//				  addr_data - address data
//				  dout_bits - actual number of bits to transmit
//				  dout_data - output data
//				  din_bits - actual number of bits to receive
//				  
//		 Returns: read data - uint32 containing read in data only if RX was set 
//				  0 - something went wrong (or actual read data was 0)
//				  1 - data sent ok (or actual read data is 1)
//				  Note: all data is assumed to be stored in the lower bits of
//				  the data variables (for anything <32 bits). 
//
////////////////////////////////////////////////////////////////////////////////

uint32 spi_transaction(uint8 spi_no, uint8 cmd_bits, uint16 cmd_data, uint32 addr_bits, uint32 addr_data, uint32 dout_bits, uint32 dout_data,
				uint32 din_bits, uint32 dummy_bits){

	if(spi_no > 1) return 0;  //Check for a valid SPI 

	//code for custom Chip Select as GPIO PIN here

	while(spi_busy(spi_no)); //wait for SPI to be ready	

//########## Enable SPI Functions ##########//
	//disable MOSI, MISO, ADDR, COMMAND, DUMMY in case previously set.
	CLEAR_PERI_REG_MASK(SPI_USER(spi_no), SPI_USR_MOSI|SPI_USR_MISO|SPI_USR_COMMAND|SPI_USR_ADDR|SPI_USR_DUMMY);

	//enable functions based on number of bits. 0 bits = disabled. 
	//This is rather inefficient but allows for a very generic function.
	//CMD ADDR and MOSI are set below to save on an extra if statement.
//	if(cmd_bits) {SET_PERI_REG_MASK(SPI_USER(spi_no), SPI_USR_COMMAND);}
//	if(addr_bits) {SET_PERI_REG_MASK(SPI_USER(spi_no), SPI_USR_ADDR);}
	if(din_bits) {SET_PERI_REG_MASK(SPI_USER(spi_no), SPI_USR_MISO);}
	if(dummy_bits) {SET_PERI_REG_MASK(SPI_USER(spi_no), SPI_USR_DUMMY);}
//########## END SECTION ##########//

//########## Setup Bitlengths ##########//
	WRITE_PERI_REG(SPI_USER1(spi_no), ((addr_bits-1)&SPI_USR_ADDR_BITLEN)<<SPI_USR_ADDR_BITLEN_S | //Number of bits in Address
									  ((dout_bits-1)&SPI_USR_MOSI_BITLEN)<<SPI_USR_MOSI_BITLEN_S | //Number of bits to Send
									  ((din_bits-1)&SPI_USR_MISO_BITLEN)<<SPI_USR_MISO_BITLEN_S |  //Number of bits to receive
									  ((dummy_bits-1)&SPI_USR_DUMMY_CYCLELEN)<<SPI_USR_DUMMY_CYCLELEN_S); //Number of Dummy bits to insert
//########## END SECTION ##########//

//########## Setup Command Data ##########//
	if(cmd_bits) {
		SET_PERI_REG_MASK(SPI_USER(spi_no), SPI_USR_COMMAND); //enable COMMAND function in SPI module
		uint16 command = cmd_data << (16-cmd_bits); //align command data to high bits
		command = ((command>>8)&0xff) | ((command<<8)&0xff00); //swap byte order
		WRITE_PERI_REG(SPI_USER2(spi_no), ((((cmd_bits-1)&SPI_USR_COMMAND_BITLEN)<<SPI_USR_COMMAND_BITLEN_S) | command&SPI_USR_COMMAND_VALUE));	
	}
//########## END SECTION ##########//

//########## Setup Address Data ##########//
	if(addr_bits){
		SET_PERI_REG_MASK(SPI_USER(spi_no), SPI_USR_ADDR); //enable ADDRess function in SPI module
		WRITE_PERI_REG(SPI_ADDR(spi_no), addr_data<<(32-addr_bits)); //align address data to high bits
	}
	

//########## END SECTION ##########//	

//########## Setup DOUT data ##########//
	if(dout_bits) {
		SET_PERI_REG_MASK(SPI_USER(spi_no), SPI_USR_MOSI); //enable MOSI function in SPI module
	//copy data to W0
	if(READ_PERI_REG(SPI_USER(spi_no))&SPI_WR_BYTE_ORDER) {
		WRITE_PERI_REG(SPI_W0(spi_no), dout_data<<(32-dout_bits));
	} else {

		uint8 dout_extra_bits = dout_bits%8;

		if(dout_extra_bits){
			//if your data isn't a byte multiple (8/16/24/32 bits)and you don't have SPI_WR_BYTE_ORDER set, you need this to move the non-8bit remainder to the MSBs
			//not sure if there's even a use case for this, but it's here if you need it...
			//for example, 0xDA4 12 bits without SPI_WR_BYTE_ORDER would usually be output as if it were 0x0DA4, 
			//of which 0xA4, and then 0x0 would be shifted out (first 8 bits of low byte, then 4 MSB bits of high byte - ie reverse byte order). 
			//The code below shifts it out as 0xA4 followed by 0xD as you might require. 
			WRITE_PERI_REG(SPI_W0(spi_no), ((0xFFFFFFFF<<(dout_bits - dout_extra_bits)&dout_data)<<(8-dout_extra_bits) | (0xFFFFFFFF>>(32-(dout_bits - dout_extra_bits)))&dout_data));
		} else {
			WRITE_PERI_REG(SPI_W0(spi_no), dout_data);
		}
	}
	}
//########## END SECTION ##########//

//########## Begin SPI Transaction ##########//
	SET_PERI_REG_MASK(SPI_CMD(spi_no), SPI_USR);
//########## END SECTION ##########//

//########## Return DIN data ##########//
	if(din_bits) {
		while(spi_busy(spi_no));	//wait for SPI transaction to complete
		
		if(READ_PERI_REG(SPI_USER(spi_no))&SPI_RD_BYTE_ORDER) {
			return READ_PERI_REG(SPI_W0(spi_no)) >> (32-din_bits); //Assuming data in is written to MSB. TBC
		} else {
			return READ_PERI_REG(SPI_W0(spi_no)); //Read in the same way as DOUT is sent. Note existing contents of SPI_W0 remain unless overwritten! 
		}

		return 0; //something went wrong
	}
//########## END SECTION ##########//

	//Transaction completed
	return 1; //success
}

////////////////////////////////////////////////////////////////////////////////

/*///////////////////////////////////////////////////////////////////////////////
//
// Function Name: func
//   Description: 
//    Parameters: 
//				 
////////////////////////////////////////////////////////////////////////////////

void func(params){

}

///////////////////////////////////////////////////////////////////////////////*/


