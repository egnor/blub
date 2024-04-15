// Standard hardware on the BLUB station carrier board

#pragma once

class LittleStatus;

extern LittleStatus* status_screen;

void blub_station_init(char const* name);
