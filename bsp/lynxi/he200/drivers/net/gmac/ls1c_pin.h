#ifndef __LS1C_PIN_H__
#define __LS1C_PIN_H__

typedef enum
{
    PIN_PURPOSE_GPIO = 0,
    PIN_PURPOSE_OTHER,
} pin_purpose_t;

typedef enum
{
    PIN_REMAP_FIRST = 0,
    PIN_REMAP_SECOND,
    PIN_REMAP_THIRD,
    PIN_REMAP_FOURTH,
    PIN_REMAP_FIFTH,
    PIN_REMAP_DEFAULT,
} pin_remap_t;

void pin_set_purpose(unsigned int gpio, pin_purpose_t purpose);
void pin_set_remap(unsigned int gpio, pin_remap_t remap);

#endif
