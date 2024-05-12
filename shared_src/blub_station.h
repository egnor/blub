// Standard hardware on the BLUB station carrier board

#pragma once

class LittleStatus;
class XBeeRadio;

extern LittleStatus* status_screen;
extern XBeeRadio* xbee_radio;

void blub_station_init(char const* name);
