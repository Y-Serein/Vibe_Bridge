use clap::Parser;
use minifb::{Key, Window, WindowOptions};
use std::time::{Duration, Instant};

#[allow(dead_code)]
struct ImagePacketDecoder {
    width: usize,
    height: usize,
    buf: Vec<u8>,
    addr: usize,
    waiting_header: bool,
}

impl ImagePacketDecoder {
    fn new(width: usize, height: usize) -> Self {
        Self {
            width,
            height,
            buf: vec![0u8; width * height],
            addr: 0,
            waiting_header: true,
        }
    }

    fn reset(&mut self) {
        self.addr = 0;
        self.waiting_header = true;
    }

    fn feed_byte(&mut self, byte: u8) -> Option<Vec<u8>> {
        if self.waiting_header {
            if byte == 0xFF {
                self.waiting_header = false;
            }
            return None;
        }

        self.buf[self.addr] = byte;
        self.addr += 1;

        if self.addr >= self.buf.len() {
            let img = self.buf.clone();
            self.reset();
            return Some(img);
        }

        None
    }
}

fn gray_to_rgb(gray: &[u8], rgb: &mut [u32]) {
    for (i, &g) in gray.iter().enumerate() {
        let v = g as u32;
        rgb[i] = (v << 16) | (v << 8) | v;
    }
}

#[derive(Parser, Debug)]
#[command(author, version, about)]
struct Args {
    /// Uart port, like "COM1" or /dev/ttyUSB0
    #[arg(short, long)]
    port: String,

    /// Uart baudrate
    #[arg(short, long)]
    baudrate: u32,

    /// width
    #[arg(short, long)]
    width: u32,

    /// height
    #[arg(short, long)]
    height: u32,
}

fn main() -> anyhow::Result<()> {
    let args = Args::parse();
    let port_name = &args.port;
    let baud = args.baudrate;
    let width = args.width as usize;
    let height = args.height as usize;

    let mut port = serialport::new(port_name, baud)
        .timeout(Duration::from_millis(10))
        .open()
        .expect("Failed to open serial port");

    let mut decoder = ImagePacketDecoder::new(width, height);

    let mut window = Window::new(
        "UART Image Viewer",
        width,
        height,
        WindowOptions {
            resize: false,
            scale: minifb::Scale::X2,
            ..WindowOptions::default()
        },
    )?;

    let mut rgb_buf = vec![0u32; width * height];

    let mut frame_count: u64 = 0;
    let mut last_time = Instant::now();

    let mut byte = [0u8; 1];

    println!("Waiting for frames...");

    while window.is_open() && !window.is_key_down(Key::Escape) {
        match port.read(&mut byte) {
            Ok(1) => {
                if let Some(img) = decoder.feed_byte(byte[0]) {
                    frame_count += 1;

                    let now = Instant::now();
                    let dt = now.duration_since(last_time).as_secs_f32();
                    last_time = now;

                    let fps = if dt > 0.0 { 1.0 / dt } else { 0.0 };

                    gray_to_rgb(&img, &mut rgb_buf);

                    window.update_with_buffer(&rgb_buf, width, height).unwrap();

                    window.set_title(&format!("#{frame_count} | {:.2}", fps));
                }
            }
            _ => {
                // timeout or no data, let UI breathe
                std::thread::sleep(Duration::from_millis(1));
            }
        }
    }

    Ok(())
}
