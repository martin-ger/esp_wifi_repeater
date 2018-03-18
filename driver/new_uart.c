/*
 * File	: uart.c
 * Copyright (C) 2013 - 2016, Espressif Systems
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 3 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "ets_sys.h"
#include "osapi.h"
#include "driver/uart.h"
#include "osapi.h"
#include "driver/uart_register.h"
#include "mem.h"
#include "os_type.h"

#ifdef _ENABLE_RING_BUFFER
    static ringbuf_t rxBuff;
    static ringbuf_t txBuff;
#endif

#ifdef _ENABLE_CONSOLE_INTEGRATION
    uint8_t linked_to_console = 0;
#endif

extern UartDevice    UartDev;

/* Local variables */
static uint8 uart_recvTaskPrio = 0;

/* Internal Functions */
static  void ICACHE_FLASH_ATTR uart_config(uint8 uart_no);
static  void uart0_rx_intr_handler(void *para);

/* Public APIs */
void ICACHE_FLASH_ATTR UART_init(UartBautRate uart0_br, UartBautRate uart1_br, uint8 recv_task_priority);

#define ENABLE_DEBUG

#ifdef ENABLE_DEBUG
    #define DBG
    #define DBG1 os_printf
    #define DBG2 os_printf
#else
    #define DBG
    #define DBG1
    #define DBG2
#endif

#if _ENABLE_CONSOLE_INTEGRATION == 1
void ICACHE_FLASH_ATTR UART_init_console(UartBautRate uart0_br,
                                         uint8 recv_task_priority,
                                         ringbuf_t rxbuffer,
                                         ringbuf_t txBuffer)
{
    /* Set the task which should receive the signals once the data is received */
    uart_recvTaskPrio = recv_task_priority;

    UartDev.baut_rate = uart0_br;
    rxBuff = rxbuffer;
    txBuff = txBuffer;
    linked_to_console = 1;

    uart_config(UART0);
    UART_SetPrintPort(UART0);

    UartDev.baut_rate = uart0_br;
    uart_config(UART1);

    ETS_UART_INTR_ENABLE();
}
#endif

void ICACHE_FLASH_ATTR UART_init(UartBautRate uart0_br, UartBautRate uart1_br, uint8 recv_task_priority)
{
    /* Set the task which should receive the signals once the data is received */
    uart_recvTaskPrio = recv_task_priority;

    UartDev.baut_rate = uart0_br;
    uart_config(UART0);

    UART_SetPrintPort(UART0);

    UartDev.baut_rate = uart1_br;
    uart_config(UART1);

    ETS_UART_INTR_ENABLE();

    #if _ENABLE_CONSOLE_INTEGRATION == 0
        #if _ENABLE_RING_BUFFER == 1
            rxBuff = ringbuf_new(RX_RING_BUFFER_SIZE);
        #endif
    #endif
}



/******************************************************************************
 * FunctionName : uart_config
 * Description  : Internal used function
 *                UART0 used for data TX/RX, RX buffer size is 0x100, interrupt enabled
 *                UART1 just used for debug output
 * Parameters   : uart_no, use UART0 or UART1 defined ahead
 * Returns      : NONE
*******************************************************************************/
LOCAL void ICACHE_FLASH_ATTR uart_config(uint8 uart_no)
{
    if (uart_no == UART1)
    {
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_U1TXD_BK);
    }
    else
    {
        /* rcv_buff size if 0x100 */
        ETS_UART_INTR_ATTACH(uart0_rx_intr_handler,  &(UartDev.rcv_buff));
        PIN_PULLUP_DIS(PERIPHS_IO_MUX_U0TXD_U);
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_U0TXD);

        #if UART_HW_RTS
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, FUNC_U0RTS);   //HW FLOW CONTROL RTS PIN
        #endif

        #if UART_HW_CTS
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, FUNC_U0CTS);   //HW FLOW CONTROL CTS PIN
        #endif
    }
    uart_div_modify(uart_no, UART_CLK_FREQ / (UartDev.baut_rate));//SET BAUDRATE

    WRITE_PERI_REG(UART_CONF0(uart_no), ((UartDev.exist_parity & UART_PARITY_EN_M)  <<  UART_PARITY_EN_S) //SET BIT AND PARITY MODE
                                                                        | ((UartDev.parity & UART_PARITY_M)  <<UART_PARITY_S )
                                                                        | ((UartDev.stop_bits & UART_STOP_BIT_NUM) << UART_STOP_BIT_NUM_S)
                                                                        | ((UartDev.data_bits & UART_BIT_NUM) << UART_BIT_NUM_S));

    //clear rx and tx fifo,not ready
    SET_PERI_REG_MASK(UART_CONF0(uart_no), UART_RXFIFO_RST | UART_TXFIFO_RST);    //RESET FIFO
    CLEAR_PERI_REG_MASK(UART_CONF0(uart_no), UART_RXFIFO_RST | UART_TXFIFO_RST);

    if (uart_no == UART0)
    {
        int rx_threshold = 10;

        #if _ENABLE_CONSOLE_INTEGRATION == 1
            rx_threshold = 1;
        #endif

        //set rx fifo trigger
        WRITE_PERI_REG(UART_CONF1(uart_no),
                        ((rx_threshold & UART_RXFIFO_FULL_THRHD) << UART_RXFIFO_FULL_THRHD_S) |
                        #if UART_HW_RTS
                        ((110 & UART_RX_FLOW_THRHD) << UART_RX_FLOW_THRHD_S) |
                        UART_RX_FLOW_EN |   //enbale rx flow control
                        #endif
//                        (0x02 & UART_RX_TOUT_THRHD) << UART_RX_TOUT_THRHD_S |
//                        UART_RX_TOUT_EN|
                        ((0x10 & UART_TXFIFO_EMPTY_THRHD)<<UART_TXFIFO_EMPTY_THRHD_S));//wjl
                        #if UART_HW_CTS
                        SET_PERI_REG_MASK( UART_CONF0(uart_no),UART_TX_FLOW_EN);  //add this sentense to add a tx flow control via MTCK( CTS )
                        #endif
        SET_PERI_REG_MASK(UART_INT_ENA(uart_no), UART_RXFIFO_TOUT_INT_ENA |UART_FRM_ERR_INT_ENA);
    }
    else
    {
        WRITE_PERI_REG(UART_CONF1(uart_no),((UartDev.rcv_buff.TrigLvl & UART_RXFIFO_FULL_THRHD) << UART_RXFIFO_FULL_THRHD_S));//TrigLvl default val == 1
    }
    //clear all interrupt
    WRITE_PERI_REG(UART_INT_CLR(uart_no), 0xffff);

    //enable rx_interrupt
    SET_PERI_REG_MASK(UART_INT_ENA(uart_no), UART_RXFIFO_FULL_INT_ENA|UART_RXFIFO_OVF_INT_ENA);
}

/******************************************************************************
 * FunctionName : UART_Recv
 * Description  : Public API, unloads the contents of the Rx FIFO for specified
 *                uart into the buffer specified. Will unload upto buffer size
 *                only
 * Parameters   :   IN      uart number (uart_no)
 *                  IN/OUT  char *buffer
 *                  IN      int max_buf_len
 * Returns      : int (number of bytes unloaded ) 0 if FIFO is empty
*******************************************************************************/
int UART_Recv(uint8 uart_no, char *buffer, int max_buf_len)
{
    uint8 max_unload, index = -1;

    #if _ENABLE_RING_BUFFER == 1
    /* If the ring buffer is enabled, then unload from Rx Ring Buffer */
    uint8 bytes_ringbuffer = ringbuf_bytes_used(rxBuff);

    //ring_buffer_dump(rxBuff);
    if (bytes_ringbuffer)
    {
        max_unload = (bytes_ringbuffer<max_buf_len ? bytes_ringbuffer: max_buf_len);

	ringbuf_memcpy_from(buffer, rxBuff, max_unload);
    }
    #else
    /* If the ring buffer is not enabled, then unload from Rx FIFO */
    uint8 fifo_len = (READ_PERI_REG(UART_STATUS(uart_no))>>UART_RXFIFO_CNT_S)&UART_RXFIFO_CNT;

    if (fifo_len)
    {
        max_unload = (fifo_len<max_buf_len ? fifo_len : max_buf_len);
        DBG1("Rx Fifo contains %d characters have to unload %d\r\n", fifo_len , max_unload);

        for (index=0;index<max_unload; index++)
        {
            *(buffer+index) = READ_PERI_REG(UART_FIFO(UART0)) & 0xFF;
        }
        //return index;
        WRITE_PERI_REG(UART_INT_CLR(uart_no), UART_RXFIFO_FULL_INT_CLR);
        uart_rx_intr_enable(uart_no);
    }
    #endif

    return index;
}

int UART_Send(uint8 uart_no, char *buffer, int len)
{
    int     index = 0;
    char    ch ;

    //DBG1("Sending: %s\n", buffer);
    for (index=0; index <len; index ++)
    {
        ch = *(buffer+index);
        uart_tx_one_char(uart_no, ch);
    }

    return index;
}

/*---------------------------------------------------------------------------*
 *                          Internal Functions
 *---------------------------------------------------------------------------*/
/******************************************************************************
 * FunctionName : uart_rx_intr_disable
 * Description  : Internal used function disables the uart interrupts
 * Parameters   : IN uart number (uart_no)
 * Returns      : NONE
*******************************************************************************/
void uart_rx_intr_disable(uint8 uart_no)
{
   //DBG1("RxIntr Disabled\r\n");
#if 1
    CLEAR_PERI_REG_MASK(UART_INT_ENA(uart_no), UART_RXFIFO_FULL_INT_ENA|UART_RXFIFO_TOUT_INT_ENA);
#else
    ETS_UART_INTR_DISABLE();
#endif
}

/******************************************************************************
 * FunctionName : uart_rx_intr_enable
 * Description  : Internal used function enables the uart interrupts
 * Parameters   : IN uart number (uart_no)
 * Returns      : NONE
*******************************************************************************/
void uart_rx_intr_enable(uint8 uart_no)
{
   //DBG1("RxIntr Enabled\r\n");
#if 1
    SET_PERI_REG_MASK(UART_INT_ENA(uart_no), UART_RXFIFO_FULL_INT_ENA|UART_RXFIFO_TOUT_INT_ENA);
#else
    ETS_UART_INTR_ENABLE();
#endif
}

/******************************************************************************
 * FunctionName : uart0_rx_intr_handler
 * Description  : Internal used function
 *                UART0 interrupt handler, add self handle code inside
 * Parameters   : void *para - point to ETS_UART_INTR_ATTACH's arg
 * Returns      : NONE
*******************************************************************************/
static void uart0_rx_intr_handler(void *para)
{
    uint8 uart_no = UART0;//UartDev.buff_uart_no;

	/* Is the frame Error interrupt set ? */
    if(UART_FRM_ERR_INT_ST == (READ_PERI_REG(UART_INT_ST(uart_no)) & UART_FRM_ERR_INT_ST))
    {
        /* The Frame Error  (UART_FRM_ERR_INT_ST) has bee set */
        DBG1("Frame Error\r\n");
        WRITE_PERI_REG(UART_INT_CLR(uart_no), UART_FRM_ERR_INT_CLR);
        goto end_int_handler;
    }

    if(UART_RXFIFO_FULL_INT_ST == (READ_PERI_REG(UART_INT_ST(uart_no)) & UART_RXFIFO_FULL_INT_ST))
    {
        /* Rx FIFO is full, hence the interrupt */
        #if _ENABLE_RING_BUFFER == 1
        uint16_t index;
        uint16_t fifo_len = (READ_PERI_REG(UART_STATUS(uart_no))>>UART_RXFIFO_CNT_S)&UART_RXFIFO_CNT;
        //DBG1("RX FIFO FULL [%d]\r\n", fifo_len );

        if (fifo_len)
        {
            //DBG1("Rx Fifo contains %d characters have to unload\r\n", fifo_len);

            for (index=0;index<fifo_len; index++)
            {
                uint8_t ch = (READ_PERI_REG(UART_FIFO(UART0)) & 0xFF);

                //if (ch == '\r') ch = '\n';
	        ringbuf_memcpy_into(rxBuff, &ch, 1);
                #if _ENABLE_CONSOLE_INTEGRATION == 1
                uart_tx_one_char(uart_no, ch);
                if (ch == '\r')
                {
                    system_os_post(0, SIG_CONSOLE_RX, 0);
                }
                #endif
            }

            WRITE_PERI_REG(UART_INT_CLR(uart_no), UART_RXFIFO_FULL_INT_CLR);
            uart_rx_intr_enable(uart_no);
            #if _ENABLE_CONSOLE_INTEGRATION == 0
                system_os_post(uart_recvTaskPrio, SIG_UART0, 0);
            #endif
        }
        #else
        DBG1("RX FIFO FULL [%d]\r\n", (READ_PERI_REG(UART_STATUS(uart_no))>>UART_RXFIFO_CNT_S)& UART_RXFIFO_CNT);
        uart_rx_intr_disable(UART0);
        WRITE_PERI_REG(UART_INT_CLR(UART0), UART_RXFIFO_FULL_INT_CLR);
        system_os_post(uart_recvTaskPrio, SIG_UART0, 0);
        #endif

        goto end_int_handler;
    }

    if(UART_RXFIFO_TOUT_INT_ST == (READ_PERI_REG(UART_INT_ST(uart_no)) & UART_RXFIFO_TOUT_INT_ST))
    {
        /* The Time out threshold for Rx/Tx is being execeeded */
        DBG1("Rx Timeout Threshold not being met \r\n");
        uart_rx_intr_disable(UART0);
        WRITE_PERI_REG(UART_INT_CLR(UART0), UART_RXFIFO_TOUT_INT_CLR);

        system_os_post(uart_recvTaskPrio, SIG_UART0, 0);
        goto end_int_handler;
    }

    if(UART_TXFIFO_EMPTY_INT_ST == (READ_PERI_REG(UART_INT_ST(uart_no)) & UART_TXFIFO_EMPTY_INT_ST))
    {
        /* The Tx FIFO is empty, the FIFO needs to be fed with new data */
        DBG1("Tx FIFO is empty\r\n");
        CLEAR_PERI_REG_MASK(UART_INT_ENA(UART0), UART_TXFIFO_EMPTY_INT_ENA);

        #if UART_BUFF_EN
            tx_start_uart_buffer(UART0);
        #endif
        //system_os_post(uart_recvTaskPrio, 1, 0);
        WRITE_PERI_REG(UART_INT_CLR(uart_no), UART_TXFIFO_EMPTY_INT_CLR);
    }

end_int_handler:
    return;
}

/******************************************************************************
 * FunctionName : uart_tx_one_char_no_wait
 * Description  : uart tx a single char without waiting for fifo
 * Parameters   : uint8 uart - uart port
 *                uint8 TxChar - char to tx
 * Returns      : STATUS
*******************************************************************************/
STATUS uart_tx_one_char(uint8 uart, uint8 TxChar)
{
    while (true)
	{
		uint32 fifo_cnt = READ_PERI_REG(UART_STATUS(uart)) & (UART_TXFIFO_CNT<<UART_TXFIFO_CNT_S);
		if ((fifo_cnt >> UART_TXFIFO_CNT_S & UART_TXFIFO_CNT) < 126) {
			break;
		}
	}

	WRITE_PERI_REG(UART_FIFO(uart) , TxChar);
	return OK;
}

/******************************************************************************
 * FunctionName : uart_tx_one_char_no_wait
 * Description  : uart tx a single char without waiting for fifo
 * Parameters   : uint8 uart - uart port
 *                uint8 TxChar - char to tx
 * Returns      : STATUS
*******************************************************************************/
STATUS uart_tx_one_char_no_wait(uint8 uart, uint8 TxChar)
{
    uint8 fifo_cnt = (( READ_PERI_REG(UART_STATUS(uart))>>UART_TXFIFO_CNT_S)& UART_TXFIFO_CNT);
    if (fifo_cnt < 126)
    {
        WRITE_PERI_REG(UART_FIFO(uart) , TxChar);
    }
    return OK;
}


/******************************************************************************
 * FunctionName : uart0_write_char_no_wait
 * Description  : tx a single char without waiting for uart 0.
                  helper function for os_printf output to fifo or tx buffer
 * Parameters   : uint8 uart - uart port
 *                uint8 TxChar - char to tx
 * Returns      : STATUS
*******************************************************************************/
LOCAL void ICACHE_FLASH_ATTR uart0_write_char_no_wait(char c)
{
#if UART_BUFF_EN    //send to uart0 fifo but do not wait
    uint8 chr;
    if (c == '\n'){
        chr = '\r';
        tx_buff_enq(&chr, 1);
        chr = '\n';
        tx_buff_enq(&chr, 1);
    }else if (c == '\r'){

    }else{
        tx_buff_enq(&c,1);
    }
#else //send to uart tx buffer
    if (c == '\n')
    {
        uart_tx_one_char(UART0, '\r');
        uart_tx_one_char(UART0, '\n');

        //uart_tx_one_char_no_wait(UART0, '\r');
        //uart_tx_one_char_no_wait(UART0, '\n');
    }
    else
        if (c == '\r')
        {
            uart_tx_one_char(UART0, c);
        }
        else
        {
            uart_tx_one_char(UART0, c);
            //uart_tx_one_char_no_wait(UART0, c);
        }
#endif
}


/******************************************************************************
 * FunctionName : UART_SetPrintPort
 * Description  :
 *
 * Parameters   :
 * Returns      : NONE
*******************************************************************************/
void ICACHE_FLASH_ATTR UART_SetPrintPort(uint8 uart_no)
{
    if(uart_no==1)
    {
        //os_install_putc1(uart1_write_char);
    }
    else
    {
        /*option 1: do not wait if uart fifo is full,drop current character*/
        //os_install_putc1(uart0_write_char_no_wait);
        /*option 2: wait for a while if uart fifo is full*/
        //os_install_putc1(uart0_write_char);
    }
}

