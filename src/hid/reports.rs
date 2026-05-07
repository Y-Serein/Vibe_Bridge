use serde::{Deserialize, Serialize};

pub const VIBE_KEYBOARD_USAGE_PAGE: u16 = 0xFF60;
pub const VIBE_KEYBOARD_USAGE: u16 = 0x01;

pub const REPORT_ID_INPUT: u8 = 0x10;
pub const REPORT_ID_OUTPUT: u8 = 0x20;
pub const REPORT_ID_ACK: u8 = 0x21;
#[allow(dead_code)]
pub const REPORT_ID_FEATURE: u8 = 0x30;

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct InputReport {
    pub key0: bool,
    pub key1: bool,
    pub key2: bool,
    pub key3: bool,
    pub key4: bool,
    pub key5: bool,
    pub key6: bool,
    pub knob_press: bool,
    pub encoder_delta: i8,
}

impl InputReport {
    pub fn parse(data: &[u8]) -> Option<Self> {
        if data.len() < 3 || data[0] != REPORT_ID_INPUT {
            return None;
        }

        let keys_byte = data[1];
        let encoder_byte = data[2];

        Some(InputReport {
            key0: (keys_byte & 0x01) != 0,
            key1: (keys_byte & 0x02) != 0,
            key2: (keys_byte & 0x04) != 0,
            key3: (keys_byte & 0x08) != 0,
            key4: (keys_byte & 0x10) != 0,
            key5: (keys_byte & 0x20) != 0,
            key6: (keys_byte & 0x40) != 0,
            knob_press: (keys_byte & 0x80) != 0,
            encoder_delta: encoder_byte as i8,
        })
    }
}

#[allow(dead_code)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ScreenCmd {
    Clear = 0x01,
    Write = 0x02,
    SetCursor = 0x03,
    Newline = 0x04,
    Backspace = 0x05,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AckStatus {
    Ok = 0x00,
    Busy = 0x01,
    Err = 0x02,
}

impl AckStatus {
    pub fn from_u8(v: u8) -> Self {
        match v {
            0x00 => AckStatus::Ok,
            0x01 => AckStatus::Busy,
            _ => AckStatus::Err,
        }
    }
}

pub struct AckReport {
    pub seq: u8,
    pub status: AckStatus,
}

impl AckReport {
    pub fn parse(data: &[u8]) -> Option<Self> {
        if data.len() < 3 || data[0] != REPORT_ID_ACK {
            return None;
        }
        Some(AckReport {
            seq: data[1],
            status: AckStatus::from_u8(data[2]),
        })
    }
}
