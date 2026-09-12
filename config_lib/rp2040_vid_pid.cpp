#include <Arduino.h>
#include <USB.h>

#if defined(ARDUINO_ARCH_RP2040)
void initVariant() {
  // Use Pi SDK VID/PID so `picotool run -f` works as intended
  USB.setVIDPID(0x2e8a, 0x000a);
}
#endif
