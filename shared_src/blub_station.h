// Standard hardware on the BLUB station carrier board

#pragma once

class OkLittleLayout;
class XBeeRadio;

extern OkLittleLayout* status_layout;
extern XBeeRadio* xbee_radio;

void blub_station_init(char const* name);
