/*
* easygpio.h
*
* Copyright (c) 2015, eadf (https://github.com/eadf)
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* * Redistributions of source code must retain the above copyright notice,
* this list of conditions and the following disclaimer.
* * Redistributions in binary form must reproduce the above copyright
* notice, this list of conditions and the following disclaimer in the
* documentation and/or other materials provided with the distribution.
* * Neither the name of Redis nor the names of its contributors may be used
* to endorse or promote products derived from this software without
* specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
* ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
* POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef EASYGPIO_INCLUDE_EASYGPIO_EASYGPIO_H_
#define EASYGPIO_INCLUDE_EASYGPIO_EASYGPIO_H_

#include "c_types.h"

typedef enum {
  EASYGPIO_INPUT=0,
  EASYGPIO_OUTPUT=1
} EasyGPIO_PinMode;

typedef enum {
  EASYGPIO_PULLUP=3,
  EASYGPIO_NOPULL=4
} EasyGPIO_PullStatus;

/**
 * Returns the gpio name and func for a specific pin.
 */
bool easygpio_getGPIONameFunc(uint8_t gpio_pin, uint32_t *gpio_name, uint8_t *gpio_func);

/**
 * Sets the 'gpio_pin' pin as a GPIO and sets the interrupt to trigger on that pin.
 * The 'interruptArg' is the function argument that will be sent to your interruptHandler
 * (this way you can several interrupts with one interruptHandler)
 */
bool easygpio_attachInterrupt(uint8_t gpio_pin, EasyGPIO_PullStatus pullStatus, void (*interruptHandler)(void *arg), void *interruptArg);

/**
 * Deatach the interrupt handler from the 'gpio_pin' pin.
 */
bool easygpio_detachInterrupt(uint8_t gpio_pin);

/**
 * Returns the number of active pins in the gpioMask.
 */
uint8_t easygpio_countBits(uint32_t gpioMask);

/**
 * Sets the 'gpio_pin' pin as a GPIO and enables/disables the pull-up on that pin.
 * 'pullStatus' has no effect on output pins or GPIO16
 */
bool easygpio_pinMode(uint8_t gpio_pin, EasyGPIO_PullStatus pullStatus, EasyGPIO_PinMode pinMode);

/**
 * Enable or disable the internal pull up for a pin.
 */
bool easygpio_pullMode(uint8_t gpio_pin, EasyGPIO_PullStatus pullStatus);

/**
 * Uniform way of getting GPIO input value. Handles GPIO 0-16.
 * The pin must be initiated with easygpio_pinMode() so that the pin mux is setup as a gpio in the first place.
 * If you know that you won't be using GPIO16 then you'd better off by just using GPIO_INPUT_GET().
 */
uint8_t easygpio_inputGet(uint8_t gpio_pin);

/**
 * Uniform way of setting GPIO output value. Handles GPIO 0-16.
 *
 * You can not rely on that this function will switch the gpio to an output like GPIO_OUTPUT_SET does.
 * Use easygpio_outputEnable() to change an input gpio to output mode.
 */
void easygpio_outputSet(uint8_t gpio_pin, uint8_t value);

/**
 * Uniform way of turning an output GPIO pin into input mode. Handles GPIO 0-16.
 * The pin must be initiated with easygpio_pinMode() so that the pin mux is setup as a gpio in the first place.
 * This function does the same thing as GPIO_DIS_OUTPUT, but works on GPIO16 too.
 */
void easygpio_outputDisable(uint8_t gpio_pin);

/**
 * Uniform way of turning an input GPIO pin into output mode. Handles GPIO 0-16.
 * The pin must be initiated with easygpio_pinMode() so that the pin mux is setup as a gpio in the first place.
 *
 * This function:
 *  - should only be used to convert a input pin into an output pin.
 *  - is a little bit slower than easygpio_outputSet() so you should use that
 *    function to just change output value.
 *  - does the same thing as GPIO_OUTPUT_SET, but works on GPIO16 too.
 */
void easygpio_outputEnable(uint8_t gpio_pin, uint8_t value);

#endif /* EASYGPIO_INCLUDE_EASYGPIO_EASYGPIO_H_ */
