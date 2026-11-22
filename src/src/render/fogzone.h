// Fog for the frame: the mission's fog, or a NewDark fog zone's.
//
// NewDark missions (FOGZONEVAR) define a global fog and eight fog zones; each
// world cell names its zone in the low nibble of its WREXT zone byte (the
// high nibble is the ambient-light zone). The fog drawn is the zone the camera
// is in, eased over a second when it changes, so walking from the street into
// a fogged crypt thickens the air rather than switching it.

#ifndef _FOGZONE_H_
#define _FOGZONE_H_
#pragma once

#include <lg.h>

// The camera's cell gives the zone (-1: unknown). out: colour 0-255, distance
// in feet at which the fog is opaque (0: no fog). Call once a frame.
EXTERN void FogZoneFrame(int cameraCell, float rgb[3], float *dist);

// Zones 1-8 (0 is the mission's global fog, portal_fog_color/dist); for the
// engine script service. Get returns FALSE for a zone that has no fog of its
// own (it then follows the global fog).
EXTERN BOOL FogZoneGet(int zone, int *r, int *g, int *b, float *dist);
EXTERN void FogZoneSet(int zone, int r, int g, int b, float dist);

// low nibble of the WREXT cell zone byte (wrloop.cpp)
EXTERN int portal_cell_fog_zone(int cell);

#endif
