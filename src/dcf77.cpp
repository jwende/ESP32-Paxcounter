/*
// Emulate a DCF77 radio receiver
//
// parts of this code werde adapted from source:
https://www.elektormagazine.com/labs/dcf77-emulator-with-esp8266-elektor-labs-version-150713
//
// a nice & free logic test program for DCF77 can be found here:
https://www-user.tu-chemnitz.de/~heha/viewzip.cgi/hs/Funkuhr.zip/
//
*/

#if defined HAS_DCF77

#include "dcf77.h"

// Local logging tag
static const char TAG[] = "main";

TaskHandle_t DCF77Task;
hw_timer_t *dcfCycle = NULL;

#define DCF77_FRAME_SIZE 60

// array of dcf pulses for three minutes
uint8_t DCFtimeframe[DCF77_FRAME_SIZE];

// initialize and configure DCF77 output
int dcf77_init(void) {

  time_t t, tt;
  BitsPending = false;

  pinMode(HAS_DCF77, OUTPUT);
  set_DCF77_pin(dcf_low);

  xTaskCreatePinnedToCore(dcf77_loop,  // task function
                          "dcf77loop", // name of task
                          2048,        // stack size of task
                          (void *)1,   // parameter of the task
                          3,           // priority of the task
                          &DCF77Task,  // task handle
                          0);          // CPU core

  assert(DCF77Task); // has dcf77 task started?

  // setup 100ms clock signal for DCF77 generator using esp32 hardware timer 1
  ESP_LOGD(TAG, "Starting DCF pulse...");
  dcfCycle = timerBegin(1, 8000, true); // set 80 MHz prescaler to 1/10000 sec
  timerAttachInterrupt(dcfCycle, &DCF77IRQ, true);
  timerAlarmWrite(dcfCycle, 1000, true); // 100ms cycle

  // wait until beginning of next second, then kick off first DCF pulse and
  // start timer interrupt

  t = tt = now();
  do {
    tt = now();
  } while (t == tt);

  DCF_Out(second(tt));
  timerAlarmEnable(dcfCycle);

  return 1; // success

} // ifdcf77_init

void generateTimeframe(time_t tt) {

  uint8_t ParityCount;
  time_t t = myTZ.toLocal(tt); // convert to local time

  // ENCODE HEAD
  // bits 0..19 initialized with zeros
  for (int n = 0; n <= 19; n++)
    DCFtimeframe[n] = dcf_zero;
  // bits 17..18: adjust for DayLightSaving
  DCFtimeframe[18 - (myTZ.locIsDST(t) ? 1 : 0)] = dcf_one;
  // bit 20: must be 1 to indicate time active
  DCFtimeframe[20] = dcf_one;

  // ENCODE MINUTE (bits 21..28)
  ParityCount = dec2bcd(minute(t), 21, 27, DCFtimeframe);
  DCFtimeframe[28] = (ParityCount & 1) ? dcf_one : dcf_zero;

  // ENCODE HOUR (bits 29..35)
  ParityCount = dec2bcd(hour(t), 29, 34, DCFtimeframe);
  DCFtimeframe[35] = (ParityCount & 1) ? dcf_one : dcf_zero;

  // ENCODE DATE (bits 36..58)
  ParityCount = dec2bcd(day(t), 36, 41, DCFtimeframe);
  ParityCount +=
      dec2bcd((weekday(t) - 1) ? (weekday(t) - 1) : 7, 42, 44, DCFtimeframe);
  ParityCount += dec2bcd(month(t), 45, 49, DCFtimeframe);
  ParityCount +=
      dec2bcd(year(t) - 2000, 50, 57,
              DCFtimeframe); // yes, we have a millenium 3000 bug here ;-)
  DCFtimeframe[58] = (ParityCount & 1) ? dcf_one : dcf_zero;

  // ENCODE TAIL (bit 59)
  DCFtimeframe[59] = dcf_off;
  // !! missing code here for leap second !!

  /*
    // for debug: print the DCF77 frame buffer
    char out[DCF77_FRAME_SIZE + 1];
    uint8_t i;
    for (i = 0; i < DCF77_FRAME_SIZE; i++) {
      out[i] = DCFtimeframe[i] + '0'; // convert int digit to printable ascii
    }
    out[DCF77_FRAME_SIZE] = '\0'; // string termination char
    ESP_LOGD(TAG, "DCF Timeframe = %s", out);
  */
}

// called every 100msec by hardware timer to pulse out DCF signal
void DCF_Out(uint8_t startsec) {

  static uint8_t bit = startsec;
  static uint8_t pulse = 0;

  if (!BitsPending) {
    // prepare frame to send for next minute
    generateTimeframe(now() + 61);
    // start blinking symbol on display and kick off timer
    BitsPending = true;
  }

  // ticker out current DCF frame
  if (BitsPending) {
    switch (pulse++) {

    case 0: // start of second -> start of timeframe for logic signal
      if (DCFtimeframe[bit] != dcf_off)
        set_DCF77_pin(dcf_low);
      break;

    case 1: // 100ms after start of second -> end of timeframe for logic 0
      if (DCFtimeframe[bit] == dcf_zero)
        set_DCF77_pin(dcf_high);
      break;

    case 2: // 200ms after start of second -> end of timeframe for logic 1
      set_DCF77_pin(dcf_high);
      break;

    case 9: // last pulse before next second starts
      pulse = 0;
      if (bit++ == (DCF77_FRAME_SIZE - 1)) // end of DCF77 frame (59th second)
      {
        bit = 0;
        BitsPending = false;
      };
      break;

    }; // switch
  };   // if

} // DCF_Out()

// interrupt service routine triggered each 100ms by ESP32 hardware timer
void IRAM_ATTR DCF77IRQ() {
  xTaskNotifyFromISR(DCF77Task, 0, eNoAction, NULL);
  portYIELD_FROM_ISR();
}

void dcf77_loop(void *pvParameters) {

  configASSERT(((uint32_t)pvParameters) == 1); // FreeRTOS check

  // task remains in blocked state until it is notified by isr
  for (;;) {
    xTaskNotifyWait(
        0x00,      // don't clear any bits on entry
        ULONG_MAX, // clear all bits on exit
        NULL,
        portMAX_DELAY); // wait forever (missing error handling here...)

    DCF_Out(0);
  }
  BitsPending = false; // stop blink in display, should never be reached
  vTaskDelete(DCF77Task);
} // dcf77_loop()

// helper function to convert decimal to bcd digit
uint8_t dec2bcd(uint8_t dec, uint8_t startpos, uint8_t endpos,
                uint8_t pArray[]) {

  uint8_t data = (dec < 10) ? dec : ((dec / 10) << 4) + (dec % 10);
  uint8_t parity = 0;

  for (uint8_t n = startpos; n <= endpos; n++) {
    pArray[n] = (data & 1) ? dcf_one : dcf_zero;
    parity += (data & 1);
    data >>= 1;
  }

  return parity;
}

// helper function to switch GPIO line with DCF77 signal
void set_DCF77_pin(dcf_pinstate state) {
  switch (state) {
  case dcf_low:
#ifdef DCF77_ACTIVE_LOW
    digitalWrite(HAS_DCF77, HIGH);
#else
    digitalWrite(HAS_DCF77, LOW);
#endif
    break;
  case dcf_high:
#ifdef DCF77_ACTIVE_LOW
    digitalWrite(HAS_DCF77, LOW);
#else
    digitalWrite(HAS_DCF77, HIGH);
#endif
    break;
  } // switch
} // DCF77_pulse

#endif // HAS_DCF77