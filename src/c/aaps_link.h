// AAPS link: sends the watch's steps and heart rate to the AAPS fork on the phone, and keeps the
// AAPS data it gets back for the face to show.
//
// To add it to a watchface: copy aaps_link.c and aaps_link.h into the face's src/c, include this
// header and call aaps_link_init() once in the face's init. README.md lists the two package.json
// entries it also needs.
#pragma once

#include <pebble.h>

void aaps_link_init(void);

// When the phone last acknowledged a snapshot, or 0 if it has not yet. For a face that shows the
// link. On a face with JavaScript the face's own JavaScript acknowledges every message too, so
// there it does not prove that AAPS got anything.
time_t aaps_link_last_ack(void);

// What AAPS last sent for the face to show, as ASCII text ready to draw. Display only: nothing in
// it is ever acted on.
typedef struct {
  char bg[8];     // glucose in the user's units, e.g. "6.4"; "---" when none is newer than 15 min
  char trend[4];  // "^^", "^", "/", "->", "\", "v", "vv"; empty when unknown or the glucose is old
  char delta[8];  // e.g. "+0.2"; empty when the glucose is old
  char iob[8];    // e.g. "1.25"; "--" when AAPS has sent nothing for 15 min
  char cob[8];    // e.g. "45g"; "--" likewise
  int age_min;    // minutes since the glucose was measured, -1 when it is old or there is none
  time_t bg_time; // when the glucose was measured (phone clock, UTC seconds); 0 when bg is "---"
  int bg_mgdl;    // the glucose in whole mg/dL, to compare with limits; 0 when bg is "---" or
                  // AAPS is too old to send it
} AapsLinkDisplay;

// Fills out with the current values; call it whenever the face draws (the age moves on by itself).
void aaps_link_display(AapsLinkDisplay *out);

// Called when a new display message has arrived, so the face can redraw. NULL removes it.
typedef void (*AapsLinkDisplayHandler)(void);
void aaps_link_on_display(AapsLinkDisplayHandler handler);
