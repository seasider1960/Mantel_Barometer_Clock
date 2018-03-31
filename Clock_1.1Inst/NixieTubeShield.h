/*
ESP8266 NTP Nixie Tube Clock Program

Code developed by Craig A. Lindley

THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY OR FITNESS
FOR A PARTICULAR PURPOSE.  IT MAY INFRINGE THE INTELLECTUAL PROPERTY RIGHTS OF OTHERS
BECAUSE I COPY A LOT OF STUFF I FIND ON THE INTERWAVES. IT MAY KILL OR INJURE YOU OR RESULT IN THE LOSS
OF SOME OR ALL OF YOUR MONEY.  I WILL NOT INDEMNIFY OR DEFEND YOU IF ANY OF THIS HAPPENS.  THIS SOFTWARE MAY
HAVE BEEN PROVIDED SUBJECT TO A SEPARATE LICENSE AGREEMENT. IF THAT IS THE CASE YOUR RIGHTS AND MY 
OBLIGATIONS ARE FURTHER LIMITED (BUT IN NO EVENT EXPANDED BY THAT AGREEMENT.

ANY USE OF THIS SOFTWARE MEANS YOU HAVE AGREED TO ALL THAT.  SO THERE.

This is the clock portion of a three-part system using three separate ESP8266-based NodeMCUs
installed in a vaguely Arts and Crafts wooden clock case. 
    
    
*/

#ifndef NIXIE_TUBE_SHIELD_H
#define NIXIE_TUBE_SHIELD_H

//#include "LEDControl.h"

// ***************************************************************
// NodeMCU Amica Hardware Configuration
// ***************************************************************

//#define LED_GREEN    D0
//#define LED_RED      D1
//#define LED_BLUE     D2
#define HV_ENABLE    D3
#define LATCH_ENABLE D8
#define NEON_DOTS    D9


// ***************************************************************
// Digit Data Definitions
// Each digit, except blank, has a single active low bit in a
// field of 10 bits
// ***************************************************************

#define DIGIT_0      0x3FE
#define DIGIT_1      0x3FD
#define DIGIT_2      0x3FB
#define DIGIT_3      0x3F7
#define DIGIT_4      0x3EF
#define DIGIT_5      0x3DF
#define DIGIT_6      0x3BF
#define DIGIT_7      0x37F
#define DIGIT_8      0x2FF
#define DIGIT_9      0x1FF
#define DIGIT_BLANK  0x3FF

#define BLANK_DIGIT  10

// Class Definition
class NixieTubeShield  {

public:

    // Class Constructor
    NixieTubeShield()  {

      // Setup output pins
      pinMode(HV_ENABLE,    OUTPUT);
      pinMode(LATCH_ENABLE, OUTPUT);
      pinMode(NEON_DOTS,    OUTPUT);

      // Set default outputs
      digitalWrite(HV_ENABLE,    LOW);
      digitalWrite(LATCH_ENABLE, LOW);
      digitalWrite(NEON_DOTS,    LOW);

      // Initialize digit storage to all ones so all digits are off
      for (int i = 0; i < 6; i++) {
        digits[i] = DIGIT_BLANK;
      }
    }

    // High voltage control
    void hvEnable(boolean state) {
      digitalWrite(HV_ENABLE, state);
      
    }

    // Neon lamp control
    void dotsEnable(boolean state) {
      digitalWrite(NEON_DOTS, state);
    }

    // Set the NX1 (most significant) digit
    void setNX1Digit(int d) {
      digits[5] = NUMERIC_DIGITS[d];
    }

    // Set the NX2 digit
    void setNX2Digit(int d) {
      digits[4] = NUMERIC_DIGITS[d];
    }

    // Set the NX3 digit
    void setNX3Digit(int d) {
      digits[3] = NUMERIC_DIGITS[d];
    }

    // Set the NX4 digit
    void setNX4Digit(int d) {
      digits[2] = NUMERIC_DIGITS[d];
    }

    // Set the NX5 digit
    void setNX5Digit(int d) {
      digits[1] = NUMERIC_DIGITS[d];
    }

    // Set the NX6 (least significant) digit
    void setNX6Digit(int d) {
      digits[0] = NUMERIC_DIGITS[d];
    }

    // Transfer digit data to the Nixie Tubes for display
    // using a Finite State Machine (FSM). The data is a 60 bit
    // stream which contains 10 bits for each of the six nixie
    // tubes.
    void show() {

      uint16_t accum;
      int digitIndex;
      int accumBits;

      // All possible FSM states
      enum STATES {STATE_INIT, STATE_CHECK, STATE_FILL, STATE_OUTPUT, STATE_DONE};

      // Set initial state of FSM
      enum STATES state = STATE_INIT;

      boolean done = false;

      while (! done) {
        switch (state) {
          // Initialize FSM variables
          case STATE_INIT:
            accum = 0;

            // Ignore 4 unused bits in 8, 8 bit SPI transfers
            accumBits = 4;
            digitIndex = 0;

            // Set latch enable low
            digitalWrite(LATCH_ENABLE, LOW);

            // Setup next state
            state = STATE_CHECK;
            break;

          // Check to see if there are enough bits to output
          case STATE_CHECK:
            if (accumBits < 8) {
              state = STATE_FILL;
            } else  {
              state = STATE_OUTPUT;
            }
            break;

          // Fill accumulator with new data
          case STATE_FILL:
            // Merge new digit data into accumulator
            accum |= ((digits[digitIndex++]) << (6 - accumBits));
            accumBits += 10;
            state = STATE_CHECK;
            break;

          // Output 8 bits of data from accumulator
          case STATE_OUTPUT:
            SPI.transfer((accum >> 8) & 0xFF);
            accum <<= 8;
            accumBits -= 8;

            // Check for transfer of digits complete
            if (digitIndex >= 6) {
              // Output final bits
              
              SPI.transfer((accum >> 8) & 0xFF);
              state = STATE_DONE;
              
            } else  {
              state = STATE_CHECK;
            }
            break;

          // Finish up
          case STATE_DONE:
            // Toggle enable to latch data
            digitalWrite(LATCH_ENABLE, HIGH);
            digitalWrite(LATCH_ENABLE, LOW);
            done = true;
            break;
        }
      }
    }

    // Do anti-poisoning routine and then turn off all tubes
    void doAntiPoisoning(void) {

      dotsEnable(false);

      for (int i = 0; i < 4; i++) {
        for (int i = 0; i < 11; i++) {
          setNX1Digit(i);
          setNX2Digit(i);
          setNX3Digit(i);
          setNX4Digit(i);
          setNX5Digit(i);
          setNX6Digit(i);

          // Make the changes visible
          show();

          // Small delay
          delay(500);
        }
      }
    }

  private:
    // Private data
    // Array of codes for each nixie digit
    int NUMERIC_DIGITS[11] = {
      DIGIT_0, DIGIT_1, DIGIT_2, DIGIT_3, DIGIT_4,
      DIGIT_5, DIGIT_6, DIGIT_7, DIGIT_8, DIGIT_9,
      DIGIT_BLANK
    };

    // Storage for codes for each nixie digit
    // Digit order is: NX6, NX5, NX4, NX3, NX2, NX1
    uint16_t digits[6];
};

#endif


