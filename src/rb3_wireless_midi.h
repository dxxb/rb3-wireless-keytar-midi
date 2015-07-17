/*
 *  rb3-wireless-keytar-midi
 *
 *  Copyright (c) 2015 Delio Brignoli. All rights reserved. See LICENSE file.
 *
 */

#import <IOKit/hid/IOHIDLib.h>

int setup_hid(IOHIDManagerRef hid_manager);
void teardown_hid(IOHIDManagerRef hid_manager);
