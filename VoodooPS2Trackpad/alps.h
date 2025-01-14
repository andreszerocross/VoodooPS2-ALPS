/*
 * Copyright (c) 2002 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.2 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 *
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include "ApplePS2MouseDevice.h"
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/hidsystem/IOHIPointing.h>
#include <IOKit/IOCommandGate.h>
#include "VoodooPS2Common.h"

#include "VoodooInputMultitouch/VoodooInputEvent.h"

// #include "../VoodooInput/VoodooInput/VoodooInputMultitouch/VoodooInputTransducer.h"
// #include "../VoodooInput/VoodooInput/VoodooInputMultitouch/VoodooInputMessages.h"
// #include "../VoodooInput/VoodooInput/VoodooInputMultitouch/VoodooInputEvent.h"

#define ALPS_PROTO_V1             0x100
#define ALPS_PROTO_V2             0x200
#define ALPS_PROTO_V3             0x300
#define ALPS_PROTO_V3_RUSHMORE    0x310
#define ALPS_PROTO_V4             0x400
#define ALPS_PROTO_V5             0x500
#define ALPS_PROTO_V6             0x600
#define ALPS_PROTO_V7             0x700    /* t3btl t4s */
#define ALPS_PROTO_V8             0x800    /* SS4btl SS4s */
#define ALPS_PROTO_V9             0x900    /* ss3btl */

#define MAX_TOUCHES     5

#define DOLPHIN_COUNT_PER_ELECTRODE	64
#define DOLPHIN_PROFILE_XOFFSET		8	/* x-electrode offset */
#define DOLPHIN_PROFILE_YOFFSET		1	/* y-electrode offset */

// TODO: Remove or move?
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// SimpleAverage Class Declaration
//

template <class T, int N>
class SimpleAverage
{
private:
    T m_buffer[N];
    int m_count;
    int m_sum;
    int m_index;
    
public:
    inline SimpleAverage() { reset(); }
    T filter(T data)
    {
        // add new entry to sum
        m_sum += data;
        // if full buffer, then we are overwriting, so subtract old from sum
        if (m_count == N)
            m_sum -= m_buffer[m_index];
        // new entry into buffer
        m_buffer[m_index] = data;
        // move index to next position with wrap around
        if (++m_index >= N)
            m_index = 0;
        // keep count moving until buffer is full
        if (m_count < N)
            ++m_count;
        // return average of current items
        return m_sum / m_count;
    }
    inline void reset()
    {
        m_count = 0;
        m_sum = 0;
        m_index = 0;
    }
    inline int count() const { return m_count; }
    inline int sum() const { return m_sum; }
    T oldest() const
    {
        // undefined if nothing in here, return zero
        if (m_count == 0)
            return 0;
        // if it is not full, oldest is at index 0
        // if full, it is right where the next one goes
        if (m_count < N)
            return m_buffer[0];
        else
            return m_buffer[m_index];
    }
    T newest() const
    {
        // undefined if nothing in here, return zero
        if (m_count == 0)
            return 0;
        // newest is index - 1, with wrap
        int index = m_index;
        if (--index < 0)
            index = m_count-1;
        return m_buffer[index];
    }
    T average() const
    {
        if (m_count == 0)
            return 0;
        return m_sum / m_count;
    }
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// DecayingAverage Class Declaration
//

template <class T, class TT, int N1, int N2, int D>
class DecayingAverage
{
private:
    T m_last;
    bool m_lastvalid;
    
public:
    inline DecayingAverage() { reset(); }
    T filter(T data, int fingers)
    {
        TT result = data;
        TT last = m_last;
        if (m_lastvalid)
            result = (result * N1) / D + (last * N2) / D;
        m_lastvalid = true;
        m_last = (T)result;
        return m_last;
    }
    inline void reset()
    {
        m_lastvalid = false;
    }
};

template <class T, class TT, int N1, int N2, int D>
class UndecayAverage
{
private:
    T m_last;
    bool m_lastvalid;
    
public:
    inline UndecayAverage() { reset(); }
    T filter(T data)
    {
        TT result = data;
        TT last = m_last;
        if (m_lastvalid)
            result = (result * D) / N1 - (last * N2) / N1;
        m_lastvalid = true;
        m_last = (T)data;
        return (T)result;
    }
    inline void reset()
    {
        m_lastvalid = false;
    }
};

/*
 * enum SS4_PACKET_ID - defines the packet type for V8
 * SS4_PACKET_ID_IDLE: There's no finger and no button activity.
 * SS4_PACKET_ID_ONE: There's one finger on touchpad
 *  or there's button activities.
 * SS4_PACKET_ID_TWO: There's two or more fingers on touchpad
 * SS4_PACKET_ID_MULTI: There's three or more fingers on touchpad
 * SS4_PACKET_ID_STICK: A stick pointer packet
 */
enum SS4_PACKET_ID {
    SS4_PACKET_ID_IDLE = 0,
    SS4_PACKET_ID_ONE,
    SS4_PACKET_ID_TWO,
    SS4_PACKET_ID_MULTI,
    SS4_PACKET_ID_STICK,
};

#define SS4_COUNT_PER_ELECTRODE        256
#define SS4_NUMSENSOR_XOFFSET        7
#define SS4_NUMSENSOR_YOFFSET        7
#define SS4_MIN_PITCH_MM        50

#define SS4_MASK_NORMAL_BUTTONS        0x07

#define SS4PLUS_COUNT_PER_ELECTRODE    128
#define SS4PLUS_NUMSENSOR_XOFFSET    16
#define SS4PLUS_NUMSENSOR_YOFFSET    5
#define SS4PLUS_MIN_PITCH_MM        37

#define IS_SS4PLUS_DEV(_b)    (((_b[0]) == 0x73) &&    \
                 ((_b[1]) == 0x03) &&    \
                 ((_b[2]) == 0x28)        \
                )

#define SS4_IS_IDLE_V2(_b)    (((_b[0]) == 0x18) &&        \
                 ((_b[1]) == 0x10) &&        \
                 ((_b[2]) == 0x00) &&        \
                 ((_b[3] & 0x88) == 0x08) &&    \
                 ((_b[4]) == 0x10) &&        \
                 ((_b[5]) == 0x00)        \
                )

#define SS4_1F_X_V2(_b)        (((_b[0]) & 0x0007) |        \
                 ((_b[1] << 3) & 0x0078) |    \
                 ((_b[1] << 2) & 0x0380) |    \
                 ((_b[2] << 5) & 0x1C00)    \
                )

#define SS4_1F_Y_V2(_b)        (((_b[2]) & 0x000F) |        \
                 ((_b[3] >> 2) & 0x0030) |    \
                 ((_b[4] << 6) & 0x03C0) |    \
                 ((_b[4] << 5) & 0x0C00)    \
                )

#define SS4_1F_Z_V2(_b)        (((_b[5]) & 0x0F) |        \
                 ((_b[5] >> 1) & 0x70) |    \
                 ((_b[4]) & 0x80)        \
                )

#define SS4_1F_LFB_V2(_b)    (((_b[2] >> 4) & 0x01) == 0x01)

#define SS4_MF_LF_V2(_b, _i)    ((_b[1 + (_i) * 3] & 0x0004) == 0x0004)

#define SS4_BTN_V2(_b)        ((_b[0] >> 5) & SS4_MASK_NORMAL_BUTTONS)

#define SS4_STD_MF_X_V2(_b, _i)    (((_b[0 + (_i) * 3] << 5) & 0x00E0) |    \
                 ((_b[1 + _i * 3]  << 5) & 0x1F00)    \
                )

#define SS4_PLUS_STD_MF_X_V2(_b, _i) (((_b[0 + (_i) * 3] << 4) & 0x0070) | \
                 ((_b[1 + (_i) * 3]  << 4) & 0x0F80)    \
                )

#define SS4_STD_MF_Y_V2(_b, _i)    (((_b[1 + (_i) * 3] << 3) & 0x0010) |    \
                 ((_b[2 + (_i) * 3] << 5) & 0x01E0) |    \
                 ((_b[2 + (_i) * 3] << 4) & 0x0E00)    \
                )

#define SS4_BTL_MF_X_V2(_b, _i)    (SS4_STD_MF_X_V2(_b, _i) |        \
                 ((_b[0 + (_i) * 3] >> 3) & 0x0010)    \
                )

#define SS4_PLUS_BTL_MF_X_V2(_b, _i) (SS4_PLUS_STD_MF_X_V2(_b, _i) |    \
                 ((_b[0 + (_i) * 3] >> 4) & 0x0008)    \
                )

#define SS4_BTL_MF_Y_V2(_b, _i)    (SS4_STD_MF_Y_V2(_b, _i) | \
                 ((_b[0 + (_i) * 3] >> 3) & 0x0008)    \
                )

#define SS4_MF_Z_V2(_b, _i)    (((_b[1 + (_i) * 3]) & 0x0001) |    \
                 ((_b[1 + (_i) * 3] >> 1) & 0x0002)    \
                )

#define SS4_IS_MF_CONTINUE(_b)    ((_b[2] & 0x10) == 0x10)
#define SS4_IS_5F_DETECTED(_b)    ((_b[2] & 0x10) == 0x10)

#define SS4_TS_X_V2(_b)        (int)(                \
                 ((_b[0] & 0x01) << 7) |    \
                 (_b[1] & 0x7F)        \
                )

#define SS4_TS_Y_V2(_b)        -(int)(                \
                 ((_b[3] & 0x01) << 7) |    \
                 (_b[2] & 0x7F)        \
                )

#define SS4_TS_Z_V2(_b)        (int)(_b[4] & 0x7F)


#define SS4_MFPACKET_NO_AX        8160    /* X-Coordinate value */
#define SS4_MFPACKET_NO_AY        4080    /* Y-Coordinate value */
#define SS4_MFPACKET_NO_AX_BL        8176    /* Buttonless X-Coord value */
#define SS4_MFPACKET_NO_AY_BL        4088    /* Buttonless Y-Coord value */
#define SS4_PLUS_MFPACKET_NO_AX        4080    /* SS4 PLUS, X */
#define SS4_PLUS_MFPACKET_NO_AX_BL    4088    /* Buttonless SS4 PLUS, X */

// TODO: Move to different place
struct alps_hw_state {
     int x;
     int y;
     int z;
     int virtualFingerIndex;
 };

struct virtual_finger_state {
     SimpleAverage<int, 5> x_avg;
     SimpleAverage<int, 5> y_avg;
     uint8_t pressure;
     bool touch;
     bool button;
     MT2FingerType fingerType;
 };

/*
 * enum V7_PACKET_ID - defines the packet type for V7
 * V7_PACKET_ID_IDLE: There's no finger and no button activity.
 * V7_PACKET_ID_TWO: There's one or two non-resting fingers on touchpad
 *  or there's button activities.
 * V7_PACKET_ID_MULTI: There are at least three non-resting fingers.
 * V7_PACKET_ID_NEW: The finger position in slot is not continues from
 *  previous packet.
 */
enum V7_PACKET_ID {
    V7_PACKET_ID_IDLE,
    V7_PACKET_ID_TWO,
    V7_PACKET_ID_MULTI,
    V7_PACKET_ID_NEW,
    V7_PACKET_ID_UNKNOWN,
};

/**
 * struct alps_protocol_info - information about protocol used by a device
 * @version: Indicates V1/V2/V3/...
 * @byte0: Helps figure out whether a position report packet matches the
 *   known format for this model.  The first byte of the report, ANDed with
 *   mask0, should match byte0.
 * @mask0: The mask used to check the first byte of the report.
 * @flags: Additional device capabilities (passthrough port, trackstick, etc.).
 */
struct alps_protocol_info {
    UInt16 version;
    UInt8 byte0, mask0;
    unsigned int flags;
};

/**
 * struct alps_model_info - touchpad ID table
 * @signature: E7 response string to match.
 * @protocol_info: information about protocol used by the device.
 *
 * Many (but not all) ALPS touchpads can be identified by looking at the
 * values returned in the "E7 report" and/or the "EC report."  This table
 * lists a number of such touchpads.
 */
struct alps_model_info {
    UInt8 signature[3];
    struct alps_protocol_info protocol_info;
};

/**
 * struct alps_nibble_commands - encodings for register accesses
 * @command: PS/2 command used for the nibble
 * @data: Data supplied as an argument to the PS/2 command, if applicable
 *
 * The ALPS protocol uses magic sequences to transmit binary data to the
 * touchpad, as it is generally not OK to send arbitrary bytes out the
 * PS/2 port.  Each of the sequences in this table sends one nibble of the
 * register address or (write) data.  Different versions of the ALPS protocol
 * use slightly different encodings.
 */
struct alps_nibble_commands {
    SInt32 command;
    UInt8 data;
};

struct alps_bitmap_point {
    int start_bit;
    int num_bits;
};

struct input_mt_pos {
    UInt32 x;
    UInt32 y;
};

/**
 * struct alps_fields - decoded version of the report packet
 * @x_map: Bitmap of active X positions for MT.
 * @y_map: Bitmap of active Y positions for MT.
 * @fingers: Number of fingers for MT.
 * @pressure: Pressure.
 * @st: position for ST.
 * @mt: position for MT.
 * @first_mp: Packet is the first of a multi-packet report.
 * @is_mp: Packet is part of a multi-packet report.
 * @left: Left touchpad button is active.
 * @right: Right touchpad button is active.
 * @middle: Middle touchpad button is active.
 * @ts_left: Left trackstick button is active.
 * @ts_right: Right trackstick button is active.
 * @ts_middle: Middle trackstick button is active.
 */
struct alps_fields {
    unsigned int x_map;
    unsigned int y_map;
    unsigned int fingers;

    int pressure;
    struct input_mt_pos st;
    struct input_mt_pos mt[MAX_TOUCHES];

    unsigned int first_mp:1;
    unsigned int is_mp:1;

    unsigned int left:1;
    unsigned int right:1;
    unsigned int middle:1;

    unsigned int ts_left:1;
    unsigned int ts_right:1;
    unsigned int ts_middle:1;
};

class ALPS;

/**
 * struct alps_data - private data structure for the ALPS driver
 * @nibble_commands: Command mapping used for touchpad register accesses.
 * @addr_command: Command used to tell the touchpad that a register address
 *   follows.
 * @proto_version: Indicates V1/V2/V3/...
 * @byte0: Helps figure out whether a position report packet matches the
 *   known format for this model.  The first byte of the report, ANDed with
 *   mask0, should match byte0.
 * @mask0: The mask used to check the first byte of the report.
 * @fw_ver: cached copy of firmware version (EC report)
 * @flags: Additional device capabilities (passthrough port, trackstick, etc.).
 * @x_max: Largest possible X position value.
 * @y_max: Largest possible Y position value.
 * @x_bits: Number of X bits in the MT bitmap.
 * @y_bits: Number of Y bits in the MT bitmap.
 * @prev_fin: Finger bit from previous packet.
 * @multi_packet: Multi-packet data in progress.
 * @multi_data: Saved multi-packet data.
 * @f: Decoded packet data fields.
 * @quirks: Bitmap of ALPS_QUIRK_*.
 */
struct alps_data {
    /* these are autodetected when the device is identified */
    const struct alps_nibble_commands *nibble_commands;
    SInt32 addr_command;
    UInt16 proto_version;
    UInt8 byte0, mask0;
    UInt8 dev_id[3];
    UInt8 fw_ver[3];
    int flags;
    SInt32 x_max;
    SInt32 y_max;
    SInt32 x_bits;
    SInt32 y_bits;
    unsigned int x_res;
    unsigned int y_res;
    
    SInt32 prev_fin;
    SInt32 multi_packet;
    int second_touch;
    UInt8 multi_data[6];
    struct alps_fields f;
    UInt8 quirks;
    bool PSMOUSE_BAD_DATA;
    
    int pktsize = 6;
};

// Pulled out of alps_data, now saved as vars on class
// makes invoking a little easier
typedef bool (ALPS::*hw_init)();
typedef bool (ALPS::*decode_fields)(struct alps_fields *f, UInt8 *p);
typedef void (ALPS::*process_packet)(UInt8 *packet);
//typedef void (ALPS::*set_abs_params)();

#define ALPS_QUIRK_TRACKSTICK_BUTTONS	1 /* trakcstick buttons in trackstick packet */

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// ALPS Class Declaration
//

typedef struct ALPSStatus {
    UInt8 bytes[3];
} ALPSStatus_t;

#define XMIN 0
#define XMAX 6143
#define YMIN 0
#define YMAX 6143
#define XMIN_NOMINAL 1472
#define XMAX_NOMINAL 5472
#define YMIN_NOMINAL 1408
#define YMAX_NOMINAL 4448

#define ABS_POS_BITS 13
#define X_MAX_POSITIVE 8176
#define Y_MAX_POSITIVE 8176

#define kPacketLength 6
#define kPacketLengthSmall  3
#define kPacketLengthLarge  6
#define kPacketLengthMax    6
#define kDP_CommandNibble10 0xf2
#define BITS_PER_BYTE 8

// predeclure stuff
struct alps_data;

class EXPORT ALPS : public IOHIPointing {
    typedef IOHIPointing super;
        OSDeclareDefaultStructors( ALPS );
    
private:
    IOService *voodooInputInstance;
    VoodooInputEvent inputEvent;
    
    alps_data priv;
    hw_init hw_init;
    decode_fields decode_fields;
    process_packet process_packet;
    //    set_abs_params set_abs_params;
    
public:
    ALPS * probe(IOService *provider, SInt32 *score) override;
    
    bool init(OSDictionary * dict) override;
    
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    UInt32 deviceType() override;
    UInt32 interfaceID() override;

    IOReturn setParamProperties(OSDictionary * dict) override;
    IOReturn setProperties(OSObject *props) override;
    
    // Acidanthera VoodooPS2
    bool handleOpen(IOService *forClient, IOOptionBits options, void *arg) override;
    void handleClose(IOService *forClient, IOOptionBits options) override;
    
protected:
    int _multiPacket;
    
    UInt8 _multiData[6];
    
    IOGBounds _bounds;
    
    virtual bool deviceSpecificInit();
    
    bool resetMouse();
    
    void alps_process_packet_v1_v2(UInt8 *packet);
    
    int alps_process_bitmap(struct alps_data *priv, struct alps_fields *f);
    
    void alps_process_trackstick_packet_v3(UInt8 * packet);
    
    bool alps_decode_buttons_v3(struct alps_fields *f, UInt8 *p);
    
    bool alps_decode_pinnacle(struct alps_fields *f, UInt8 *p);
    
    bool alps_decode_rushmore(struct alps_fields *f, UInt8 *p);
    
    bool alps_decode_dolphin(struct alps_fields *f, UInt8 *p);
    
    void alps_process_touchpad_packet_v3_v5(UInt8 * packet);
    
    void alps_process_packet_v3(UInt8 *packet);
    
    void alps_process_packet_v6(UInt8 *packet);
    
    void alps_process_packet_v4(UInt8 *packet);
    
    unsigned char alps_get_packet_id_v7(UInt8 *byte);
    
    void alps_get_finger_coordinate_v7(struct input_mt_pos *mt, UInt8 *pkt, UInt8 pkt_id);
    
    int alps_get_mt_count(struct input_mt_pos *mt);
    
    bool alps_decode_packet_v7(struct alps_fields *f, UInt8 *p);
    
    void alps_process_trackstick_packet_v7(UInt8 *packet);
    
    void alps_process_touchpad_packet_v7(UInt8 *packet);
    
    void alps_process_packet_v7(UInt8 *packet);
    
    unsigned char alps_get_pkt_id_ss4_v2(UInt8 *byte);
    
    bool alps_decode_ss4_v2(struct alps_fields *f, UInt8 *p);
    
    void alps_process_packet_ss4_v2(UInt8 *packet);
    
    void setTouchPadEnable(bool enable);
    
    PS2InterruptResult interruptOccurred(UInt8 data);
    
    void packetReady();
    
    bool alps_command_mode_send_nibble(int value);
    
    bool alps_command_mode_set_addr(int addr);
    
    int alps_command_mode_read_reg(int addr);
    
    bool alps_command_mode_write_reg(int addr, UInt8 value);
    
    bool alps_command_mode_write_reg(UInt8 value);
    
    bool alps_rpt_cmd(SInt32 init_command, SInt32 init_arg, SInt32 repeated_command, ALPSStatus_t *report);
    
    bool alps_enter_command_mode();
    
    bool alps_exit_command_mode();
    
    bool alps_passthrough_mode_v2(bool enable);
        
    bool alps_absolute_mode_v1_v2();
    
    int alps_monitor_mode_send_word(int word);
    
    int alps_monitor_mode_write_reg(int addr, int value);
    
    int alps_monitor_mode(bool enable);
    
    void alps_absolute_mode_v6();
    
    bool alps_get_status(ALPSStatus_t *status);
    
    bool alps_tap_mode(bool enable);
    
    bool alps_hw_init_v1_v2();
    
    bool alps_hw_init_v6();
    
    bool alps_passthrough_mode_v3(int regBase, bool enable);
    
    bool alps_absolute_mode_v3();
    
    IOReturn alps_probe_trackstick_v3_v7(int regBase);
    
    IOReturn alps_setup_trackstick_v3(int regBase);
    
    bool alps_hw_init_v3();
    
    bool alps_get_v3_v7_resolution(int reg_pitch);
    
    bool alps_hw_init_rushmore_v3();
    
    bool alps_absolute_mode_v4();
    
    bool alps_hw_init_v4();
    
    void alps_get_otp_values_ss4_v2(unsigned char index, unsigned char otp[]);
    
    void alps_update_device_area_ss4_v2(unsigned char otp[][4], struct alps_data *priv);
    
    void alps_update_btn_info_ss4_v2(unsigned char otp[][4], struct alps_data *priv);
    
    void alps_update_dual_info_ss4_v2(unsigned char otp[][4], struct alps_data *priv);
    
    void alps_set_defaults_ss4_v2(struct alps_data *priv);
    
    int alps_dolphin_get_device_area(struct alps_data *priv);
    
    bool alps_hw_init_dolphin_v1();
    
    bool alps_hw_init_v7();
    
    bool alps_hw_init_ss4_v2();
    
    void ps2_command_short(UInt8 command);
    
    void ps2_command(unsigned char value, UInt8 command);
        
    void set_protocol();
    
    bool matchTable(ALPSStatus_t *e7, ALPSStatus_t *ec);
    
    IOReturn identify();
    
    void restart();
    
    ApplePS2MouseDevice * _device;
    bool                _interruptHandlerInstalled;
    bool                _powerControlHandlerInstalled;
    bool                _messageHandlerInstalled;
    RingBuffer<UInt8, kPacketLength*32> _ringBuffer;
    UInt32              _packetByteCount;
    UInt8               _lastdata;
    UInt16              _touchPadVersion;

    IOCommandGate*      _cmdGate;
    
    int skippyThresh;
    int lastdx;
    int lastdy;
    
    OSArray* transducers;
    
    // buttons and scroll wheel
    unsigned int left:1;
    unsigned int right:1;
    unsigned int middle:1;
    
    unsigned int left_ts:1;
    
    int margin_size_x, margin_size_y;
    
    uint32_t logical_max_x;
    uint32_t logical_max_y;
    uint32_t logical_min_x;
    uint32_t logical_min_y;
    
    uint32_t physical_max_x;
    uint32_t physical_max_y;
    
    struct alps_hw_state fingerStates[MAX_TOUCHES];
    struct virtual_finger_state virtualFingerStates[MAX_TOUCHES];
    bool freeFingerTypes[kMT2FingerTypeCount];
    
    static_assert(MAX_TOUCHES <= kMT2FingerTypeLittleFinger, "Too many fingers for one hand");
    
    void assignVirtualFinger(int physicalFinger);
    void assignFingerType(virtual_finger_state &vf);
    int lastFingerCount;
    int lastSentFingerCount;
    bool hadLiftFinger;
    
    int upperFingerIndex() const;
    const alps_hw_state& upperFinger() const;
    void swapFingers(int dst, int src);
    void alps_parse_hw_state(const UInt8 buf[], struct alps_fields &f);
    
    /// Translates physical fingers into virtual fingers so that host software doesn't see 'jumps' and has coordinates for all fingers.
    /// @return True if is ready to send finger state to host interface
    bool renumberFingers();
    void sendTouchData();
    void freeAndMarkVirtualFingers();
    int dist(int physicalFinger, int virtualFinger);
    
    void set_resolution();
    
    ForceTouchMode _forceTouchMode;
    int _forceTouchPressureThreshold;
    
    int _forceTouchCustomDownThreshold;
    int _forceTouchCustomUpThreshold;
    int _forceTouchCustomPower;
    
    int clampedFingerCount;
    bool wasSkipped;
    int z_finger;
    int threefingervertswipe;
    int threefingerhorizswipe;
    int draglocktemp;
    int noled;
    uint64_t maxaftertyping;
    int mousemultiplierx, mousemultipliery;
    int wakedelay;
    int skippassthru;
    int _resolution, _scrollresolution;
    int _buttonCount;
    int ignoredeltasstart;
    int minXOverride, minYOverride, maxXOverride, maxYOverride;
    int bogusdxthresh, bogusdythresh;
    int manual_x_log, manual_y_log;
    int manual_x_phy, manual_y_phy;
    
    int rightclick_corner;

    // three finger and four finger state
    uint8_t inSwipeLeft, inSwipeRight;
    uint8_t inSwipeUp, inSwipeDown;
    uint8_t inSwipe4Left, inSwipe4Right;
    uint8_t inSwipe4Up, inSwipe4Down;
    int xmoved, ymoved;

    // state related to secondary packets/extendedwmode
    int lastx2, lasty2;
    bool tracksecondary;
    int xrest2, yrest2;
    bool clickedprimary;
    bool _extendedwmode;

    // normal state
    int lastx, lasty, last_fingers, b4last;
    UInt32 lastbuttons;
    UInt32 lastTrackStickButtons, lastTouchpadButtons;
    int ignoredeltas;
    int ignoresingle;
    int xrest, yrest, scrollrest;
    int touchx, touchy;
    uint64_t touchtime;
    uint64_t untouchtime;
    bool wasdouble,wastriple;
    bool scrolldebounce;
    uint64_t keytime;
    bool ignoreall;
    UInt32 passbuttons;
#ifdef SIMULATE_PASSTHRU
    UInt32 trackbuttons;
#endif
    bool passthru;
    bool ledpresent;
    bool _reportsv;
    int clickpadtype;   //0=not, 1=1button, 2=2button, 3=reserved
    UInt32 _clickbuttons;  //clickbuttons to merge into buttons
    bool usb_mouse_stops_trackpad;

    int _modifierdown; // state of left+right control keys

    // for scaling x/y values
    int xupmm, yupmm;

    // for middle button simulation
    enum mbuttonstate
    {
        STATE_NOBUTTONS,
        STATE_MIDDLE,
        STATE_WAIT4TWO,
        STATE_WAIT4NONE,
        STATE_NOOP,
    } _mbuttonstate;

    UInt32 _pendingbuttons;
    uint64_t _buttontime;
    IOTimerEventSource* _buttonTimer;
    uint64_t _maxmiddleclicktime;

    // momentum scroll state
    bool wasScroll = false;
    SimpleAverage<int, 32> dy_history;
    SimpleAverage<uint64_t, 32> time_history;
    IOTimerEventSource* scrollTimer;

    // timer for drag delay
    IOTimerEventSource* dragTimer;
    
    IOTimerEventSource* scrollDebounceTIMER;
    
    SimpleAverage<int, 5> x_avg;
    SimpleAverage<int, 5> y_avg;
    //DecayingAverage<int, int64_t, 1, 1, 2> x_avg;
    //DecayingAverage<int, int64_t, 1, 1, 2> y_avg;
    UndecayAverage<int, int64_t, 1, 1, 2> x_undo;
    UndecayAverage<int, int64_t, 1, 1, 2> y_undo;

    SimpleAverage<int, 5> x2_avg;
    SimpleAverage<int, 5> y2_avg;
    //DecayingAverage<int, int64_t, 1, 1, 2> x2_avg;
    //DecayingAverage<int, int64_t, 1, 1, 2> y2_avg;
    UndecayAverage<int, int64_t, 1, 1, 2> x2_undo;
    UndecayAverage<int, int64_t, 1, 1, 2> y2_undo;
    
    const char* modeName(int touchmode);

    virtual void   setDevicePowerState(UInt32 whatToDo);

    virtual void   receiveMessage(int message, void* data);

    virtual void touchpadToggled() {};
    virtual void touchpadShutdown() {};
    virtual void initTouchPad();

    inline bool isFingerTouch(int z) { return z>z_finger; }

    enum MBComingFrom { fromPassthru, fromTimer, fromTrackpad, fromCancel };
    UInt32 middleButton(UInt32 buttons, uint64_t now, MBComingFrom from);

    virtual void setParamPropertiesGated(OSDictionary* dict);

    IOItemCount buttonCount() override;
    IOFixed     resolution() override;
    inline void dispatchRelativePointerEventX(int dx, int dy, UInt32 buttonState, uint64_t now)
        { dispatchRelativePointerEvent(dx, dy, buttonState, *(AbsoluteTime*)&now); }
    inline void dispatchScrollWheelEventX(short deltaAxis1, short deltaAxis2, short deltaAxis3, uint64_t now)
        { dispatchScrollWheelEvent(deltaAxis1, deltaAxis2, deltaAxis3, *(AbsoluteTime*)&now); }
};
