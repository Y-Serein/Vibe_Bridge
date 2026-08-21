#ifndef _USB_DESCRIPTORS_H_
#define _USB_DESCRIPTORS_H_

/* Time stamp base clock. It is a deprecated parameter. */
#define UVC_CLOCK_FREQUENCY 27000000
/* video capture path */
#define UVC_ENTITY_CAP_INPUT_TERMINAL  0x01
#define UVC_ENTITY_CAP_OUTPUT_TERMINAL 0x02

#define FRAME_WIDTH  160
#define FRAME_HEIGHT 120
#define FRAME_RATE   10
#define FRAME_SIZE   (FRAME_WIDTH * FRAME_HEIGHT * 2)

#ifndef UVC_MJPEG640_STATIC_TEST
#    define UVC_MJPEG640_STATIC_TEST 0
#endif

#define UVC_FORMAT_INDEX_YUY2        1
#define UVC_FORMAT_INDEX_MJPEG_TEST  2
#define UVC_MJPEG_TEST_WIDTH         640
#define UVC_MJPEG_TEST_HEIGHT        480
#define UVC_MJPEG_TEST_RATE          10
#define UVC_MJPEG_TEST_MAX_FRAME_SIZE \
    (UVC_MJPEG_TEST_WIDTH * UVC_MJPEG_TEST_HEIGHT * 2)

/* Optional VideoControl status endpoint.  Some Android Serenegiant/libuvc
 * builds require it even though hosts such as Windows and Linux accept a VC
 * interface without one.  Match the Linux UVC gadget values. */
#define UVC_STATUS_MAX_PACKET_SIZE 16
#define UVC_STATUS_INTERVAL        8

#define CFG_EXAMPLE_VIDEO_DISABLE_MJPEG
#define CFG_EXAMPLE_VIDEO_READONLY

enum {
    ITF_NUM_VIDEO_CONTROL,
    ITF_NUM_VIDEO_STREAMING,
    ITF_NUM_TOTAL
};

#define TUD_VIDEO_DESC_STD_VC_INT_EP_LEN 7
#define TUD_VIDEO_DESC_CS_VC_INT_EP_LEN  5

/* TinyUSB 2.2.0 emits interface protocol 0x01 (UVC 1.5) from its generic
 * descriptor macros, while this firmware advertises bcdUVC 1.10.  Keep the
 * on-wire declaration internally consistent by using the pre-1.5 undefined
 * interface protocol required by UVC 1.1. */
#define TUD_VIDEO_DESC_STD_VC_UVC11(_itfnum, _nEPs, _stridx) \
    TUD_VIDEO_DESC_STD_VC_LEN, TUSB_DESC_INTERFACE, _itfnum, 0x00, \
        _nEPs, TUSB_CLASS_VIDEO, VIDEO_SUBCLASS_CONTROL, \
        VIDEO_ITF_PROTOCOL_15, _stridx

#define TUD_VIDEO_DESC_STD_VS_UVC11(_itfnum, _alt, _epn, _stridx) \
    TUD_VIDEO_DESC_STD_VS_LEN, TUSB_DESC_INTERFACE, _itfnum, _alt, \
        _epn, TUSB_CLASS_VIDEO, VIDEO_SUBCLASS_STREAMING, \
        VIDEO_ITF_PROTOCOL_15, _stridx

#define TUD_VIDEO_CAPTURE_DESC_UNCOMPR_LEN \
    (TUD_VIDEO_DESC_IAD_LEN /* control */ \
     + TUD_VIDEO_DESC_STD_VC_LEN \
     + (TUD_VIDEO_DESC_CS_VC_LEN + 1 /*bInCollection*/) \
     + TUD_VIDEO_DESC_CAMERA_TERM_LEN \
     + TUD_VIDEO_DESC_OUTPUT_TERM_LEN /* Interface 1, Alternate 0 */ \
     + TUD_VIDEO_DESC_STD_VS_LEN \
     + (TUD_VIDEO_DESC_CS_VS_IN_LEN + 1 /*bNumFormats x bControlSize*/) \
     + TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR_LEN \
     + TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_CONT_LEN \
     + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN /* Interface 1, Alternate 1 */ \
     + TUD_VIDEO_DESC_STD_VS_LEN + 7 /* Endpoint */ \
    )

#define TUD_VIDEO_CAPTURE_DESC_UNCOMPR_BULK_LEN \
    (TUD_VIDEO_DESC_IAD_LEN /* control */ \
     + TUD_VIDEO_DESC_STD_VC_LEN \
     + (TUD_VIDEO_DESC_CS_VC_LEN + 1 /*bInCollection*/) \
     + TUD_VIDEO_DESC_CAMERA_TERM_LEN \
     + TUD_VIDEO_DESC_OUTPUT_TERM_LEN /* Interface 1, Alternate 0 */ \
     + TUD_VIDEO_DESC_STD_VS_LEN \
     + (TUD_VIDEO_DESC_CS_VS_IN_LEN + 1 /*bNumFormats x bControlSize*/) \
     + TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR_LEN \
     + (TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_DISC_LEN + 4) \
     + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN \
     + 7 /* Endpoint */ \
    )

#define TUD_VIDEO_CAPTURE_DESC_UNCOMPR_MJPEG_BULK_LEN \
    (TUD_VIDEO_DESC_IAD_LEN /* control */ \
     + TUD_VIDEO_DESC_STD_VC_LEN \
     + (TUD_VIDEO_DESC_CS_VC_LEN + 1 /*bInCollection*/) \
     + TUD_VIDEO_DESC_CAMERA_TERM_LEN \
     + TUD_VIDEO_DESC_OUTPUT_TERM_LEN \
     + TUD_VIDEO_DESC_STD_VC_INT_EP_LEN \
     + TUD_VIDEO_DESC_CS_VC_INT_EP_LEN /* Interface 1, Alternate 0 */ \
     + TUD_VIDEO_DESC_STD_VS_LEN \
     + (TUD_VIDEO_DESC_CS_VS_IN_LEN + 2 /* one control byte per format */) \
     + TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR_LEN \
     + (TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_DISC_LEN + 4) \
     + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN \
     + TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN \
     + (TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_DISC_LEN + 4) \
     + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN \
     + 7 /* Endpoint */ \
    )

#define TUD_VIDEO_CAPTURE_DESC_MJPEG_LEN \
    (TUD_VIDEO_DESC_IAD_LEN /* control */ \
     + TUD_VIDEO_DESC_STD_VC_LEN \
     + (TUD_VIDEO_DESC_CS_VC_LEN + 1 /*bInCollection*/) \
     + TUD_VIDEO_DESC_CAMERA_TERM_LEN \
     + TUD_VIDEO_DESC_OUTPUT_TERM_LEN /* Interface 1, Alternate 0 */ \
     + TUD_VIDEO_DESC_STD_VS_LEN \
     + (TUD_VIDEO_DESC_CS_VS_IN_LEN + 1 /*bNumFormats x bControlSize*/) \
     + TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN \
     + TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_LEN \
     + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN /* Interface 1, Alternate 1 */ \
     + TUD_VIDEO_DESC_STD_VS_LEN + 7 /* Endpoint */ \
    )

/* Windows support YUY2 and NV12
 * https://docs.microsoft.com/en-us/windows-hardware/drivers/stream/usb-video-class-driver-overview
 */

#define TUD_VIDEO_DESC_CS_VS_FMT_YUY2( \
    _fmtidx, _numfmtdesc, _frmidx, _asrx, _asry, _interlace, _cp) \
    TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR(_fmtidx, \
                                     _numfmtdesc, \
                                     TUD_VIDEO_GUID_YUY2, \
                                     16, \
                                     _frmidx, \
                                     _asrx, \
                                     _asry, \
                                     _interlace, \
                                     _cp)
#define TUD_VIDEO_DESC_CS_VS_FMT_NV12( \
    _fmtidx, _numfmtdesc, _frmidx, _asrx, _asry, _interlace, _cp) \
    TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR(_fmtidx, \
                                     _numfmtdesc, \
                                     TUD_VIDEO_GUID_NV12, \
                                     12, \
                                     _frmidx, \
                                     _asrx, \
                                     _asry, \
                                     _interlace, \
                                     _cp)
#define TUD_VIDEO_DESC_CS_VS_FMT_M420( \
    _fmtidx, _numfmtdesc, _frmidx, _asrx, _asry, _interlace, _cp) \
    TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR(_fmtidx, \
                                     _numfmtdesc, \
                                     TUD_VIDEO_GUID_M420, \
                                     12, \
                                     _frmidx, \
                                     _asrx, \
                                     _asry, \
                                     _interlace, \
                                     _cp)
#define TUD_VIDEO_DESC_CS_VS_FMT_I420( \
    _fmtidx, _numfmtdesc, _frmidx, _asrx, _asry, _interlace, _cp) \
    TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR(_fmtidx, \
                                     _numfmtdesc, \
                                     TUD_VIDEO_GUID_I420, \
                                     12, \
                                     _frmidx, \
                                     _asrx, \
                                     _asry, \
                                     _interlace, \
                                     _cp)

/* TinyUSB's variadic discrete-frame macro counts the four expanded bytes from
 * U32_TO_U8S_LE() as four intervals on this SDK version.  Keep the one-interval
 * descriptor explicit so bLength=30 and bFrameIntervalType=1 on the wire. */
#define TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_DISC_1( \
    _frmidx, _cap, _width, _height, _minbr, _maxbr, _maxfrmbufsz, _interval) \
    (TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_DISC_LEN + 4), \
        TUSB_DESC_CS_INTERFACE, VIDEO_CS_ITF_VS_FRAME_UNCOMPRESSED, \
        _frmidx, _cap, U16_TO_U8S_LE(_width), U16_TO_U8S_LE(_height), \
        U32_TO_U8S_LE(_minbr), U32_TO_U8S_LE(_maxbr), \
        U32_TO_U8S_LE(_maxfrmbufsz), U32_TO_U8S_LE(_interval), \
        1, U32_TO_U8S_LE(_interval)

#define TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_DISC_1( \
    _frmidx, _cap, _width, _height, _minbr, _maxbr, _maxfrmbufsz, _interval) \
    (TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_DISC_LEN + 4), \
        TUSB_DESC_CS_INTERFACE, VIDEO_CS_ITF_VS_FRAME_MJPEG, \
        _frmidx, _cap, U16_TO_U8S_LE(_width), U16_TO_U8S_LE(_height), \
        U32_TO_U8S_LE(_minbr), U32_TO_U8S_LE(_maxbr), \
        U32_TO_U8S_LE(_maxfrmbufsz), U32_TO_U8S_LE(_interval), \
        1, U32_TO_U8S_LE(_interval)

/* TinyUSB's generic variadic input-header macro cannot express two formats
 * with one bmaControls byte each without over-counting bLength. */
#define TUD_VIDEO_DESC_CS_VS_INPUT_2_FORMATS( \
    _totallen, _ep, _inf, _termlnk, _sticaptmeth, _trgspt, _trgusg, _ctl1, _ctl2) \
    (TUD_VIDEO_DESC_CS_VS_IN_LEN + 2), TUSB_DESC_CS_INTERFACE, \
        VIDEO_CS_ITF_VS_INPUT_HEADER, 2, \
        U16_TO_U8S_LE((_totallen) + TUD_VIDEO_DESC_CS_VS_IN_LEN + 2), \
        _ep, _inf, _termlnk, _sticaptmeth, _trgspt, _trgusg, \
        1, _ctl1, _ctl2

#define TUD_VIDEO_DESC_STD_VC_INT_EP(_ep, _epsize, _interval) \
    TUD_VIDEO_DESC_STD_VC_INT_EP_LEN, TUSB_DESC_ENDPOINT, _ep, \
        TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(_epsize), _interval

#define TUD_VIDEO_DESC_CS_VC_INT_EP(_max_transfer_size) \
    TUD_VIDEO_DESC_CS_VC_INT_EP_LEN, TUSB_DESC_CS_ENDPOINT, \
        VIDEO_CS_EP_INTERRUPT, U16_TO_U8S_LE(_max_transfer_size)

#define TUD_VIDEO_CAPTURE_DESCRIPTOR_UNCOMPR( \
    _stridx, _epin, _width, _height, _fps, _epsize) \
    TUD_VIDEO_DESC_IAD(ITF_NUM_VIDEO_CONTROL, \
                       /* 2 Interfaces */ 0x02, \
                       _stridx), /* Video control 0 */ \
        TUD_VIDEO_DESC_STD_VC_UVC11(ITF_NUM_VIDEO_CONTROL, 0, _stridx), \
        TUD_VIDEO_DESC_CS_VC( \
            0x0110, /* wTotalLength - bLength */ \
            TUD_VIDEO_DESC_CAMERA_TERM_LEN + TUD_VIDEO_DESC_OUTPUT_TERM_LEN, \
            UVC_CLOCK_FREQUENCY, \
            ITF_NUM_VIDEO_STREAMING), \
        TUD_VIDEO_DESC_CAMERA_TERM(UVC_ENTITY_CAP_INPUT_TERMINAL, \
                                   0, \
                                   0, \
                                   /*wObjectiveFocalLengthMin*/ 0, \
                                   /*wObjectiveFocalLengthMax*/ 0, \
                                   /*wObjectiveFocalLength*/ 0, \
                                   /*bmControls*/ 0), \
        TUD_VIDEO_DESC_OUTPUT_TERM(UVC_ENTITY_CAP_OUTPUT_TERMINAL, \
                                   VIDEO_TT_STREAMING, \
                                   0, \
                                   1, \
                                   0), /* Video stream alt. 0 */ \
        TUD_VIDEO_DESC_STD_VS_UVC11(ITF_NUM_VIDEO_STREAMING, \
                                    0, \
                                    0, \
                                    _stridx), /* Video stream header for without \
                                           still image capture */ \
        TUD_VIDEO_DESC_CS_VS_INPUT( \
            /*bNumFormats*/ 1, /*wTotalLength - bLength */ \
            TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR_LEN \
                + TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_CONT_LEN \
                + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN, \
            _epin, \
            /*bmInfo*/ 0, \
            /*bTerminalLink*/ UVC_ENTITY_CAP_OUTPUT_TERMINAL, \
            /*bStillCaptureMethod*/ 0, \
            /*bTriggerSupport*/ 0, \
            /*bTriggerUsage*/ 0, \
            /*bmaControls(1)*/ 0), /* Video stream format */ \
        TUD_VIDEO_DESC_CS_VS_FMT_YUY2( \
            /*bFormatIndex*/ 1, \
            /*bNumFrameDescriptors*/ 1, \
            /*bDefaultFrameIndex*/ 1, \
            0, \
            0, \
            0, \
            /*bCopyProtect*/ 0), /* Video stream frame format */ \
        TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_CONT(/*bFrameIndex */ 1, \
                                              0, \
                                              _width, \
                                              _height, \
                                              _width* _height * 16, \
                                              _width * _height * 16 * _fps, \
                                              _width * _height * 16 / 8, \
                                              (10000000 / _fps), \
                                              (10000000 / _fps), \
                                              (10000000 / _fps) * _fps, \
                                              (10000000 / _fps)), \
        TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING( \
            VIDEO_COLOR_PRIMARIES_BT709, \
            VIDEO_COLOR_XFER_CH_BT709, \
            VIDEO_COLOR_COEF_SMPTE170M), /* VS alt 1 */ \
        TUD_VIDEO_DESC_STD_VS_UVC11( \
            ITF_NUM_VIDEO_STREAMING, 1, 1, _stridx), /* EP */ \
        TUD_VIDEO_DESC_EP_ISO(_epin, _epsize, 1)

#define TUD_VIDEO_CAPTURE_DESCRIPTOR_UNCOMPR_BULK( \
    _stridx, _epin, _width, _height, _fps, _epsize) \
    TUD_VIDEO_DESC_IAD(ITF_NUM_VIDEO_CONTROL, \
                       /* 2 Interfaces */ 0x02, \
                       _stridx), /* Video control 0 */ \
        TUD_VIDEO_DESC_STD_VC_UVC11(ITF_NUM_VIDEO_CONTROL, 0, _stridx), \
        TUD_VIDEO_DESC_CS_VC( \
            0x0110, /* wTotalLength - bLength */ \
            TUD_VIDEO_DESC_CAMERA_TERM_LEN + TUD_VIDEO_DESC_OUTPUT_TERM_LEN, \
            UVC_CLOCK_FREQUENCY, \
            ITF_NUM_VIDEO_STREAMING), \
        TUD_VIDEO_DESC_CAMERA_TERM(UVC_ENTITY_CAP_INPUT_TERMINAL, \
                                   0, \
                                   0, \
                                   /*wObjectiveFocalLengthMin*/ 0, \
                                   /*wObjectiveFocalLengthMax*/ 0, \
                                   /*wObjectiveFocalLength*/ 0, \
                                   /*bmControls*/ 0), \
        TUD_VIDEO_DESC_OUTPUT_TERM(UVC_ENTITY_CAP_OUTPUT_TERMINAL, \
                                   VIDEO_TT_STREAMING, \
                                   0, \
                                   1, \
                                   0), \
        /* Video stream alt. 0 */ \
        TUD_VIDEO_DESC_STD_VS_UVC11(ITF_NUM_VIDEO_STREAMING, \
                                    0, \
                                    1, \
                                    _stridx), /* Video stream header for without \
                                           still image capture */ \
        TUD_VIDEO_DESC_CS_VS_INPUT( \
            /*bNumFormats*/ 1, /*wTotalLength - bLength */ \
            TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR_LEN \
                + (TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_DISC_LEN + 4) \
                + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN, \
            _epin, \
            /*bmInfo*/ 0, \
            /*bTerminalLink*/ UVC_ENTITY_CAP_OUTPUT_TERMINAL, \
            /*bStillCaptureMethod*/ 0, \
            /*bTriggerSupport*/ 0, \
            /*bTriggerUsage*/ 0, \
            /*bmaControls(1)*/ 0), /* Video stream format */ \
        TUD_VIDEO_DESC_CS_VS_FMT_YUY2( \
            /*bFormatIndex*/ 1, \
            /*bNumFrameDescriptors*/ 1, \
            /*bDefaultFrameIndex*/ 1, \
            0, \
            0, \
            0, \
            /*bCopyProtect*/ 0), /* Video stream frame format */ \
        TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_DISC_1( \
            /*bFrameIndex */ 1, \
            0, \
            _width, \
            _height, \
            _width * _height * 16 * _fps, \
            _width * _height * 16 * _fps, \
            _width * _height * 16 / 8, \
            (10000000 / _fps)), \
        TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING( \
            VIDEO_COLOR_PRIMARIES_BT709, \
            VIDEO_COLOR_XFER_CH_BT709, \
            VIDEO_COLOR_COEF_SMPTE170M), \
        TUD_VIDEO_DESC_EP_BULK(_epin, 64, 1)

#define TUD_VIDEO_CAPTURE_DESCRIPTOR_MJPEG( \
    _stridx, _epin, _width, _height, _fps, _epsize) \
    TUD_VIDEO_DESC_IAD(ITF_NUM_VIDEO_CONTROL, \
                       /* 2 Interfaces */ 0x02, \
                       _stridx), /* Video control 0 */ \
        TUD_VIDEO_DESC_STD_VC_UVC11(ITF_NUM_VIDEO_CONTROL, 0, _stridx), \
        TUD_VIDEO_DESC_CS_VC( \
            0x0110, /* wTotalLength - bLength */ \
            TUD_VIDEO_DESC_CAMERA_TERM_LEN + TUD_VIDEO_DESC_OUTPUT_TERM_LEN, \
            UVC_CLOCK_FREQUENCY, \
            ITF_NUM_VIDEO_STREAMING), \
        TUD_VIDEO_DESC_CAMERA_TERM(UVC_ENTITY_CAP_INPUT_TERMINAL, \
                                   0, \
                                   0, \
                                   /*wObjectiveFocalLengthMin*/ 0, \
                                   /*wObjectiveFocalLengthMax*/ 0, \
                                   /*wObjectiveFocalLength*/ 0, \
                                   /*bmControls*/ 0), \
        TUD_VIDEO_DESC_OUTPUT_TERM(UVC_ENTITY_CAP_OUTPUT_TERMINAL, \
                                   VIDEO_TT_STREAMING, \
                                   0, \
                                   1, \
                                   0), /* Video stream alt. 0 */ \
        TUD_VIDEO_DESC_STD_VS_UVC11(ITF_NUM_VIDEO_STREAMING, \
                                    0, \
                                    0, \
                                    _stridx), /* Video stream header for without \
                                           still image capture */ \
        TUD_VIDEO_DESC_CS_VS_INPUT( \
            /*bNumFormats*/ 1, /*wTotalLength - bLength */ \
            TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN \
                + TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_LEN \
                + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN, \
            _epin, \
            /*bmInfo*/ 0, \
            /*bTerminalLink*/ UVC_ENTITY_CAP_OUTPUT_TERMINAL, \
            /*bStillCaptureMethod*/ 0, \
            /*bTriggerSupport*/ 0, \
            /*bTriggerUsage*/ 0, \
            /*bmaControls(1)*/ 0), /* Video stream format */ \
        TUD_VIDEO_DESC_CS_VS_FMT_MJPEG( \
            /*bFormatIndex*/ 1, \
            /*bNumFrameDescriptors*/ 1, \
            /*bmFlags*/ 0, \
            /*bDefaultFrameIndex*/ 1, \
            0, \
            0, \
            0, \
            /*bCopyProtect*/ 0), /* Video stream frame format */ \
        TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT(/*bFrameIndex */ 1, \
                                            0, \
                                            _width, \
                                            _height, \
                                            _width* _height * 16, \
                                            _width * _height * 16 * _fps, \
                                            _width * _height * 16 / 8, \
                                            (10000000 / _fps), \
                                            (10000000 / _fps), \
                                            (10000000 / _fps) * _fps, \
                                            (10000000 / _fps)), \
        TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING( \
            VIDEO_COLOR_PRIMARIES_BT709, \
            VIDEO_COLOR_XFER_CH_BT709, \
            VIDEO_COLOR_COEF_SMPTE170M), /* VS alt 1 */ \
        TUD_VIDEO_DESC_STD_VS_UVC11( \
            ITF_NUM_VIDEO_STREAMING, 1, 1, _stridx), /* EP */ \
        TUD_VIDEO_DESC_EP_ISO(_epin, _epsize, 1)

#define TUD_VIDEO_CAPTURE_DESCRIPTOR_UNCOMPR_MJPEG_BULK( \
    _stridx, _epctl, _epin, _width, _height, _fps) \
    TUD_VIDEO_DESC_IAD(ITF_NUM_VIDEO_CONTROL, \
                       /* 2 Interfaces */ 0x02, \
                       _stridx), \
        TUD_VIDEO_DESC_STD_VC_UVC11(ITF_NUM_VIDEO_CONTROL, 1, _stridx), \
        TUD_VIDEO_DESC_CS_VC( \
            0x0110, \
            TUD_VIDEO_DESC_CAMERA_TERM_LEN + TUD_VIDEO_DESC_OUTPUT_TERM_LEN, \
            UVC_CLOCK_FREQUENCY, \
            ITF_NUM_VIDEO_STREAMING), \
        TUD_VIDEO_DESC_CAMERA_TERM(UVC_ENTITY_CAP_INPUT_TERMINAL, \
                                   0, \
                                   0, \
                                   0, \
                                   0, \
                                   0, \
                                   0), \
        TUD_VIDEO_DESC_OUTPUT_TERM(UVC_ENTITY_CAP_OUTPUT_TERMINAL, \
                                   VIDEO_TT_STREAMING, \
                                   0, \
                                   1, \
                                   0), \
        TUD_VIDEO_DESC_STD_VC_INT_EP(_epctl, \
                                     UVC_STATUS_MAX_PACKET_SIZE, \
                                     UVC_STATUS_INTERVAL), \
        TUD_VIDEO_DESC_CS_VC_INT_EP(UVC_STATUS_MAX_PACKET_SIZE), \
        TUD_VIDEO_DESC_STD_VS_UVC11(ITF_NUM_VIDEO_STREAMING, \
                                    0, \
                                    1, \
                                    _stridx), \
        TUD_VIDEO_DESC_CS_VS_INPUT_2_FORMATS( \
            TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR_LEN \
                + (TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_DISC_LEN + 4) \
                + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN \
                + TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN \
                + (TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_DISC_LEN + 4) \
                + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN, \
            _epin, \
            0, \
            UVC_ENTITY_CAP_OUTPUT_TERMINAL, \
            0, \
            0, \
            0, \
            0, \
            0), \
        TUD_VIDEO_DESC_CS_VS_FMT_YUY2( \
            UVC_FORMAT_INDEX_YUY2, \
            1, \
            1, \
            0, \
            0, \
            0, \
            0), \
        TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_DISC_1( \
            1, \
            0, \
            _width, \
            _height, \
            _width * _height * 16 * _fps, \
            _width * _height * 16 * _fps, \
            _width * _height * 16 / 8, \
            (10000000 / _fps)), \
        TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING( \
            VIDEO_COLOR_PRIMARIES_BT709, \
            VIDEO_COLOR_XFER_CH_BT709, \
            VIDEO_COLOR_COEF_SMPTE170M), \
        TUD_VIDEO_DESC_CS_VS_FMT_MJPEG( \
            UVC_FORMAT_INDEX_MJPEG_TEST, \
            1, \
            0, \
            1, \
            0, \
            0, \
            0, \
            0), \
        TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_DISC_1( \
            1, \
            0, \
            UVC_MJPEG_TEST_WIDTH, \
            UVC_MJPEG_TEST_HEIGHT, \
            UVC_MJPEG_TEST_WIDTH * UVC_MJPEG_TEST_HEIGHT * 16 \
                * UVC_MJPEG_TEST_RATE, \
            UVC_MJPEG_TEST_WIDTH * UVC_MJPEG_TEST_HEIGHT * 16 \
                * UVC_MJPEG_TEST_RATE, \
            UVC_MJPEG_TEST_MAX_FRAME_SIZE, \
            (10000000 / UVC_MJPEG_TEST_RATE)), \
        TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING( \
            VIDEO_COLOR_PRIMARIES_BT709, \
            VIDEO_COLOR_XFER_CH_BT709, \
            VIDEO_COLOR_COEF_SMPTE170M), \
        TUD_VIDEO_DESC_EP_BULK(_epin, 64, 1)

#endif
