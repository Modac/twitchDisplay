/**
******************************************************************************
* @file    moving_average.h
* @author  Mohammad Hussein Tavakoli Bina, Sepehr Hashtroudi.
* @brief   This file contains an efficient implementation of 
*		    moving average filter.
******************************************************************************
*MIT License
*
*Copyright (c) 2018 Mohammad Hussein Tavakoli Bina
*
*Permission is hereby granted, free of charge, to any person obtaining a copy
*of this software and associated documentation files (the "Software"), to deal
*in the Software without restriction, including without limitation the rights
*to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
*copies of the Software, and to permit persons to whom the Software is
*furnished to do so, subject to the following conditions:
*
*The above copyright notice and this permission notice shall be included in all
*copies or substantial portions of the Software.
*
*THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
*OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
*SOFTWARE.
*/

#ifndef MOVING_AVERAGE_H
#define MOVING_AVERAGE_H

/* Includes ------------------------------------------------------------------*/
#include "stdint.h"

template <int WindowLength> // order is 1 or 2
class MovingAverage
{
  public:
    template <int WindowSize> 
    struct FilterTypeDef {
        uint32_t History[WindowSize]; /*Array to store values of filter window*/
        uint32_t Sum;	/* Sum of filter window's elements*/
        uint32_t FirstIndex; /* Index of the first element of window*/
    };

  private:
    FilterTypeDef<WindowLength> filter_struct;

  public:
    MovingAverage(){
        filter_struct.Sum = 0;
        filter_struct.FirstIndex = 0;
    
        for(uint32_t i=0; i<WindowLength; i++) {
            filter_struct.History[i] = 0;
        }
    }

    uint32_t compute(uint32_t raw_data){
        filter_struct.Sum += raw_data;
        filter_struct.Sum -= filter_struct.History[filter_struct.FirstIndex];
        filter_struct.History[filter_struct.FirstIndex] = raw_data;
        if(filter_struct.FirstIndex < WindowLength - 1) {
            filter_struct.FirstIndex += 1;
        } else {
            filter_struct.FirstIndex = 0;
        }
        return filter_struct.Sum/WindowLength;
    }
};

#endif