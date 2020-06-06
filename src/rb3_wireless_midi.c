/*
 *  rb3-wireless-keytar-midi
 *
 *  Copyright (c) 2015 Delio Brignoli. All rights reserved. See LICENSE file.
 *
 */

#import <CoreFoundation/CoreFoundation.h>
#import <CoreMIDI/MIDIServices.h>

#include "rb3_wireless_midi.h"

#define USE_MATCHING_DICT 1

#define VENDOR_ID (0x1BAD)
#define PRODUCT_ID (0x3330)

#define KEY_NUM (25)
#define MIN_OCTAVE (0)
#define MAX_OCTAVE (8)
#define DEFAULT_OCTAVE (4)
#define MIDI_BUFSZ (128)
#define FIRST_GENERAL_MIDI_DRUM_NOTE (35)
#define MIN_PROGRAM (0)
#define MAX_PROGRAM (127)
#define MIDI_VOLUME_CTRL (0x07)
#define MIDI_FOOT_CTRL (0x04)
#define MIDI_EXPRESSION_CTRL (0x0B)
#define DEFAULT_MIDI_PEDAL_CTRL MIDI_EXPRESSION_CTRL /* default to expression pedal */

#define BTN_AB12_IDX (0) /* 1 0x1, A 0x2, B 0x4, 2 0x8  */
#define BTN_MHP_IDX (1) /* minus 0x01, home 0x10, plus 0x02 */
#define DPAD_STATE_IDX (2) /* 0x8 off, 0x0 up, 0x2 right, 0x6 left, 0x4 down */

/* One bit per key, 25 keys, C3 -> C5 MSB first */
#define KB_KEYSTATE1_IDX (5)
#define KB_KEYSTATE2_IDX (6)
#define KB_KEYSTATE3_IDX (7)
#define KB_KEYSTATE4_IDX (8) /* this overlaps with velocity slot #1, bit 7 has the key state */

/* Up to 5 key velocity info report offsets 8-12, values 0x00 - 0x7F */
#define VELOCITY_SLOT_COUNT (5)
#define KB_FIRST_KEYVEL_IDX (8)

#define BTN_HANDLE_IDX (13) /* 0x80 pressed, 0x00 depressed */
#define MISC_PEDAL_IDX (14) /* (0x00-0x7F & 0x7F) for expression pedal value and (val & 0x80) for switch state */
#define MISC_TOUCHSTRIP_IDX (15) /* 0x00 - 0x7F, 0x00 when no touch info is available */
#define MISC_TRSJACK_STATUS_IDX (20) /* 0x00 no connection, 0x01 r-s shorted, 0x02 R(r-s) < R(t-s), 0x03 R(r-s) > R(t-s) */

#define USB_UPDATESEQID_IDX (25) /* Increments for each changed report. 0x00 when disconnected. */
#define WLESS_CHANSTATUS_IDX (26) /* wireless channel select or status? 0x00 when disconnected */

#define BTN_1_MASK (0x01)
#define BTN_A_MASK (0x02)
#define BTN_B_MASK (0x04)
#define BTN_2_MASK (0x08)

#define BTN_MINUS_MASK (0x01)
#define BTN_HOME_MASK (0x10)
#define BTN_PLUS_MASK (0x02)

#define DPAD_OFF_VAL (0x08)
#define DPAD_L_VAL (0x06)
#define DPAD_R_VAL (0x02)
#define DPAD_U_VAL (0x00)
#define DPAD_D_VAL (0x04)

struct rb_keytar_dev {
    struct rb_keytar_dev *prev;
    struct rb_keytar_dev *next;

    IOHIDDeviceRef io_hid_dev;

    size_t errored_report_count;
    size_t missed_report_count;
    size_t in_report_size;
    uint8_t *in_report;
    uint8_t *last_in_report;

    MIDIClientRef midiclient;
    MIDIPortRef midiport;
    MIDIEndpointRef  midiout;
    MIDIPacket *midi_curpacket;
    size_t midi_packetlist_sz;
    MIDIPacketList *midi_packetlist;

    uint8_t channel;
    uint8_t octave;
    uint8_t program;
    uint8_t pedal_midi_ctrl;
    bool drum_mapping;
};

static CFStringRef client_name = CFSTR("RB3 Wireless Keytar MIDI Client");
static CFStringRef source_name = CFSTR("RB3 Wireless Keytar MIDI Source");
static CFStringRef port_name = CFSTR("RB3 Wireless Keytar MIDI Port");

static struct rb_keytar_dev *list_head = NULL, *list_tail = NULL;

CFMutableDictionaryRef create_dev_matching_dict(int vendor_id, int prod_id)
{
    CFMutableDictionaryRef res = CFDictionaryCreateMutable(
                                                           kCFAllocatorDefault,
                                                           0,
                                                           &kCFTypeDictionaryKeyCallBacks,
                                                           &kCFTypeDictionaryValueCallBacks);
    if (res) {
        CFNumberRef vid = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                         &vendor_id);
        if (vid) {
            CFDictionarySetValue(res, CFSTR(kIOHIDVendorIDKey), vid);
            CFRelease(vid);
        } else {
            goto err;
        }

        CFNumberRef pid = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                         &prod_id);
        if (pid) {
            CFDictionarySetValue(res, CFSTR(kIOHIDProductIDKey), pid);
            CFRelease(pid);
        } else {
            goto err;
        }
    }
    return res;

err:
    if (res)
        CFRelease(res);
    return NULL;
}

static void midipacketlist_send_(struct rb_keytar_dev *ktr_dev)
{
    if (ktr_dev->midi_curpacket && ktr_dev->midi_packetlist->numPackets) {
        MIDIReceived(ktr_dev->midiout, ktr_dev->midi_packetlist);
        ktr_dev->midi_curpacket = NULL;
    }
}

static void midipacket_add_(struct rb_keytar_dev *ktr_dev, MIDITimeStamp time,
                            ByteCount nData, const Byte *data)
{
    if (!ktr_dev->midi_curpacket)
        ktr_dev->midi_curpacket = MIDIPacketListInit(ktr_dev->midi_packetlist);

    MIDIPacket *tmp = MIDIPacketListAdd(ktr_dev->midi_packetlist, ktr_dev->midi_packetlist_sz,
                                        ktr_dev->midi_curpacket, time, nData, data);
    if (!tmp) {
        MIDIReceived(ktr_dev->midiout, ktr_dev->midi_packetlist);
        ktr_dev->midi_curpacket = MIDIPacketListInit(ktr_dev->midi_packetlist);
        tmp = MIDIPacketListAdd(ktr_dev->midi_packetlist, ktr_dev->midi_packetlist_sz,
                                ktr_dev->midi_curpacket, time, nData, data);
    }

    assert(tmp);
    ktr_dev->midi_curpacket = tmp;
}

static void midi_panic_(struct rb_keytar_dev *ktr_dev)
{
    MIDITimeStamp timestamp = 0;
    uint8_t alloff[3]= {0xB0 | ktr_dev->channel, 0x78, 0x00};
    midipacket_add_(ktr_dev, timestamp, sizeof(alloff), alloff);
}

static inline uint32_t key_bits_(uint8_t *report_buffer)
{
    return (uint32_t)report_buffer[KB_KEYSTATE1_IDX]<<24|((uint32_t)report_buffer[KB_KEYSTATE2_IDX]<<16)|
    ((uint32_t)report_buffer[KB_KEYSTATE3_IDX]<<8)|((uint32_t)report_buffer[KB_KEYSTATE4_IDX]&0x80);
}

static inline bool report_idx_changed_(struct rb_keytar_dev *ktr_dev, size_t report_idx)
{
    return (ktr_dev->in_report[report_idx] != ktr_dev->last_in_report[report_idx]);
}

static inline uint8_t report_idx_changed_bits_(struct rb_keytar_dev *ktr_dev, size_t report_idx)
{
    return (ktr_dev->in_report[report_idx] ^ ktr_dev->last_in_report[report_idx]);
}

static inline bool single_key_edge_(struct rb_keytar_dev *ktr_dev, size_t report_idx, uint8_t mask)
{
    return ((ktr_dev->in_report[report_idx] == mask) &&
            !(ktr_dev->last_in_report[report_idx] & mask));
}

static void handle_input_report(void * inContext,
                                IOReturn inResult,
                                void * inSender,
                                IOHIDReportType inType,
                                uint32_t inReportID,
                                uint8_t *inReport,
                                CFIndex InReportLength)
{
    struct rb_keytar_dev *ktr_dev = (struct rb_keytar_dev*)inContext;
    MIDITimeStamp timestamp = 0;

    printf("\nReport (%ld bytes): [", (long)InReportLength);
    for (long idx=0; idx < InReportLength; idx++) {
        printf("%02hhX,", inReport[idx]);
    }
    printf("]\n");

    /* Ignore errored reports */
    if (inResult) {
        ktr_dev->errored_report_count++;
        return;
    }

    /* Ignore reports where nothing changed */
    if (!report_idx_changed_(ktr_dev, USB_UPDATESEQID_IDX))
        return;

    /* Detect keyboard disconnect and send MIDI all off */
    if (report_idx_changed_(ktr_dev, WLESS_CHANSTATUS_IDX) && !ktr_dev->in_report[WLESS_CHANSTATUS_IDX]) {
        midi_panic_(ktr_dev);
        goto send_midi_cmds;
    }

    /* Detect lost report and send MIDI all off */
    if ((ktr_dev->in_report[USB_UPDATESEQID_IDX]-ktr_dev->last_in_report[USB_UPDATESEQID_IDX]) != 1) {
        ktr_dev->missed_report_count++;
    }

    /* determine which keys changed state */
    uint32_t key_new_bits = key_bits_(ktr_dev->in_report);
    uint32_t key_old_bits = key_bits_(ktr_dev->last_in_report);
    uint32_t key_changed_bits = key_new_bits ^ key_old_bits;

    /* FIXME:
     * - in MIDI mode both note on and note off messages carry a valid velocity value (i.e. release
     *     velocity is also transmitted).
     * - in MIDI mode valid velocity information is transmitted independently of the number of keys
     *     currently held down.
     * - in MIDI mode when mapping lower octave to drums velocity on note off is always zero.
     *
     * This suggests the current velocity code could be improved assuming the same information is made
     * available via the wireless dongle. It is not clear how to retrieve it because there are
     * apparently only 5 slots with velocity information in the USB report. Perhaps a slot
     * can be freed by acknowledging its data.
     */

    /* determine which velocity slots contain new velocity information
     *
     * Slots can transition as follows:
     *     0x00 -> 0xVV : slot has velocity info for newly pressed key.
     *     0xVV -> 0x40 : slot is being re-associated to an already pressed key for which no velocity info had been published.
     *     0x40 -> 0xVV : slot shows the original (non-published) velocity info for the already pressed key.
     *     0xVV -> 0x00 : slot is being freed.
     *
     * There are only 5 slots for velocity information so velocity information is not available at the time of the
     * keypress detection beyond the 5th key being held down simultaneously. This implementation uses the default
     * velocity value (0x40) whenever velocity information is not available instead of waiting for slots to be freed.
     *
     * A more complicated implementation could hold new note-on events to try and associate them with
     * delayed velocity information that was not available at the time of the keypress. If a maximum note lag is
     * reached and no velocity information is available then the note would be transmitted with a default velocity.
     *
     * Another alterative would be to re-trigger the note once its veocity information appears in a slot
     * (0xVV->0x40->0xVV transition) but re-triggering a note is rendered by the playback device in a device-specific
     * way (i.e. re-start the same voice or allocate and start a new one) so it's probably going to result in undesired
     * glitches.
     *
     */
    uint8_t new_key_vel[VELOCITY_SLOT_COUNT] = {0x40, 0x40, 0x40, 0x40, 0x40};
    for (size_t slot_idx = KB_FIRST_KEYVEL_IDX, array_idx = 0; slot_idx < KB_FIRST_KEYVEL_IDX+VELOCITY_SLOT_COUNT; slot_idx++) {
        if (!report_idx_changed_(ktr_dev, slot_idx))
            continue;
        if (!ktr_dev->last_in_report[slot_idx]) {
            /* slot is changing from zero to non-zero */
            /* this is the only case when velocity information for a new key is provided */
            new_key_vel[array_idx++] = ktr_dev->in_report[slot_idx] & 0x7F;
        }
    }

    size_t key_idx = 0;
    size_t new_note_cnt = 0;
    while (key_changed_bits) {
        if (!(key_changed_bits & 0x80000000))
            goto next_key;
        /* process key */
        uint8_t midi_note_idx = (ktr_dev->octave*12)+key_idx;
        uint8_t midi_note[3] = {ktr_dev->channel & 0xF, midi_note_idx, 0x00};
        if (ktr_dev->drum_mapping && midi_note_idx < 12) {
            /* drums only on channel 10 */
            midi_note[0] = 0x9;
            midi_note[1] = FIRST_GENERAL_MIDI_DRUM_NOTE+key_idx;
        }
        if (key_new_bits & 0x80000000) {
            /* key on */
            midi_note[0] |= 0x90;
            midi_note[2] = new_note_cnt < VELOCITY_SLOT_COUNT ? new_key_vel[new_note_cnt] : 0x40;
            new_note_cnt++;
        } else {
            /* key off */
            midi_note[0] |= 0x80;
        }
        midipacket_add_(ktr_dev, timestamp, sizeof(midi_note), midi_note);

    next_key:
        key_changed_bits <<= 1;
        key_new_bits <<= 1;
        key_idx++;
    }

    /* handle minus-home-plus buttons events */
    if (report_idx_changed_(ktr_dev, BTN_MHP_IDX)) {
        if (ktr_dev->in_report[BTN_MHP_IDX] == (BTN_MINUS_MASK|BTN_HOME_MASK|BTN_PLUS_MASK)) {
            /* panic key combination, send MIDI all off */
            midi_panic_(ktr_dev);
        } else if (single_key_edge_(ktr_dev, BTN_MHP_IDX, BTN_MINUS_MASK)) {
            /* send MIDI stop */
            uint8_t realtimemsg[]= {0xFA+2};
            midipacket_add_(ktr_dev, timestamp, sizeof(realtimemsg), realtimemsg);
        } else if (single_key_edge_(ktr_dev, BTN_MHP_IDX, BTN_HOME_MASK)) {
            /* send MIDI continue */
            uint8_t realtimemsg[]= {0xFA+1};
            midipacket_add_(ktr_dev, timestamp, sizeof(realtimemsg), realtimemsg);
        } else if (single_key_edge_(ktr_dev, BTN_MHP_IDX, BTN_PLUS_MASK)) {
            /* send MIDI start */
            uint8_t realtimemsg[]= {0xFA+0};
            midipacket_add_(ktr_dev, timestamp, sizeof(realtimemsg), realtimemsg);
        }
    }

    /* handle 1,2,A,B buttons events */
    while (report_idx_changed_(ktr_dev, BTN_AB12_IDX)) {
        if (ktr_dev->in_report[BTN_AB12_IDX] == (BTN_1_MASK|BTN_B_MASK)) {
            /* reset octave transpose */
            ktr_dev->octave = DEFAULT_OCTAVE;
        } else if (single_key_edge_(ktr_dev, BTN_AB12_IDX, BTN_1_MASK)) {
            /* octave down */
            ktr_dev->octave = ktr_dev->octave > MIN_OCTAVE ? ktr_dev->octave-1 : MIN_OCTAVE;
        } else if (single_key_edge_(ktr_dev, BTN_AB12_IDX, BTN_B_MASK)) {
            /* octave up */
            ktr_dev->octave = ktr_dev->octave < MAX_OCTAVE ? ktr_dev->octave+1 : MAX_OCTAVE;
        }

        if (ktr_dev->in_report[BTN_AB12_IDX] == (BTN_2_MASK|BTN_A_MASK)) {
            /* reset program */
            ktr_dev->program = 0;
        } else if (single_key_edge_(ktr_dev, BTN_AB12_IDX, BTN_A_MASK)) {
            /* program down */
            ktr_dev->program = ktr_dev->program > MIN_PROGRAM ? ktr_dev->program-1 : MIN_PROGRAM;
        } else if (single_key_edge_(ktr_dev, BTN_AB12_IDX, BTN_2_MASK)) {
            /* program up */
            ktr_dev->program = ktr_dev->program < MAX_PROGRAM ? ktr_dev->program+1 : MAX_PROGRAM;
        } else {
            /* skip updating midi program if nothing changed */
            break;
        }

        uint8_t pchange[2]= {0xC0 | ktr_dev->channel, ktr_dev->program};
        midipacket_add_(ktr_dev, timestamp, sizeof(pchange), pchange);

        /* exit block */
        break;
    }

#if 1
    /* in MIDI mode pitch bender is not reset to zero when the handle button is released */
    /* in MIDI mode modulation wheel is not reset to zero when the handle button is pressed */
#else
    if (report_idx_changed_(ktr_dev, BTN_HANDLE_IDX)) {
        if (!ktr_dev->in_report[BTN_HANDLE_IDX]) {
            uint8_t pitch_bender[3]= {0xE0 | ktr_dev->channel, 0, 0x40};
            midipacket_add_(ktr_dev, timestamp, sizeof(pitch_bender), pitch_bender);
        } else {
            uint8_t mod_wheel[3]= {0xB0 | ktr_dev->channel, 1, 0x00};
            midipacket_add_(ktr_dev, timestamp, sizeof(mod_wheel), mod_wheel);
        }
    }
#endif

    /* handle touchstrip events */
    if (report_idx_changed_(ktr_dev, MISC_TOUCHSTRIP_IDX)) {
        uint8_t val = ktr_dev->in_report[MISC_TOUCHSTRIP_IDX];
        if (ktr_dev->in_report[BTN_HANDLE_IDX]) {
            /* in MIDI mode pitch bender is 0x40 (center) when not touching the strip */
            uint8_t pitch_bender[3]= {0xE0 | ktr_dev->channel, 0, val ? val : 0x40};
            midipacket_add_(ktr_dev, timestamp, sizeof(pitch_bender), pitch_bender);
        } else if (val) {
            /* in MIDI mode modulation wheel is not reset to zero when not touching the strip */
            uint8_t mod_wheel[3]= {0xB0 | ktr_dev->channel, 1, val};
            midipacket_add_(ktr_dev, timestamp, sizeof(mod_wheel), mod_wheel);
        }
    }

    /* handle d-pad events */
    if (report_idx_changed_(ktr_dev, DPAD_STATE_IDX)) {
        uint8_t val = ktr_dev->in_report[DPAD_STATE_IDX];
        if (val == DPAD_U_VAL) {
            ktr_dev->drum_mapping = !ktr_dev->drum_mapping;
        } else if (val == DPAD_D_VAL) {
            ktr_dev->pedal_midi_ctrl = MIDI_VOLUME_CTRL;
        } else if (val == DPAD_L_VAL) {
            ktr_dev->pedal_midi_ctrl = MIDI_EXPRESSION_CTRL;
        } else if (val == DPAD_R_VAL) {
            ktr_dev->pedal_midi_ctrl = MIDI_FOOT_CTRL;
        }
    }

    /* handle pedal and switch */
    uint8_t pedal_changed_bits = report_idx_changed_bits_(ktr_dev, MISC_PEDAL_IDX);
    if (pedal_changed_bits & 0x80) {
        uint8_t sustain_pedal[3]= {0xB0 | ktr_dev->channel, 0x40,
            (ktr_dev->in_report[MISC_PEDAL_IDX] & 0x80) ? 0x7F : 0x00};
        midipacket_add_(ktr_dev, timestamp, sizeof(sustain_pedal), sustain_pedal);
    }
    if (pedal_changed_bits & 0x7F) {
        uint8_t pedal[3]= {0xB0 | ktr_dev->channel, ktr_dev->pedal_midi_ctrl,
            ktr_dev->in_report[MISC_PEDAL_IDX]};
        midipacket_add_(ktr_dev, timestamp, sizeof(pedal), pedal);
    }

send_midi_cmds:
    midipacketlist_send_(ktr_dev);
    memcpy(ktr_dev->last_in_report, ktr_dev->in_report, ktr_dev->in_report_size);
}

static Boolean IOHIDDevice_GetLongProperty(IOHIDDeviceRef inDeviceRef,
                                           CFStringRef inKey,
                                           long * outValue)
{
    Boolean result = FALSE;

    CFTypeRef tCFTypeRef = IOHIDDeviceGetProperty( inDeviceRef, inKey );
    if ( tCFTypeRef ) {
        // if this is a number
        if ( CFNumberGetTypeID( ) == CFGetTypeID( tCFTypeRef ) ) {
            // get its value
            result = CFNumberGetValue( ( CFNumberRef ) tCFTypeRef, kCFNumberSInt32Type, outValue );
        }
    }
    return result;
}

long IOHIDDevice_GetVendorID( IOHIDDeviceRef inIOHIDDeviceRef )
{
    long result = 0;
    ( void ) IOHIDDevice_GetLongProperty(inIOHIDDeviceRef,
                                         CFSTR(kIOHIDVendorIDKey), &result);
    return result;
}

long IOHIDDevice_GetProductID( IOHIDDeviceRef inIOHIDDeviceRef )
{
    long result = 0;
    ( void ) IOHIDDevice_GetLongProperty( inIOHIDDeviceRef, CFSTR(kIOHIDProductIDKey), &result );
    return result;
}

static void add_matching_device(void *inContext, IOReturn inResult,
                                void *inSender,
                                IOHIDDeviceRef inIOHIDDeviceRef) {
    OSStatus status;
    long max_report_size;
    struct rb_keytar_dev *newdev = NULL;

    /* Ignore devices we are not interested in */
    if (IOHIDDevice_GetVendorID(inIOHIDDeviceRef) != VENDOR_ID ||
        IOHIDDevice_GetProductID(inIOHIDDeviceRef) != PRODUCT_ID)
        return;

    /* Get the maximum size for the input report */
    CFTypeRef p = IOHIDDeviceGetProperty(inIOHIDDeviceRef,
                                         CFSTR(kIOHIDMaxInputReportSizeKey));
    if (!p || !CFNumberGetValue(p, kCFNumberLongType, &max_report_size))
        goto fail;

    /* Create device */
    newdev = calloc(1, sizeof(struct rb_keytar_dev));
    if (!newdev)
        goto fail;

    newdev->midi_packetlist_sz = MIDI_BUFSZ;
    newdev->midi_packetlist = calloc(1, newdev->midi_packetlist_sz);
    if (!newdev->midi_packetlist)
        goto fail;

    newdev->in_report_size = max_report_size;
    newdev->octave = DEFAULT_OCTAVE;
    newdev->program = MIN_PROGRAM;
    newdev->pedal_midi_ctrl = DEFAULT_MIDI_PEDAL_CTRL;

    newdev->in_report = calloc(2, newdev->in_report_size);
    if (!newdev->in_report)
        goto fail;
    newdev->last_in_report = newdev->in_report + newdev->in_report_size;

    status = MIDIClientCreate(client_name, NULL, NULL, &newdev->midiclient);
    if (status)
        goto fail;
    status = MIDISourceCreate (newdev->midiclient, source_name, &newdev->midiout);
    if (status)
        goto fail;
    status = MIDIOutputPortCreate(newdev->midiclient, port_name, &newdev->midiport);
    if (status)
        goto fail;

    /* Add it to our list of devices */
    newdev->prev = NULL;
    newdev->next = list_head;
    if (list_head)
        list_head->prev = newdev;
    list_head = newdev;
    if (list_tail == NULL)
        list_tail = newdev;

    newdev->io_hid_dev = inIOHIDDeviceRef;

    /* Register callback for handling input reports */
    IOHIDDeviceRegisterInputReportCallback(inIOHIDDeviceRef, newdev->in_report,
                                           newdev->in_report_size,
                                           handle_input_report,
                                           newdev);

    printf("MIDI Client for matching device created\n");
    return;

fail:
    if (newdev) {
        if (newdev->in_report)
            free(newdev->in_report);
        if (newdev->midi_packetlist)
            free(newdev->midi_packetlist);
        if (newdev->midiclient)
            MIDIClientDispose(newdev->midiclient);
        free(newdev);
    }
    printf("Error setting up matching device\n");
}

static void rm_matching_device(void *inContext, IOReturn inResult,
                               void *inSender,
                               IOHIDDeviceRef inIOHIDDeviceRef) {

    struct rb_keytar_dev *olddev = list_head;

    while (olddev) {
        if (olddev->io_hid_dev == inIOHIDDeviceRef)
            break;
        olddev = olddev->next;
    }

    if (!olddev)
        return;

    /* Unregister input report callback */
    IOHIDDeviceRegisterInputReportCallback(inIOHIDDeviceRef, olddev->in_report,
                                           olddev->in_report_size, NULL, NULL);

    /* Cleanup all MIDI related state */
    MIDIClientDispose(olddev->midiclient);

    /* Remove device form the list */
    if (olddev->next)
        olddev->next->prev = olddev->prev;
    else
        list_tail = olddev->prev;

    if (olddev->prev)
        olddev->prev->next = olddev->next;
    else
        list_head = olddev->next;

    /* Dispose of the device */
    if (olddev->in_report)
        free(olddev->in_report);
    if (olddev->midi_packetlist)
        free(olddev->midi_packetlist);
    free(olddev);

    printf("MIDI Client disposed\n");
}

int setup_hid(IOHIDManagerRef hid_manager)
{
    hid_manager = IOHIDManagerCreate(kCFAllocatorDefault,
                                     kIOHIDOptionsTypeNone);
    if (!hid_manager)
        return -ENOMEM;

    IOHIDManagerScheduleWithRunLoop(hid_manager, CFRunLoopGetCurrent(),
                                    kCFRunLoopDefaultMode);

    if (IOHIDManagerOpen(hid_manager, kIOHIDOptionsTypeNone) != kIOReturnSuccess)
        goto err;

#ifdef USE_MATCHING_DICT
    CFDictionaryRef matching_dict = create_dev_matching_dict(VENDOR_ID, PRODUCT_ID);
    if (!matching_dict)
        return -ENOMEM;
#endif

    IOHIDManagerSetDeviceMatching(hid_manager, matching_dict);

#ifdef USE_MATCHING_DICT
    CFRelease(matching_dict);
#endif

    IOHIDManagerRegisterDeviceMatchingCallback(hid_manager, add_matching_device, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(hid_manager, rm_matching_device, NULL);

    return 0;

err:
    if (hid_manager) {
        IOHIDManagerClose(hid_manager, kIOHIDOptionsTypeNone);
        IOHIDManagerUnscheduleFromRunLoop(hid_manager,
                                          CFRunLoopGetCurrent(),
                                          kCFRunLoopDefaultMode);
        CFRelease(hid_manager);
    }
    return -ENODEV;
}

void teardown_hid(IOHIDManagerRef hid_manager)
{
    if (hid_manager) {
        IOHIDManagerClose(hid_manager, kIOHIDOptionsTypeNone);
        IOHIDManagerUnscheduleFromRunLoop(hid_manager,
                                          CFRunLoopGetCurrent(),
                                          kCFRunLoopDefaultMode);
        /* TODO: Free all devices */
        CFRelease(hid_manager);
    }
}
