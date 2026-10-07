#pragma once
/* Host shim: the panel driver's surface the UI task reaches. No I2C. */
#include <stdbool.h>
#include "u8g2.h"
bool i2c_u8g2_service(void);
