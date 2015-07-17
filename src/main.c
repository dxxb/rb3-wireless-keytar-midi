/*
 *  rb3-wireless-keytar-midi
 *
 *  Copyright (c) 2015 Delio Brignoli. All rights reserved. See LICENSE file.
 *
 */

#include <stdio.h>
#import <IOKit/hid/IOHIDLib.h>

#include "rb3_wireless_midi.h"

int main(int argc, const char *argv[])
{
    IOHIDManagerRef hid_manager = NULL;

    printf("rb3-wireless-keytar-midi started...\n");
    int err = setup_hid(hid_manager);
    if (err)
        printf("setup_hid() failed: %d\n", err);
    CFRunLoopRun();
    teardown_hid(hid_manager);
    return 0;
}
